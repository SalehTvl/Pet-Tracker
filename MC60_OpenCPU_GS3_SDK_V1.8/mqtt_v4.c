/*****************************************************************************
*  MC60 OpenCPU MQTT Client for ThingsBoard
*  Based on Quectel SDK Example
*****************************************************************************/
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
// 1. تنظیمات سیم‌کارت (همراه اول: mcinet | ایرانسل: mtnirancell)
#define APN      "mcinet"
#define USERID   ""
#define PASSWD   ""

// 2. توکن دستگاه خود را اینجا وارد کنید
#define DEVICE_ACCESS_TOKEN  "FxVYefTdmZpOWZ4DNIsf" 

// 3. تنظیمات سرور ThingsBoard
#define HOST_NAME             "thingsboard.cloud"
#define HOST_PORT             1883
#define MQTT_TOPIC            "v1/devices/me/telemetry"

// ================= DEBUG & UART =================
#define DEBUG_ENABLE 1
#define DEBUG_PORT  UART_PORT2 // همان پورت آپلود فریمور
#define DBG_BUF_LEN   1024
static char DBG_BUFFER[DBG_BUF_LEN];

#define APP_DEBUG(FORMAT,...) {\
    Ql_memset(DBG_BUFFER, 0, DBG_BUF_LEN);\
    Ql_sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8*)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
}

// ================= STATE MACHINE =================
typedef enum{
    STATE_NW_QUERY_STATE,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_MQTT_SUB,
    STATE_MQTT_PUB,
    STATE_MQTT_WAIT, // حالت انتظار برای ارسال بعدی
    STATE_MQTT_TOTAL_NUM
}Enum_MQTT_STATE;
static u8 m_mqtt_state = STATE_NW_QUERY_STATE;

// ================= TIMERS =================
#define MQTT_TIMER_ID         0x200
#define MQTT_TIMER_PERIOD     1000  // چک کردن استیت هر 1 ثانیه

// ================= VARIABLES =================
Enum_ConnectID connect_id = ConnectID_0;
u32 pub_message_id = 0;

// بافرهای MQTT
u8 clientID[] =      "MC60_Tracker\0"; 
u8 username[] =      DEVICE_ACCESS_TOKEN; // در تینگزبورد یوزرنیم همان توکن است
u8 passwd[] =        "";                  // پسورد خالی است

u8 mqtt_payload[128];
u8 topic[] = MQTT_TOPIC;

static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara);
static void Callback_Timer(u32 timerId, void* param);
static void mqtt_recv(u8* buffer,u32 length);

// ================= MAIN TASK =================
void proc_main_task(s32 taskId)
{
    ST_MSG msg;
    s32 ret;

    // 1. باز کردن پورت سریال برای دیباگ (با سرعت 115200)
    Ql_UART_Register(DEBUG_PORT, CallBack_UART_Hdlr, NULL);
    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);

    APP_DEBUG("\r\n<--- MC60 ThingsBoard Client Started --->\r\n");

    // 2. رجیستر کردن تایمر و MQTT Callback
    Ql_Timer_Register(MQTT_TIMER_ID, Callback_Timer, NULL);
    Ql_Mqtt_Recv_Register(mqtt_recv);

    // 3. راه‌اندازی RIL
    Ql_RIL_Initialize();

    while(TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            case MSG_ID_RIL_READY:
                APP_DEBUG("<RIL READY>\r\n");
                break;
            case MSG_ID_URC_INDICATION:
                switch (msg.param1)
                {
                    case URC_SIM_CARD_STATE_IND:
                        APP_DEBUG("SIM State: %d\r\n", msg.param2);
                        if(SIM_STAT_READY == msg.param2)
                        {
                            // سیم‌کارت آماده است، تایمر را استارت می‌زنیم
                            Ql_Timer_Start(MQTT_TIMER_ID, MQTT_TIMER_PERIOD, TRUE);
                        }
                        break;
                    
                    case URC_MQTT_OPEN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result)
                        {
                            APP_DEBUG("MQTT Open OK\r\n");
                            m_mqtt_state = STATE_MQTT_CONN;
                        }
                        else APP_DEBUG("MQTT Open Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                        break;
                    
                    case URC_MQTT_CONN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result)
                        {
                            APP_DEBUG("MQTT Connected!\r\n");
                            m_mqtt_state = STATE_MQTT_PUB; // برو برای ارسال داده
                        }
                        else APP_DEBUG("MQTT Connect Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                        break;

                    case URC_MQTT_PUB:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result)
                        {
                            APP_DEBUG("Message Published OK\r\n");
                            // بعد از ارسال موفق، برو به حالت انتظار
                            m_mqtt_state = STATE_MQTT_WAIT; 
                        }
                        else 
                        {
                            APP_DEBUG("Publish Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                            m_mqtt_state = STATE_MQTT_PUB; // دوباره تلاش کن
                        }
                        break;
                }
                break;
            default:
                break;
        }
    }
}

