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
// 1. تنظیمات اپراتور
#define APN      "mcinet"        // mcinet یا mtnirancell
#define USERID   ""
#define PASSWD   ""

// 2. توکن تینگزبورد
#define DEVICE_ACCESS_TOKEN  "FxVYefTdmZpOWZ4DNIsf" 

// 3. سرور
#define HOST_NAME             "thingsboard.cloud"
#define HOST_PORT             1883
#define MQTT_TOPIC            "v1/devices/me/telemetry"

// ================= DEBUG =================
#define DEBUG_PORT  UART_PORT2
static char DBG_BUFFER[512]; // کاهش حجم بافر برای جلوگیری از Stack Overflow

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
#define MQTT_TIMER_PERIOD   1000

Enum_ConnectID connect_id = ConnectID_0;
u32 pub_message_id = 0;

u8 clientID[] = "MC60_Tracker\0"; 
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

    // 1. باز کردن پورت سریال (همان اول کار)
    Ql_UART_Register(DEBUG_PORT, CallBack_UART_Hdlr, NULL);
    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);

    // تاخیر کوچک برای پایداری
    Ql_Sleep(1000); 
    APP_DEBUG("\r\n<--- SYSTEM RESTART --->\r\n");

    // 2. رجیستر کردن تایمر
    Ql_Timer_Register(MQTT_TIMER_ID, Callback_Timer, NULL);
    
    // 3. رجیستر کردن MQTT Recv
    Ql_Mqtt_Recv_Register(mqtt_recv);

    // نکته مهم: اینجا هنوز RIL Init را صدا نمی‌زنیم!

    while(TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            case MSG_ID_RIL_READY:
                APP_DEBUG("<RIL READY> Initializing...\r\n");
                Ql_RIL_Initialize(); // <--- جای درست اینجاست
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
                            // اگر خطا داد، برگرد عقب دوباره تلاش کن
                            m_mqtt_state = STATE_NW_QUERY_STATE; 
                        }
                        break;
                    
                    case URC_MQTT_CONN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Connected: OK\r\n");
                            m_mqtt_state = STATE_MQTT_PUB;
                        } else {
                            APP_DEBUG("MQTT Connect Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                             m_mqtt_state = STATE_MQTT_OPEN; // تلاش مجدد
                        }
                        break;

                    case URC_MQTT_PUB:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("Pub Success\r\n");
                        } else {
                            APP_DEBUG("Pub Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                        }
                        m_mqtt_state = STATE_MQTT_WAIT; // برو به حالت انتظار
                        break;
                }
                break;
            default:
                break;
        }
    }
}

// ================= TIMER LOGIC (UPDATED) =================
static u32 wait_tick = 0;

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
            
            // تغییر ۱: این خط را از کامنت درآوردم تا ببینیم وضعیت شبکه چیست
            APP_DEBUG("Network Check: %d (Waiting for 1 or 5)\r\n", cgreg); 
            
            // cgreg: 0=Not Reg, 1=Registered, 2=Searching, 3=Denied, 5=Roaming
            if((cgreg == NW_STAT_REGISTERED)||(cgreg == NW_STAT_REGISTERED_ROAMING))
            {
                APP_DEBUG("Network Found! Setting APN...\r\n");
                
                RIL_NW_SetGPRSContext(0);
                RIL_NW_SetAPN(1, APN, USERID, PASSWD);
                
                ret = RIL_NW_OpenPDPContext();
                
                // تغییر ۲: نمایش نتیجه اتصال اینترنت
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
                 APP_DEBUG("Opening MQTT Connection...\r\n");
            } else {
                 APP_DEBUG("MQTT Open Command Failed: %d\r\n", ret);
            }
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_CONN:
            APP_DEBUG("Sending Connect Packet...\r\n");
            ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_PUB:
            Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
            Ql_sprintf((char*)mqtt_payload, "{\"temp\":24, \"status\":\"live\"}");
            
            APP_DEBUG("Publishing Data...\r\n");
            pub_message_id++;
            
            ret = RIL_MQTT_QMTPUB(connect_id, pub_message_id, 0, 0, topic, Ql_strlen((char*)mqtt_payload), mqtt_payload);
            
            m_mqtt_state = STATE_IDLE; 
            break;

        case STATE_MQTT_WAIT:
            wait_tick++;
            if(wait_tick >= 10) { 
                wait_tick = 0;
                m_mqtt_state = STATE_MQTT_PUB;
            }
            break;
            
        case STATE_IDLE:
            break;
    }
}


static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara) {}
static void mqtt_recv(u8* buffer,u32 length) { APP_DEBUG("Recv: %s\r\n", buffer); }
