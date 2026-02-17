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
#define MQTT_TOPIC_PUB        "v1/devices/me/telemetry\0"
#define MQTT_TOPIC_SUB        "v1/devices/me/attributes\0"

// ================= DEBUG =================
#define DEBUG_PORT  UART_PORT2
static char DBG_BUFFER[512];

#define APP_DEBUG(FORMAT,...) {\
    Ql_memset(DBG_BUFFER, 0, 512);\
    Ql_sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8*)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
}

// ================= GLOBAL VARS =================
static char RMC_BUF[512]; 
float g_lat = 0.0;
float g_lon = 0.0;
float g_speed = 0.0; // km/h
bool  g_gps_powered = FALSE;

// --- New Vars for Logic ---
float g_pet_weight = 5.0; // Default weight (kg)
int   g_mode = 3;         // Default: 3 (10 seconds)
float g_total_calories = 0.0;

// *** BUG FIX: Manual Timer Counter instead of Ql_GetTickCount ***
u32   g_uptime_sec = 0;      // Counts total seconds since boot
u32   g_last_pub_time = 0;   // Stores the time of last publish

// ================= STATE MACHINE =================
typedef enum{
    STATE_NW_QUERY_STATE,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_MQTT_SUB,    
    STATE_GPS_CHECK,    
    STATE_MQTT_PUB,
    STATE_MQTT_WAIT,
    STATE_IDLE
} Enum_MQTT_STATE;

static u8 m_mqtt_state = STATE_NW_QUERY_STATE;

Enum_ConnectID connect_id = ConnectID_0;
u32 msg_id = 0;
u32 gps_wait_counter = 0; 
u32 loop_wait_tick = 0;   

u8 clientID[] = "MC60_Tracker\0";
u8 username[] = DEVICE_ACCESS_TOKEN;
u8 passwd[] = "";
u8 mqtt_payload[256];

#define MQTT_TIMER_ID         0x200
#define MQTT_TIMER_PERIOD     1000  // 1 Second Tick

// ================= FORWARD DECLARATIONS =================
static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara);
static void Callback_Timer(u32 timerId, void* param);
static void mqtt_recv(u8* buffer,u32 length);
static bool ParseRMC_Safe(char* rmc, float* lat, float* lon, float* speed);
static void GPS_Power_Control(bool on);
static int  Get_Battery_Level(void);
static void Calculate_Calories(float speed_kmh, float weight_kg, int duration_sec);
static void Parse_Attribute_Update(char* json_str);

