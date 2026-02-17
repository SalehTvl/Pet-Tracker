#include "custom_feature_def.h"
#include "ql_stdlib.h"
#include "ql_common.h"
#include "ql_system.h"
#include "ql_type.h"
#include "ql_trace.h"
#include "ql_error.h"
#include "ql_uart.h"
#include "ql_timer.h"
#include "ril_network.h"
#include "ril_mqtt.h"
#include "ril.h"
#include "ril_util.h"
#include "ril_system.h"
#include "ril_gps.h" 

// ================= CONFIGURATION =================
#define APN      "mcinet\0"        
#define USERID   ""
#define PASSWD   ""

#define DEVICE_ACCESS_TOKEN   "FxVYefTdmZpOWZ4DNIsf" 
#define HOST_NAME             "thingsboard.cloud"
#define HOST_PORT             1883

// Topics
#define TOPIC_TELEMETRY       "v1/devices/me/telemetry\0"
#define TOPIC_ATTRIBUTES      "v1/devices/me/attributes\0" 

// Timing
#define MQTT_TIMER_ID         0x200
#define MQTT_TIMER_PERIOD     1000  // 1 Second Tick

// ================= DEBUG =================
#define DEBUG_PORT  UART_PORT2
static char DBG_BUFFER[512]; 

#define APP_DEBUG(FORMAT,...) {\
    Ql_memset(DBG_BUFFER, 0, 512);\
    Ql_sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8*)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
}

// ================= GLOBAL VARS (PET TRACKER) =================
static char RMC_BUF[512];
float g_lat = 0.0;
float g_lon = 0.0;
float g_speed_kmh = 0.0;
float g_pet_weight = 10.0; // Default 10kg
float g_inst_cal = 0.0;

bool  g_gps_power_on = FALSE; // Flag to track GPS state

// ================= STATE MACHINE =================
typedef enum{
    STATE_NW_QUERY_STATE,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_MQTT_SUB,
    STATE_MQTT_PUB,
    STATE_MQTT_WAIT,
    STATE_IDLE
} Enum_MQTT_STATE;

static u8 m_mqtt_state = STATE_NW_QUERY_STATE;

Enum_ConnectID connect_id = ConnectID_0;
u32 pub_message_id = 0;
u32 sub_message_id = 0;
u32 wait_tick = 0;

u8 clientID[] = "MC60_PetTracker\0"; 
u8 username[] = DEVICE_ACCESS_TOKEN;
u8 passwd[] = "";
u8 mqtt_payload[256];

// ================= FORWARD DECLARATIONS =================
static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara);
static void Callback_Timer(u32 timerId, void* param);
static void mqtt_recv(u8* buffer,u32 length);
static bool ParseRMC(char* rmc, float* lat, float* lon, float* speed);
static u8 Get_Battery_Percent(void);
static void Calculate_Calories(float speed);

// ================= MAIN TASK =================
void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    // 1. Init Debug UART
    Ql_UART_Register(DEBUG_PORT, CallBack_UART_Hdlr, NULL);
    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);
    Ql_Sleep(1000); 
    APP_DEBUG("\r\n<--- PET TRACKER FINAL BOOT --->\r\n");

    // 2. Register Timer & MQTT
    Ql_Timer_Register(MQTT_TIMER_ID, Callback_Timer, NULL);
    Ql_Mqtt_Recv_Register(mqtt_recv);

    while(TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            case MSG_ID_RIL_READY:
                APP_DEBUG("<RIL READY> Init Core...\r\n");
                Ql_RIL_Initialize(); 
                // نکته مهم: اینجا GPS را روشن نمیکنیم تا شبکه پیدا شود
                break;

            case MSG_ID_URC_INDICATION:
                switch (msg.param1)
                {
                    case URC_SIM_CARD_STATE_IND:
                        APP_DEBUG("SIM State: %d\r\n", msg.param2);
                        if(SIM_STAT_READY == msg.param2)
                        {
                            APP_DEBUG("SIM Ready -> Starting Timer\r\n");
                            Ql_Timer_Start(MQTT_TIMER_ID, MQTT_TIMER_PERIOD, TRUE);
                        }
                        break;
                    
                    case URC_MQTT_OPEN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Open: OK\r\n");
                            m_mqtt_state = STATE_MQTT_CONN;
                        } else {
                            APP_DEBUG("MQTT Open Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                            m_mqtt_state = STATE_MQTT_OPEN; 
                        }
                        break;
                    
                    case URC_MQTT_CONN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Connected: OK\r\n");
                            m_mqtt_state = STATE_MQTT_SUB; // First Subscribe
                        } else {
                            APP_DEBUG("MQTT Connect Fail\r\n");
                            m_mqtt_state = STATE_MQTT_OPEN;
                        }
                        break;

                    case URC_MQTT_SUB:
                         APP_DEBUG("Subscribe Result: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                         m_mqtt_state = STATE_MQTT_PUB; // Now ready to publish
                         break;

                    case URC_MQTT_PUB: 
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG(">> Data Sent (ACK OK)\r\n");
                        } else {
                            APP_DEBUG(">> Send Failed! Error: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                        }
                        wait_tick = 0;
                        m_mqtt_state = STATE_MQTT_WAIT; 
                        break;
                }
                break;
            default:
                break;
        }
    }
}

