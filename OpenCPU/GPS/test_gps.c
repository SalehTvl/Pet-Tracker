#ifdef __CUSTOMER_CODE__

#include "custom_feature_def.h"

#include "ril.h"
#include "ril_sms.h"
#include "ril_system.h"

#include "ql_stdlib.h"
#include "ql_system.h"
#include "ql_trace.h"
#include "ql_uart.h"

/* ================= CONFIG ================= */

#define PERIOD_MS        60000     // 1 minute
#define GPS_TRY_COUNT    5
#define GPS_TRY_DELAY    2000      // 2 sec

static char DEST_PHONE[] = "+989378043205";
static bool system_ready = FALSE;
static char RMC_BUFFER[128];

/* ================= DEBUG ================= */

#define DEBUG_PORT UART_PORT1
#define DBG_BUF_LEN 256
static char DBG_BUFFER[DBG_BUF_LEN];

#define APP_DEBUG(FORMAT, ...)                         \
    do {                                               \
        Ql_memset(DBG_BUFFER, 0, DBG_BUF_LEN);         \
        Ql_sprintf(DBG_BUFFER, FORMAT, ##__VA_ARGS__); \
        Ql_UART_Write(DEBUG_PORT,                      \
            (u8*)DBG_BUFFER,                           \
            Ql_strlen(DBG_BUFFER));                    \
    } while (0)

/* ================= GPS ================= */

static void GPS_On(void)
{
    RIL_GPS_Open(1);
    APP_DEBUG("GPS ON\r\n");
}

/* ================= SMS ================= */

static void Send_SMS(char* text)
{
    u32 ref;
    RIL_SMS_SendSMS_Text(
        DEST_PHONE,
        Ql_strlen(DEST_PHONE),
        LIB_SMS_CHARSET_GSM,
        text,
        Ql_strlen(text),
        &ref
    );
}

/* ================= RMC PARSE ================= */

static bool Parse_RMC(char* rmc, float* lat, float* lon)
{
    // Status check
    if (rmc[30] != 'A')
        return FALSE;

    char lat_buf[12] = {0};
    char lon_buf[12] = {0};

    Ql_memcpy(lat_buf, &rmc[32], 9);   // ddmm.mmmm
    Ql_memcpy(lon_buf, &rmc[44], 10);  // dddmm.mmmm

    float raw_lat = Ql_atof(lat_buf);
    float raw_lon = Ql_atof(lon_buf);

    int lat_deg = (int)(raw_lat / 100);
    int lon_deg = (int)(raw_lon / 100);

    *lat = lat_deg + (raw_lat - lat_deg * 100) / 60.0f;
    *lon = lon_deg + (raw_lon - lon_deg * 100) / 60.0f;

    return TRUE;
}

/* ================= MAIN ================= */

void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    Ql_UART_Open(UART_PORT1, 115200, FC_NONE);
    APP_DEBUG("MC60 GPS Periodic SMS Started\r\n");

    while (TRUE)
    {
        Ql_OS_GetMessage(&msg);

        switch (msg.message)
        {
        case MSG_ID_RIL_READY:
            Ql_RIL_Initialize();
            GPS_On();
            break;

        case MSG_ID_URC_INDICATION:
            if (msg.param1 == URC_SYS_INIT_STATE_IND &&
                msg.param2 == SYS_STATE_SMSOK)
            {
                system_ready = TRUE;
                APP_DEBUG("System ready for SMS\r\n");
            }
            break;

        default:
            break;
        }

        if (system_ready)
        {
            bool fix_ok = FALSE;
            float lat = 0, lon = 0;

            for (int i = 0; i < GPS_TRY_COUNT; i++)
            {
                if (RIL_GPS_Read("RMC", RMC_BUFFER) == RIL_AT_SUCCESS)
                {
                    if (Parse_RMC(RMC_BUFFER, &lat, &lon))
                    {
                        fix_ok = TRUE;
                        break;
                    }
                }
                Ql_Sleep(GPS_TRY_DELAY);
            }

            if (fix_ok)
            {
                char sms[160];
                Ql_sprintf(
                    sms,
                    "Device location:\r\nlat=%.6f , lon=%.6f",
                    lat, lon
                );
                Send_SMS(sms);
                APP_DEBUG("Location SMS sent\r\n");
            }
            else
            {
                Send_SMS("GPS fix failed, location unavailable");
                APP_DEBUG("GPS FAIL SMS sent\r\n");
            }

            Ql_Sleep(PERIOD_MS);
        }
    }
}

#endif
