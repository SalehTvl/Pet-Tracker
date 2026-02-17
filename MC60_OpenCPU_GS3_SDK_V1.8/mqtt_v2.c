#ifdef __CUSTOMER_CODE__

#include "custom_feature_def.h"
#include "ql_stdlib.h"
#include "ql_common.h"
#include "ql_system.h"
#include "ql_uart.h"
#include "ql_timer.h"
#include "ql_trace.h"

#include "ril.h"
#include "ril_network.h"
#include "ril_mqtt.h"

#define DEBUG_PORT UART_PORT1
#define DBG_BUF_LEN 256
static char DBG_BUFFER[DBG_BUF_LEN];

#define APP_DEBUG(fmt, ...)                           \
    do {                                              \
        Ql_memset(DBG_BUFFER, 0, DBG_BUF_LEN);        \
        Ql_sprintf(DBG_BUFFER, fmt, ##__VA_ARGS__);   \
        Ql_UART_Write(DEBUG_PORT,                     \
            (u8*)DBG_BUFFER,                          \
            Ql_strlen(DBG_BUFFER));                   \
    } while (0)

/* ================= CONFIG YOU MUST SET ================= */

#define APN         "mcinet"                 // ← APN سیم‌کارت
#define MQTT_HOST   "thingsboard.cloud"
#define MQTT_PORT   1883

#define TB_TOKEN    "FxVYefTdmZpOWZ4DNIsf"       // ← Access Token دستگاه

#define PUB_TOPIC   "v1/devices/me/telemetry"

/* ======================================================= */

static u8 mqtt_id = 0;
static u16 pub_msg_id = 0;
static bool mqtt_connected = FALSE;

/* payload ثابت */
static u8 payload[] = "{\"msg\":\"hello thingsboard\"}";

void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);
    APP_DEBUG("=== MC60 MQTT Hello Test ===\r\n");

    while (TRUE)
    {
        Ql_OS_GetMessage(&msg);

        switch (msg.message)
        {
        case MSG_ID_RIL_READY:
            APP_DEBUG("RIL READY\r\n");
            Ql_RIL_Initialize();
            break;

        case MSG_ID_URC_INDICATION:
            switch (msg.param1)
            {
            case URC_SIM_CARD_STATE_IND:
                if (msg.param2 == SIM_STAT_READY)
                {
                    APP_DEBUG("SIM READY\r\n");

                    RIL_NW_SetGPRSContext(0);
                    RIL_NW_SetAPN(1, APN, "", "");
                    RIL_NW_OpenPDPContext();
                }
                break;

            case URC_GPRS_NW_STATE_IND:
                if (msg.param2 == NW_STAT_REGISTERED)
                {
                    APP_DEBUG("GPRS OK\r\n");
                    RIL_MQTT_QMTOPEN(mqtt_id,
                        (u8*)MQTT_HOST,
                        MQTT_PORT);
                }
                break;

            case URC_MQTT_OPEN:
            {
                MQTT_Urc_Param_t* p = msg.param2;
                if (p->result == 0)
                {
                    APP_DEBUG("MQTT OPEN OK\r\n");
                    RIL_MQTT_QMTCONN(
                        mqtt_id,
                        (u8*)"mc60",
                        (u8*)TB_TOKEN,
                        (u8*)""
                    );
                }
                else
                {
                    APP_DEBUG("MQTT OPEN FAIL=%d\r\n", p->result);
                }
                break;
            }

            case URC_MQTT_CONN:
            {
                MQTT_Urc_Param_t* p = msg.param2;
                if (p->result == 0)
                {
                    APP_DEBUG("MQTT CONNECTED\r\n");
                    mqtt_connected = TRUE;

                    pub_msg_id++;
                    RIL_MQTT_QMTPUB(
                        mqtt_id,
                        pub_msg_id,
                        0,
                        0,
                        (u8*)PUB_TOPIC,
                        Ql_strlen((char*)payload),
                        payload
                    );
                }
                else
                {
                    APP_DEBUG("MQTT CONN FAIL=%d\r\n", p->result);
                }
                break;
            }

            case URC_MQTT_PUB:
            {
                MQTT_Urc_Param_t* p = msg.param2;
                if (p->result == 0)
                {
                    APP_DEBUG("PUBLISH OK ✅\r\n");
                }
                else
                {
                    APP_DEBUG("PUBLISH FAIL=%d\r\n", p->result);
                }
                break;
            }

            default:
                break;
            }
            break;

        default:
            break;
        }
    }
}

#endif