// ================= TIMER CALLBACK (LOGIC) =================
static u32 wait_counter = 0;

static void Callback_Timer(u32 timerId, void* param)
{
    s32 ret;
    if(MQTT_TIMER_ID != timerId) return;

    switch(m_mqtt_state)
    {
        case STATE_NW_QUERY_STATE:
        {
            s32 cgreg = 0;
            RIL_NW_GetGPRSState(&cgreg);
            APP_DEBUG("Network Check: %d\r\n", cgreg);
            // 1=Home, 5=Roaming
            if((cgreg == NW_STAT_REGISTERED)||(cgreg == NW_STAT_REGISTERED_ROAMING))
            {
                RIL_NW_SetGPRSContext(0);
                RIL_NW_SetAPN(1, APN, USERID, PASSWD);
                ret = RIL_NW_OpenPDPContext();
                if(ret == RIL_AT_SUCCESS)
                {
                    APP_DEBUG("GPRS Attached!\r\n");
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
            // درخواست باز کردن سوکت فقط یکبار ارسال شود، نتیجه در URC می‌آید
            ret = RIL_MQTT_QMTOPEN(connect_id, HOST_NAME, HOST_PORT);
            if(ret != RIL_AT_SUCCESS) APP_DEBUG("Open Req Fail (Busy?)\r\n");
            m_mqtt_state = STATE_MQTT_TOTAL_NUM; // منتظر URC بمان
            break;

        case STATE_MQTT_CONN:
            // درخواست لاگین
            ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
            if(ret != RIL_AT_SUCCESS) APP_DEBUG("Conn Req Fail\r\n");
            m_mqtt_state = STATE_MQTT_TOTAL_NUM; // منتظر URC بمان
            break;

        case STATE_MQTT_PUB:
        {
            // ساخت داده جیسون تستی
            Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
            Ql_sprintf((char*)mqtt_payload, "{\"temperature\":25, \"status\":\"ok\"}");
            
            APP_DEBUG("Sending: %s\r\n", mqtt_payload);
            
            pub_message_id++;
            // در SDK قدیمی qos0 تعریف نشده، از عدد 0 استفاده میکنیم
            ret = RIL_MQTT_QMTPUB(connect_id, pub_message_id, 0, 0, topic, Ql_strlen((char*)mqtt_payload), mqtt_payload);
            
            if(ret != RIL_AT_SUCCESS) APP_DEBUG("Pub Req Fail\r\n");
            
            m_mqtt_state = STATE_MQTT_TOTAL_NUM; // منتظر URC بمان
            break;
        }

        case STATE_MQTT_WAIT:
            // یک تاخیر ساده ۱۰ ثانیه‌ای ایجاد میکنیم
            wait_counter++;
            if(wait_counter > 10) 
            {
                wait_counter = 0;
                m_mqtt_state = STATE_MQTT_PUB; // دوباره داده بفرست
            }
            break;

        default:
            break;
    }
}

// سایر توابع مورد نیاز
static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara) {}
static void mqtt_recv(u8* buffer,u32 length) { APP_DEBUG("Recv: %s\r\n", buffer); }