// ================= MAIN TASK =================
void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    // 1. Init Debug UART
    Ql_UART_Register(DEBUG_PORT, CallBack_UART_Hdlr, NULL);
    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);
    Ql_Sleep(1000);
    APP_DEBUG("\r\n<--- GPS TRACKER: FIXED BUILD --->\r\n");

    // 2. Register Timer & MQTT
    Ql_Timer_Register(MQTT_TIMER_ID, Callback_Timer, NULL);
    Ql_Mqtt_Recv_Register(mqtt_recv);

    while(TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            case MSG_ID_RIL_READY:
                APP_DEBUG("<RIL READY> Core Initialized.\r\n");
                Ql_RIL_Initialize();
                break;

            case MSG_ID_URC_INDICATION:
                switch (msg.param1)
                {
                    case URC_SIM_CARD_STATE_IND:
                        if(SIM_STAT_READY == msg.param2) {
                            APP_DEBUG("SIM Ready -> Starting Timer\r\n");
                            Ql_Timer_Start(MQTT_TIMER_ID, MQTT_TIMER_PERIOD, TRUE);
                        }
                        break;

                    case URC_MQTT_OPEN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Open: OK\r\n");
                            m_mqtt_state = STATE_MQTT_CONN;
                        } else {
                            m_mqtt_state = STATE_MQTT_OPEN;
                        }
                        break;

                    case URC_MQTT_CONN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Connected: OK\r\n");
                            m_mqtt_state = STATE_MQTT_SUB; // Go to Subscribe first
                        } else {
                            m_mqtt_state = STATE_MQTT_OPEN;
                        }
                        break;

                    case URC_MQTT_SUB:
                         APP_DEBUG("Subscribed to Attributes.\r\n");
                         // Enable GPS only after full MQTT setup
                         if(!g_gps_powered) {
                             GPS_Power_Control(TRUE);
                         }
                         m_mqtt_state = STATE_GPS_CHECK;
                         break;

                    case URC_MQTT_PUB:
                        APP_DEBUG(">> Data Sent (ACK)\r\n");
                        loop_wait_tick = 0;
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

    // *** FIX: Increment manual ticker (1 sec period) ***
    g_uptime_sec++;

    switch(m_mqtt_state)
    {
        case STATE_NW_QUERY_STATE:
        {
            s32 cgreg = 0;
            RIL_NW_GetGPRSState(&cgreg);
            if((cgreg == NW_STAT_REGISTERED)||(cgreg == NW_STAT_REGISTERED_ROAMING)) {
                APP_DEBUG("Network Found! Attaching...\r\n");
                RIL_NW_SetGPRSContext(0);
                RIL_NW_SetAPN(1, APN, USERID, PASSWD);
                if(RIL_NW_OpenPDPContext() == RIL_AT_SUCCESS) {
                    m_mqtt_state = STATE_MQTT_CFG;
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
            RIL_MQTT_QMTOPEN(connect_id, HOST_NAME, HOST_PORT);
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_CONN:
            RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_SUB:
        {
            // Subscribe to shared attributes
            ST_MQTT_topic_info_t topic_info;
            topic_info.count = 1;
            topic_info.topic[0] = (u8*)Ql_MEM_Alloc(128);
            Ql_strcpy((char*)topic_info.topic[0], MQTT_TOPIC_SUB);
            topic_info.qos[0] = QOS1_AT_LEASET_ONCE;
            
            msg_id++;
            RIL_MQTT_QMTSUB(connect_id, msg_id, &topic_info);
            Ql_MEM_Free(topic_info.topic[0]);
            
            m_mqtt_state = STATE_IDLE;
            break;
        }

        case STATE_GPS_CHECK:
        {
            if (g_mode == 0) {
                m_mqtt_state = STATE_MQTT_WAIT;
                return;
            }

            gps_wait_counter++;
            bool fix_found = FALSE;
            Ql_memset(RMC_BUF, 0, sizeof(RMC_BUF));
            
            if (RIL_GPS_Read("RMC", RMC_BUF) == RIL_AT_SUCCESS) {
                 if (ParseRMC_Safe(RMC_BUF, &g_lat, &g_lon, &g_speed)) {
                     fix_found = TRUE;
                 }
            }

            if (fix_found) {
                m_mqtt_state = STATE_MQTT_PUB;
            } 
            else {
                if (gps_wait_counter % 5 == 0) APP_DEBUG("Searching GPS (%d)...\r\n", gps_wait_counter);
                if (gps_wait_counter >= 60) {
                    m_mqtt_state = STATE_MQTT_PUB;
                }
            }
            break;
        }

        case STATE_MQTT_PUB:
        {
            int batt = Get_Battery_Level();
            
            // *** FIX: Calculate elapsed time manually ***
            u32 current_sec = g_uptime_sec;
            u32 elapsed_sec = current_sec - g_last_pub_time;
            
            // Safety cap for calorie math
            if (elapsed_sec > 600) elapsed_sec = 600; 
            if (elapsed_sec == 0) elapsed_sec = 1;
            
            g_last_pub_time = current_sec; // Update last time

            Calculate_Calories(g_speed, g_pet_weight, elapsed_sec);

            Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
            
            // Safe JSON formatting
            Ql_sprintf((char*)mqtt_payload, 
                "{\"lat\":%d.%06d, \"lon\":%d.%06d, \"speed\":%d, \"batt\":%d, \"cal\":%d.%02d, \"mode\":%d}", 
                (int)g_lat, (int)((g_lat - (int)g_lat)*1000000),
                (int)g_lon, (int)((g_lon - (int)g_lon)*1000000),
                (int)g_speed, 
                batt,
                (int)g_total_calories, (int)((g_total_calories - (int)g_total_calories)*100),
                g_mode
            );

            APP_DEBUG("PUB: %s\r\n", mqtt_payload);
            msg_id++;
            ret = RIL_MQTT_QMTPUB(connect_id, msg_id, 1, 0, (u8*)MQTT_TOPIC_PUB, Ql_strlen((char*)mqtt_payload), mqtt_payload);

            if (ret == RIL_AT_SUCCESS) m_mqtt_state = STATE_IDLE;
            else {
                 m_mqtt_state = STATE_MQTT_WAIT;
                 loop_wait_tick = 0;
            }
            break;
        }

        case STATE_MQTT_WAIT:
        {
            loop_wait_tick++;
            
            int target_wait = 10; // Default
            if (g_mode == 0) target_wait = 60;   
            if (g_mode == 1) target_wait = 600;  
            if (g_mode == 2) target_wait = 60;   
            if (g_mode == 3) target_wait = 10;   

            if(loop_wait_tick >= target_wait) {
                loop_wait_tick = 0;
                gps_wait_counter = 0;
                if(g_mode == 0) { 
                    APP_DEBUG("Mode 0 (Sleep)...\r\n");
                    loop_wait_tick = 0; 
                } else {
                    m_mqtt_state = STATE_GPS_CHECK; 
                }
            }
            break;
        }

        case STATE_IDLE:
            break;
    }
}

// ================= HELPERS & LOGIC =================

static int Get_Battery_Level(void) {
    u32 batt_mv = 0;
    Ql_GetPowerSupply(&batt_mv); 
    if(batt_mv >= 4200) return 100;
    if(batt_mv <= 3400) return 0;
    return (int)((batt_mv - 3400) * 100 / 800);
}

static void Calculate_Calories(float speed_kmh, float weight_kg, int duration_sec) {
    float met = 1.0; 
    if (speed_kmh > 1.0 && speed_kmh <= 6.0) met = 3.5; 
    else if (speed_kmh > 6.0) met = 6.0; 
    
    float time_hours = (float)duration_sec / 3600.0;
    float burned = met * weight_kg * time_hours;
    
    g_total_calories += burned;
}

static void GPS_Power_Control(bool on) {
    if (on) {
        RIL_GPS_Open(1); 
        g_gps_powered = TRUE;
    } else {
        Ql_RIL_SendATCmd("AT+QGNSSC=0", 11, NULL, NULL, 0);
        g_gps_powered = FALSE;
    }
}

static bool ParseRMC_Safe(char* rmc, float* lat, float* lon, float* speed) {
    char* p = Ql_strstr(rmc, "RMC");
    if(!p) return FALSE;
    int comma_cnt = 0;
    char* ptr = p;
    char* lat_ptr = NULL; char* lon_ptr = NULL; char* spd_ptr = NULL;
    char status = 'V'; char ns = 'N'; char ew = 'E';

    while(*ptr) {
        if(*ptr == ',') {
            comma_cnt++;
            if(comma_cnt == 2) status = *(ptr+1);
            if(comma_cnt == 3) lat_ptr = ptr+1;
            if(comma_cnt == 4) ns = *(ptr+1);
            if(comma_cnt == 5) lon_ptr = ptr+1;
            if(comma_cnt == 6) ew = *(ptr+1);
            if(comma_cnt == 7) spd_ptr = ptr+1;
        }
        ptr++;
    }
    if(status != 'A') return FALSE;
    if(lat_ptr && lon_ptr) {
        float raw_lat = Ql_atof(lat_ptr);
        int lat_d = (int)(raw_lat / 100);
        *lat = lat_d + (raw_lat - lat_d*100)/60.0;
        if(ns == 'S') *lat = -(*lat);
        float raw_lon = Ql_atof(lon_ptr);
        int lon_d = (int)(raw_lon / 100);
        *lon = lon_d + (raw_lon - lon_d*100)/60.0;
        if(ew == 'W') *lon = -(*lon);
        if(spd_ptr) *speed = Ql_atof(spd_ptr) * 1.852; 
        return TRUE;
    }
    return FALSE;
}

static void Parse_Attribute_Update(char* json_str) {
    APP_DEBUG("Parsing Update: %s\r\n", json_str);
    char* p = Ql_strstr(json_str, "pet_weight");
    if(p) {
        char* val_ptr = Ql_strstr(p, ":");
        if(val_ptr) {
            g_pet_weight = Ql_atof(val_ptr + 1);
            APP_DEBUG("NEW WEIGHT: %.2f\r\n", g_pet_weight);
        }
    }
    p = Ql_strstr(json_str, "mode");
    if(p) {
        char* val_ptr = Ql_strstr(p, ":");
        if(val_ptr) {
            g_mode = Ql_atoi(val_ptr + 1);
            APP_DEBUG("NEW MODE: %d\r\n", g_mode);
        }
    }
}

static void mqtt_recv(u8* buffer, u32 length) {
    APP_DEBUG("RX MQTT: %s\r\n", buffer);
    Parse_Attribute_Update((char*)buffer);
}

static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara) {}