// ================= TIMER LOGIC =================
static void Callback_Timer(u32 timerId, void* param)
{
    s32 ret;
    if(MQTT_TIMER_ID != timerId) return;

    switch(m_mqtt_state)
    {
        case STATE_NW_QUERY_STATE:
        {
            s32 cgreg = 0;
            ret = RIL_NW_GetGPRSState(&cgreg);
            
            // جلوگیری از پر شدن لاگ
            static u8 log_limit = 0;
            if (log_limit < 10 || cgreg == 1 || cgreg == 5) {
                APP_DEBUG("Network Check: %d\r\n", cgreg);
                log_limit++;
            }

            if((cgreg == NW_STAT_REGISTERED)||(cgreg == NW_STAT_REGISTERED_ROAMING))
            {
                APP_DEBUG("Network Found! Attaching GPRS...\r\n");
                RIL_NW_SetGPRSContext(0);
                RIL_NW_SetAPN(1, APN, USERID, PASSWD);
                ret = RIL_NW_OpenPDPContext();
                
                if(ret == RIL_AT_SUCCESS) {
                    APP_DEBUG("GPRS Attached Success!\r\n");
                    m_mqtt_state = STATE_MQTT_CFG;
                    
                    // *** استراتژی جدید: روشن کردن GPS اینجا ***
                    // حالا که اینترنت وصل شده، GPS را روشن میکنیم
                    if(!g_gps_power_on) {
                         APP_DEBUG("Turning GPS ON (Safe Mode)...\r\n");
                         RIL_GPS_Open(1);
                         g_gps_power_on = TRUE;
                    }

                }
            }
            break;
        }
        case STATE_MQTT_CFG:
            RIL_MQTT_QMTCFG_Showrecvlen(connect_id,ShowFlag_1);
            RIL_MQTT_QMTCFG_Version_Select(connect_id,Version_3_1_1);
            m_mqtt_state = STATE_MQTT_OPEN;
            break;

        case STATE_MQTT_OPEN:
            ret = RIL_MQTT_QMTOPEN(connect_id, HOST_NAME, HOST_PORT);
            if(ret == RIL_AT_SUCCESS) APP_DEBUG("Opening MQTT...\r\n");
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_CONN:
            APP_DEBUG("Sending Login...\r\n");
            ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_SUB:
        {
            // Subscribe for weight updates
            ST_MQTT_topic_info_t topic_info;
            topic_info.count = 1;
            topic_info.topic[0] = (u8*)Ql_MEM_Alloc(128);
            Ql_strcpy((char*)topic_info.topic[0], TOPIC_ATTRIBUTES);
            topic_info.qos[0] = QOS1_AT_LEASET_ONCE;
            
            sub_message_id++;
            APP_DEBUG("Subscribing to Attributes...\r\n");
            RIL_MQTT_QMTSUB(connect_id, sub_message_id, &topic_info);
            
            Ql_MEM_Free(topic_info.topic[0]);
            m_mqtt_state = STATE_IDLE;
            break;
        }

        case STATE_MQTT_PUB:
            // 1. خواندن GPS
            // حتی اگر GPS هنوز فیکس نشده باشد، برنامه گیر نمیکند
            APP_DEBUG("Reading GPS Data...\r\n");
            Ql_memset(RMC_BUF, 0, sizeof(RMC_BUF));
            if (RIL_AT_SUCCESS == RIL_GPS_Read("RMC", RMC_BUF)) {
                if(ParseRMC(RMC_BUF, &g_lat, &g_lon, &g_speed_kmh)) {
                    // Fix Valid
                }
            }

            // 2. محاسبات
            Calculate_Calories(g_speed_kmh);
            u8 batt = Get_Battery_Percent();

            // 3. آماده‌سازی پکیج
            Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
            Ql_sprintf((char*)mqtt_payload, 
                "{\"lat\":%f, \"lon\":%f, \"speed\":%f, \"batt\":%d, \"inst_cal\":%f}", 
                g_lat, g_lon, g_speed_kmh, batt, g_inst_cal);
            
            APP_DEBUG("PUB: %s\r\n", mqtt_payload);
            pub_message_id++;
            
            ret = RIL_MQTT_QMTPUB(connect_id, pub_message_id, 1, 0, (u8*)TOPIC_TELEMETRY, Ql_strlen((char*)mqtt_payload), mqtt_payload);
            
            if (ret == RIL_AT_SUCCESS) {
                m_mqtt_state = STATE_IDLE; 
            } else {
                APP_DEBUG("Pub Cmd Error: %d\r\n", ret);
                m_mqtt_state = STATE_MQTT_WAIT; 
            }
            break;

        case STATE_MQTT_WAIT:
            wait_tick++;
            if(wait_tick >= 10) { // هر 10 ثانیه ارسال کن
                wait_tick = 0;
                m_mqtt_state = STATE_MQTT_PUB;
            }
            break;
            
        case STATE_IDLE:
            break;
    }
}

