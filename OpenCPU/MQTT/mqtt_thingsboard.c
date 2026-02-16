#define __CUSTOMER_CODE__
#ifdef __CUSTOMER_CODE__

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
#include "ql_uart.h"

// بافر برای سریال
#define SERIAL_RX_BUFFER_LEN  2048
static u8 m_RxBuf_Uart[SERIAL_RX_BUFFER_LEN];

// 1. تابعی که اجازه می‌دهد مثل printf در سی شارپ یا سی پلاس پلاس لاگ بفرستید
void APP_DEBUG(const char* fmt, ...)
{
    char buffer[512]; // بافر موقت برای متن پیام
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    Ql_vsnprintf(buffer, 512, fmt, arg_ptr);
    va_end(arg_ptr);
    
    // ارسال متن ساخته شده به پورت سریال 1 (همان پورت اصلی)
    Ql_UART_Write(UART_PORT1, (u8*)buffer, Ql_strlen(buffer));
}

// 2. کال‌بک سریال (فعلاً خالی باشد، فقط برای رجیستر کردن لازم است)
static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara)
{
    // اگر بخواهیم دستوری به ماژول بفرستیم اینجا دریافت می‌شود
    // فعلاً برای دیباگ نیازی نیست
}


// ================= CONFIGURATION =================
// تنظیمات سیم‌کارت
#define APN       "mcinet\0" // یا mtnirancell
#define USERID    ""
#define PASSWD    ""

// تنظیمات تینگزبورد
#define MQTT_BROKER_HOST "thingsboard.cloud"
#define MQTT_BROKER_PORT 1883
// !!! توکن کپی شده از تینگزبورد را اینجا بگذارید !!!
#define DEVICE_ACCESS_TOKEN "FxVYefTdmZpOWZ4DNIsf" 

// تنظیمات زمان‌بندی (میلی‌ثانیه)
#define PUBLISH_INTERVAL 10000 
#define MQTT_TIMER_PERIOD 1000

// پورت دیباگ (برای دیدن لاگ‌ها در ترمینال)
#define DEBUG_PORT UART_PORT1 

// ================= GLOBALS =================
#define DBG_BUF_LEN 512
static char DBG_BUFFER[DBG_BUF_LEN];

// بافرهای MQTT
static u8 clientID[] = "MC60_GPS\0"; // هر چیزی می‌تواند باشد
static u8 username[] = DEVICE_ACCESS_TOKEN;
static u8 passwd[] = "\0"; // پسورد خالی باشد
static u8 pub_topic[] = "v1/devices/me/telemetry\0"; // تاپیک ثابت تینگزبورد

// بافرهای GPS
static char RMC_BUF[512];
static char mqtt_payload[256];

// متغیرهای وضعیت
typedef enum {
    STATE_NW_QUERY_STATE,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_MQTT_LOOP, // حلقه اصلی ارسال دیتا
    STATE_MQTT_TOTAL_NUM
} Enum_MQTT_STATE;

static u8 m_mqtt_state = STATE_NW_QUERY_STATE;
Enum_ConnectID connect_id = ConnectID_0;
u32 pub_message_id = 0;
u32 timer_counter = 0; // برای زمان‌بندی ارسال

#define MQTT_TIMER_ID 0x200

// ================= DEBUG HELPER =================
#define APP_DEBUG(FORMAT, ...) {\
    Ql_memset(DBG_BUFFER, 0, DBG_BUF_LEN);\
    Ql_sprintf(DBG_BUFFER, FORMAT, ##__VA_ARGS__);\
    Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8 *)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
}

// ================= FUNCTION PROTOTYPES =================
static void Callback_Timer(u32 timerId, void *param);
static void mqtt_recv_cb(u8 *buffer, u32 length);
static void GPS_On(void);
static bool ParseRMC(char* rmc, float* lat, float* lon);

