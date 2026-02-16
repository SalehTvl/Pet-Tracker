#ifdef __CUSTOMER_CODE__

#include "custom_feature_def.h"

#include "ril.h"
#include "ril_sms.h"
#include "ql_system.h"
#include "ql_uart.h"
#include "ql_stdlib.h"

/* ================= CONFIG ================= */

#define PERIOD_MS 60000
#define GPS_RETRY_COUNT 5
#define GPS_RETRY_DELAY 2000

static char DEST_PHONE[] = "+989378043205";

static char RMC_BUF[256];
static bool sms_ready = FALSE;

/* ================= DEBUG ================= */

#define DEBUG_PORT UART_PORT1
#define DBG_BUF_LEN 256
static char DBG_BUF[DBG_BUF_LEN];

#define APP_DEBUG(fmt, ...)                          \
    do {                                             \
        Ql_memset(DBG_BUF, 0, DBG_BUF_LEN);           \
        Ql_sprintf(DBG_BUF, fmt, ##__VA_ARGS__);      \
        Ql_UART_Write(DEBUG_PORT,                     \
            (u8*)DBG_BUF, Ql_strlen(DBG_BUF));        \
    } while (0)

/* ================= SMS ================= */

static void SendSMS(char* text)
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

/* ================= GPS ================= */

static void GPS_On(void)
{
    RIL_GPS_Open(1);
    APP_DEBUG("[GPS] Power ON\r\n");
}

/* ================= RMC PARSER (NO strtok) ================= */

static bool ParseRMC(
    char* rmc,
    float* lat,
    float* lon,
    char* fail_reason
)
{
    int comma = 0;
    char *p = rmc;

    char lat_buf[16] = {0};
    char lon_buf[16] = {0};
    char lat_dir = 0;
    char lon_dir = 0;
    char status = 0;

    char* field_start = p;

    while (*p)
    {
        if (*p == ',' || *(p + 1) == '\0')
        {
            int len = p - field_start;

            switch (comma)
            {
            case 2: // Status
                status = field_start[0];
                break;

            case 3: // Latitude
                Ql_memcpy(lat_buf, field_start, len);
                lat_buf[len] = 0;
                break;

            case 4: // N/S
                lat_dir = field_start[0];
                break;

            case 5: // Longitude
                Ql_memcpy(lon_buf, field_start, len);
                lon_buf[len] = 0;
                break;

            case 6: // E/W
                lon_dir = field_start[0];
                break;
            }

            comma++;
            field_start = p + 1;
        }
        p++;
    }

    if (status != 'A')
    {
        Ql_strcpy(fail_reason, "No GPS fix (status=V)");
        return FALSE;
    }

    if (!lat_buf[0] || !lon_buf[0])
    {
        Ql_strcpy(fail_reason, "Invalid RMC fields");
        return FALSE;
    }

    float raw_lat = Ql_atof(lat_buf);
    float raw_lon = Ql_atof(lon_buf);

    int lat_deg = (int)(raw_lat / 100);
    int lon_deg = (int)(raw_lon / 100);

    *lat = lat_deg + (raw_lat - lat_deg * 100) / 60.0f;
    *lon = lon_deg + (raw_lon - lon_deg * 100) / 60.0f;

    if (lat_dir == 'S') *lat = -*lat;
    if (lon_dir == 'W') *lon = -*lon;

    return TRUE;
}

/* ================= MAIN TASK ================= */

void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    Ql_UART_Open(UART_PORT1, 115200, FC_NONE);
    APP_DEBUG("=== MC60 GPS SMS Tracker Started ===\r\n");

    while (1)
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
                sms_ready = TRUE;
                APP_DEBUG("[SYS] SMS Ready\r\n");
            }
            break;
        }

        if (!sms_ready)
            continue;

        float lat = 0, lon = 0;
        bool fix_ok = FALSE;
        char fail_reason[64] = {0};

        for (int i = 0; i < GPS_RETRY_COUNT; i++)
        {
            s32 ret = RIL_GPS_Read("RMC", RMC_BUF);

            if (ret != RIL_AT_SUCCESS)
            {
                Ql_strcpy(fail_reason, "RIL_GPS_Read failed");
                APP_DEBUG("[GPS] Read error (%d)\r\n", ret);
            }
            else
            {
                APP_DEBUG("[GPS] RMC: %s\r\n", RMC_BUF);

                if (ParseRMC(RMC_BUF, &lat, &lon, fail_reason))
                {
                    fix_ok = TRUE;
                    break;
                }
                else
                {
                    APP_DEBUG("[GPS] Parse fail: %s\r\n", fail_reason);
                }
            }
            Ql_Sleep(GPS_RETRY_DELAY);
        }

        if (fix_ok)
        {
            char sms[160];
            Ql_sprintf(
                sms,
                "Location:\r\nlat=%.6f\r\nlon=%.6f",
                lat, lon
            );
            SendSMS(sms);
            APP_DEBUG("[SMS] Location sent\r\n");
        }
        else
        {
            char sms[160];
            Ql_sprintf(
                sms,
                "GPS failed:\r\n%s",
                fail_reason
            );
            SendSMS(sms);
            APP_DEBUG("[SMS] Fail sent (%s)\r\n", fail_reason);
        }

        Ql_Sleep(PERIOD_MS);
    }
}

#endif
