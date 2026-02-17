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

// ================= CONFIGURATION =================
#define APN      "mcinet\0"        // mcinet یا mtnirancell
#define USERID   ""
#define PASSWD   ""

// توکن خود را اینجا چک کنید
#define DEVICE_ACCESS_TOKEN  "FxVYefTdmZpOWZ4DNIsf" 

#define HOST_NAME             "thingsboard.cloud"
#define HOST_PORT             1883
#define MQTT_TOPIC            "v1/devices/me/telemetry\0"

// ================= DEBUG CONFIG =================
#define DEBUG_PORT  UART_PORT2
static char DBG_BUFFER[512]; 

#define APP_DEBUG(FORMAT,...) {\
    Ql_memset(DBG_BUFFER, 0, 512);\
    Ql_sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8*)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
}

// ================= STATE MACHINE =================
typedef enum{
    STATE_NW_QUERY_STATE,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_MQTT_PUB,
    STATE_MQTT_WAIT,
    STATE_IDLE
} Enum_MQTT_STATE;
static u8 m_mqtt_state = STATE_NW_QUERY_STATE;

// ================= VARIABLES =================
#define MQTT_TIMER_ID       0x200
#define MQTT_TIMER_PERIOD   1000  // تایمر هر 1 ثانیه چک میشود

Enum_ConnectID connect_id = ConnectID_0;
u32 pub_message_id = 0;
u32 wait_tick = 0;
u32 temp_val = 20; // دمای اولیه

u8 clientID[] = "MC60_Device\0"; 
u8 username[] = DEVICE_ACCESS_TOKEN;
u8 passwd[] = "";
u8 mqtt_payload[128];
u8 topic[] = MQTT_TOPIC;

// Forward Declarations
static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara);
static void Callback_Timer(u32 timerId, void* param);
static void mqtt_recv(u8* buffer,u32 length);

// ================= MAIN TASK =================
void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    Ql_UART_Register(DEBUG_PORT, CallBack_UART_Hdlr, NULL);
    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);
    Ql_Sleep(1000); 

    APP_DEBUG("\r\n<--- SYSTEM RESTART --->\r\n");

    Ql_Timer_Register(MQTT_TIMER_ID, Callback_Timer, NULL);
    Ql_Mqtt_Recv_Register(mqtt_recv);

    while(TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            case MSG_ID_RIL_READY:
                APP_DEBUG("<RIL READY> Initializing...\r\n");
                Ql_RIL_Initialize(); 
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
                            APP_DEBUG("MQTT Open Fail: %d (Retrying...)\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                            m_mqtt_state = STATE_MQTT_OPEN; 
                        }
                        break;
                    
                    case URC_MQTT_CONN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Connected: OK\r\n");
                            m_mqtt_state = STATE_MQTT_PUB;
                        } else {
                            APP_DEBUG("MQTT Connect Fail: %d (Retrying...)\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                            m_mqtt_state = STATE_MQTT_OPEN;
                        }
                        break;

                    case URC_MQTT_PUB: // <--- اینجا نتیجه ارسال میاید
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG(">> Data Sent Successfully! (ACK Received)\r\n");
                        } else {
                            APP_DEBUG(">> Send Failed! Error: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                        }
                        // چه موفق چه ناموفق، برو به حالت انتظار برای ارسال بعدی
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
            
            // نمایش وضعیت شبکه فقط تا زمان اتصال
            static u8 log_limit = 0;
            if (log_limit < 20 || cgreg == 1 || cgreg == 5) {
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
                } else {
                    APP_DEBUG("GPRS Attach Failed: %d\r\n", ret);
                }
            }
            break;
        }
        case STATE_MQTT_CFG:
            APP_DEBUG("Configuring MQTT...\r\n");
            RIL_MQTT_QMTCFG_Showrecvlen(connect_id,ShowFlag_1);
            RIL_MQTT_QMTCFG_Version_Select(connect_id,Version_3_1_1);
            m_mqtt_state = STATE_MQTT_OPEN;
            break;

        case STATE_MQTT_OPEN:
            ret = RIL_MQTT_QMTOPEN(connect_id, HOST_NAME, HOST_PORT);
            if(ret == RIL_AT_SUCCESS) {
                 APP_DEBUG("Opening Connection...\r\n");
            } else {
                 APP_DEBUG("Cmd Open Failed: %d\r\n", ret);
            }
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_CONN:
            APP_DEBUG("Sending Login Packet...\r\n");
            ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_PUB:
            // ساخت داده متغیر برای تست
            temp_val++; 
            if(temp_val > 50) temp_val = 20;

            Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
            // فرمت دقیق تینگزبورد
            Ql_sprintf((char*)mqtt_payload, "{\"temp\":%d, \"status\":\"live\"}", temp_val);
            
            APP_DEBUG("Publishing: %s ...\r\n", mqtt_payload);
            pub_message_id++;
            
            // پارامتر دوم (msgID) باید غیر صفر باشد برای QoS=1
            // پارامتر سوم (QoS) 0 یا 1. برای اطمینان 1 میگذاریم
            // پارامتر چهارم (Retain) معمولا 0
            ret = RIL_MQTT_QMTPUB(connect_id, pub_message_id, 1, 0, topic, Ql_strlen((char*)mqtt_payload), mqtt_payload);
            
            if (ret == RIL_AT_SUCCESS) {
                // اگر دستور با موفقیت به ماژول داده شد
                // منتظر URC میمانیم (URC_MQTT_PUB)
                m_mqtt_state = STATE_IDLE; 
            } else {
                // اگر خود دستور خطا داد (مثلا بافر پر است)
                APP_DEBUG("Publish Command Error: %d\r\n", ret);
                m_mqtt_state = STATE_MQTT_WAIT; // صبر میکنیم و دوباره تلاش میکنیم
            }
            break;

        case STATE_MQTT_WAIT:
            // شمارش معکوس برای ارسال بعدی
            wait_tick++;
            if(wait_tick >= 5) { // هر 5 ثانیه (چون تایمر 1 ثانیه است)
                wait_tick = 0;
                APP_DEBUG("Looping -> Next Pub\r\n");
                m_mqtt_state = STATE_MQTT_PUB;
            }
            break;
            
        case STATE_IDLE:
            break;
    }
}

static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara) {}
static void mqtt_recv(u8* buffer,u32 length) { APP_DEBUG("Recv from Server: %s\r\n", buffer); }