// ================= MAIN TASK =================
void proc_main_task(s32 taskId)
{
    ST_MSG msg;
    s32 ret;

    // --- شروع تنظیمات سریال برای دیباگ ---
    
    // 1. رجیستر کردن پورت 1
    Ql_UART_Register(UART_PORT1, CallBack_UART_Hdlr, NULL);
    
    // 2. باز کردن پورت با سرعت 115200
    // FC_NONE یعنی بدون Flow Control (خیلی مهم)
    Ql_UART_Open(UART_PORT1, 115200, FC_NONE);
    
    // 3. ارسال یک پیام تست برای اطمینان از سالم بودن ارتباط
    APP_DEBUG("\r\n\r\n=== SYSTEM RESTART ===\r\n");
    APP_DEBUG("Debug Port Initialized Successfully!\r\n");
    
    // --- پایان تنظیمات سریال ---

    // راه‌اندازی RIL و تایمرها مثل قبل...
    RIL_BT_Initialize(); // اگر استفاده می‌کنید

    // 2. راه اندازی RIL
    Ql_OS_GetMessage(&msg); // صبر برای اولین پیام سیستم
    Ql_RIL_Initialize();
    
    // 3. روشن کردن GPS
    GPS_On();

    // 4. رجیستر کردن تایمر و MQTT Callback
    Ql_Timer_Register(MQTT_TIMER_ID, Callback_Timer, NULL);
    Ql_Mqtt_Recv_Register(mqtt_recv_cb);
    

    while (TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch (msg.message)
        {
        case MSG_ID_RIL_READY:
            APP_DEBUG("<RIL READY>\r\n");
            break;

        case MSG_ID_URC_INDICATION:
            switch (msg.param1)
            {
            case URC_SIM_CARD_STATE_IND:
                if (SIM_STAT_READY == msg.param2) {
                    APP_DEBUG("<SIM READY> Starting Timer...\r\n");
                    Ql_Timer_Start(MQTT_TIMER_ID, MQTT_TIMER_PERIOD, TRUE);
                }
                break;
            
            case URC_MQTT_OPEN: {
                MQTT_Urc_Param_t *res = (MQTT_Urc_Param_t*)msg.param2;
                if (0 == res->result) {
                    APP_DEBUG("MQTT Open OK\r\n");
                    m_mqtt_state = STATE_MQTT_CONN;
                } else {
                    APP_DEBUG("MQTT Open Fail: %d. Retrying...\r\n", res->result);
                    m_mqtt_state = STATE_MQTT_OPEN; // تلاش مجدد
                }
                break;
            }
            case URC_MQTT_CONN: {
                MQTT_Urc_Param_t *res = (MQTT_Urc_Param_t*)msg.param2;
                if (0 == res->result) {
                    APP_DEBUG("MQTT Connected OK! Entering Loop.\r\n");
                    m_mqtt_state = STATE_MQTT_LOOP;
                } else {
                    APP_DEBUG("MQTT Connect Fail: %d\r\n", res->result);
                    m_mqtt_state = STATE_MQTT_OPEN; // ریست اتصال
                }
                break;
            }
            case URC_MQTT_PUB: {
                 // نتیجه انتشار پیام
                 // APP_DEBUG("Pub Confirm: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                 break;
            }
            case URC_MQTT_CLOSE:
            case URC_MQTT_DISC:
                APP_DEBUG("MQTT Disconnected! Reconnecting...\r\n");
                m_mqtt_state = STATE_MQTT_OPEN;
                break;
            }
            break;
        }
    }
}