// ================= HELPERS =================

static void mqtt_recv(u8* buffer, u32 length) {
    APP_DEBUG("RX: %s\r\n", buffer);
    // نمونه: {"pet_weight":12.5}
    char* p = Ql_strstr((char*)buffer, "pet_weight");
    if(p) {
        char* val_start = Ql_strstr(p, ":");
        if(val_start) {
            val_start++; 
            float new_w = Ql_atof(val_start);
            if(new_w > 0) {
                g_pet_weight = new_w;
                APP_DEBUG(">> Pet Weight Update: %f kg\r\n", g_pet_weight);
            }
        }
    }
}

static u8 Get_Battery_Percent(void) {
    u32 batt_volt_mv;
    Ql_GetPowerSupply(&batt_volt_mv); 
    if(batt_volt_mv >= 4200) return 100;
    if(batt_volt_mv <= 3600) return 0;
    return (u8)((batt_volt_mv - 3600) * 100 / (600));
}

static void Calculate_Calories(float speed) {
    // فرمول: وزن * MET * ساعت
    // بازه زمانی ارسال ما حدود 10 ثانیه است (wait_tick=10 * 1000ms)
    // 10 ثانیه = 0.00277 ساعت
    float time_h = 10.0 / 3600.0;
    float met = 1.0; // استراحت
    if(speed > 1.0) met = 3.5; // راه رفتن
    if(speed > 6.0) met = 6.0; // دویدن
    
    g_inst_cal = g_pet_weight * met * time_h;
}

static bool ParseRMC(char* rmc, float* lat, float* lon, float* speed)
{
    // پارسر ساده RMC
    char* p = Ql_strstr(rmc, "$GPRMC");
    if(!p) p = Ql_strstr(rmc, "$GNRMC");
    if(!p) return FALSE;

    // پیدا کردن کاماها
    char* tokens[13];
    u8 token_idx = 0;
    
    // کپی کردن رشته برای اینکه رشته اصلی خراب نشود (اختیاری ولی امن‌تر)
    // اما اینجا برای سادگی مستقیم روی پوینتر کار میکنیم
    p = Ql_strstr(p, ","); 
    while(p && token_idx < 12) {
        p++; // رد شدن از کاما
        tokens[token_idx++] = p;
        p = Ql_strstr(p, ",");
    }

    /*
      Index 0: Time
      Index 1: Status (A=Active, V=Void)
      Index 2: Lat
      Index 3: N/S
      Index 4: Lon
      Index 5: E/W
      Index 6: Speed (Knots)
    */

    if(token_idx < 7) return FALSE;
    if(tokens[1][0] != 'A') return FALSE; // هنوز فیکس نشده

    // پارس Latitude
    float raw_lat = Ql_atof(tokens[2]);
    int lat_d = (int)(raw_lat / 100);
    *lat = lat_d + (raw_lat - lat_d*100)/60.0;
    if(tokens[3][0] == 'S') *lat = -*lat;

    // پارس Longitude
    float raw_lon = Ql_atof(tokens[4]);
    int lon_d = (int)(raw_lon / 100);
    *lon = lon_d + (raw_lon - lon_d*100)/60.0;
    if(tokens[5][0] == 'W') *lon = -*lon;

    // پارس سرعت (گره به کیلومتر)
    *speed = Ql_atof(tokens[6]) * 1.852;

    return TRUE;
}

static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara) {}