// ================= TIMER CALLBACK (STATE MACHINE) =================
static void Callback_Timer(u32 timerId, void *param)
{
    s32 ret;

    if (MQTT_TIMER_ID != timerId) return;

    switch (m_mqtt_state)
    {
    case STATE_NW_QUERY_STATE:
    {
        s32 cgreg = 0;
        RIL_NW_GetGPRSState(&cgreg);
        APP_DEBUG("Network Check: %d\r\n", cgreg);
        if ((cgreg == NW_STAT_REGISTERED) || (cgreg == NW_STAT_REGISTERED_ROAMING))
        {
            RIL_NW_SetGPRSContext(0);
            RIL_NW_SetAPN(1, APN, USERID, PASSWD);
            ret = RIL_NW_OpenPDPContext();
            if (ret == RIL_AT_SUCCESS) {
                APP_DEBUG("GPRS Attached!\r\n");
                m_mqtt_state = STATE_MQTT_CFG;
            }
        }
        break;
    }
    case STATE_MQTT_CFG:
        RIL_MQTT_QMTCFG_Showrecvlen(connect_id, ShowFlag_1);
        RIL_MQTT_QMTCFG_Version_Select(connect_id, Version_3_1_1);
        m_mqtt_state = STATE_MQTT_OPEN;
        break;

    case STATE_MQTT_OPEN:
        ret = RIL_MQTT_QMTOPEN(connect_id, MQTT_BROKER_HOST, MQTT_BROKER_PORT);
        if (ret != RIL_AT_SUCCESS) APP_DEBUG("Open Req Fail\r\n");
        break;

    case STATE_MQTT_CONN:
        ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
        if (ret != RIL_AT_SUCCESS) APP_DEBUG("Conn Req Fail\r\n");
        break;

    case STATE_MQTT_LOOP:
    {
        timer_counter += MQTT_TIMER_PERIOD;

        if (timer_counter >= PUBLISH_INTERVAL) 
        {
            timer_counter = 0; 

            // 1. خواندن GPS
            float lat = 0.0, lon = 0.0;
            Ql_memset(RMC_BUF, 0, sizeof(RMC_BUF));
            
            ret = RIL_GPS_Read("RMC", RMC_BUF);
            
            if (ret == RIL_AT_SUCCESS && Ql_strlen(RMC_BUF) > 10) 
            {
                if (ParseRMC(RMC_BUF, &lat, &lon)) 
                {
                    // 2. ساختن پکت JSON
                    Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
                    Ql_sprintf(mqtt_payload, "{\"latitude\":%.6f,\"longitude\":%.6f}", lat, lon);
                    
                    APP_DEBUG("Publishing: %s\r\n", mqtt_payload);

                    // 3. ارسال
                    pub_message_id++;
                    
                    // *** اصلاحات اینجا انجام شد ***
                    // به جای QOS0_AT_MOST_ONCE عدد 0 گذاشتیم
                    // و mqtt_payload را به (u8 *) کست کردیم
                    RIL_MQTT_QMTPUB(connect_id, pub_message_id, 0, 0, pub_topic, Ql_strlen(mqtt_payload), (u8 *)mqtt_payload);
                }
                else 
                {
                    APP_DEBUG("GPS Fix Pending...\r\n");
                }
            } 
            else 
            {
                APP_DEBUG("GPS Read Error\r\n");
            }
        }
        break;
    }
    default:
        break;
    }
}

// ================= GPS FUNCTIONS =================
static void GPS_On(void) {
    RIL_GPS_Open(1); // 1 = Cold Start
    APP_DEBUG("GPS Turning ON...\r\n");
}

static bool ParseRMC(char* rmc, float* lat, float* lon)
{
    // پارسر ساده شده برای استخراج مختصات از رشته RMC
    // فرمت: $GPRMC,hhmmss.ss,A,llll.ll,a,yyyy.yy,a,x.x,x.x,ddmmyy,x.x,a*hh
    
    char* p = rmc;
    char* parts[12];
    int part_idx = 0;
    
    // جدا کردن فیلدها با کاما
    parts[part_idx++] = p;
    while (*p) {
        if (*p == ',') {
            *p = 0;
            parts[part_idx++] = p + 1;
            if (part_idx >= 12) break;
        }
        p++;
    }

    // بررسی وضعیت (A = Valid, V = Void)
    // فیلد دوم (ایندکس 2) وضعیت است
    if (part_idx < 6 || *parts[2] != 'A') return FALSE;

    // استخراج Latitude
    // فرمت DDMM.MMMM
    float raw_lat = Ql_atof(parts[3]);
    int lat_deg = (int)(raw_lat / 100);
    *lat = lat_deg + (raw_lat - lat_deg * 100) / 60.0f;
    if (*parts[4] == 'S') *lat = -*lat;

    // استخراج Longitude
    // فرمت DDDMM.MMMM
    float raw_lon = Ql_atof(parts[5]);
    int lon_deg = (int)(raw_lon / 100);
    *lon = lon_deg + (raw_lon - lon_deg * 100) / 60.0f;
    if (*parts[6] == 'W') *lon = -*lon;

    return TRUE;
}

static void mqtt_recv_cb(u8 *buffer, u32 length)
{
    // اگر پیامی از سرور بیاید اینجا دریافت می‌شود
    APP_DEBUG("Recv from Server: %s\r\n", buffer);
}

#endif // __CUSTOMER_CODE__
