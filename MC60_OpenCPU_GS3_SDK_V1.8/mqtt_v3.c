#define __CUSTOMER_CODE__
#ifdef __CUSTOMER_CODE__

#include "ql_stdlib.h"
#include "ql_common.h"
#include "ql_system.h"
#include "ql_type.h"
#include "ql_trace.h"
#include "ql_error.h"
#include "ql_uart.h"
#include "ql_timer.h"

#include "ril.h"
#include "ril_network.h"
#include "ril_mqtt.h"

/// ---------------- MQTT STATE ----------------
typedef enum {
    STATE_NW_CHECK,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_MQTT_PUB,
    STATE_IDLE
} MQTT_STATE;

static u8 mqtt_state = STATE_NW_CHECK;

/// ---------------- MQTT PARAM ----------------
Enum_ConnectID connect_id = ConnectID_0;

u8 clientID[] = "mc60";
u8 username[] = "FxVYefTdmZpOWZ4DNIsf";
u8 password[] = "";

static u8 pub_topic[] = "v1/devices/me/telemetry";
static u8 payload[]   = "{\"value\":123}";

u32 pub_msg_id = 0;

/// ---------------- NETWORK ----------------
#define APN       "mcinet"
#define USERID    ""
#define PASSWD    ""

#define HOST_NAME "thingsboard.cloud"
#define HOST_PORT 1883

/// ---------------- TIMER ----------------
#define MQTT_TIMER_ID     0x01
#define MQTT_TIMER_PERIOD 1000

/// ---------------- DEBUG ----------------
#define DEBUG_PORT UART_PORT1
#define DBG_BUF_LEN 256
static char dbg_buf[DBG_BUF_LEN];

#define DEBUG(fmt, ...) \
    do { \
        Ql_memset(dbg_buf, 0, DBG_BUF_LEN); \
        Ql_sprintf(dbg_buf, fmt, ##__VA_ARGS__); \
        Ql_UART_Write(DEBUG_PORT, (u8*)dbg_buf, Ql_strlen(dbg_buf)); \
    } while(0)

/// ---------------- TIMER CALLBACK ----------------
static void Timer_Callback(u32 timerId, void *param)
{
    s32 ret;
    s32 cgreg;

    if (timerId != MQTT_TIMER_ID) return;

    switch (mqtt_state)
    {
    case STATE_NW_CHECK:
        RIL_NW_GetGPRSState(&cgreg);
        DEBUG("NW state: %d\r\n", cgreg);

        if (cgreg == NW_STAT_REGISTERED || cgreg == NW_STAT_REGISTERED_ROAMING)
        {
            RIL_NW_SetGPRSContext(0);
            RIL_NW_SetAPN(1, APN, USERID, PASSWD);
            if (RIL_AT_SUCCESS == RIL_NW_OpenPDPContext())
            {
                DEBUG("PDP Activated\r\n");
                mqtt_state = STATE_MQTT_CFG;
            }
        }
        break;

    case STATE_MQTT_CFG:
        RIL_MQTT_QMTCFG_Version_Select(connect_id, Version_3_1_1);
        mqtt_state = STATE_MQTT_OPEN;
        break;

    case STATE_MQTT_OPEN:
        ret = RIL_MQTT_QMTOPEN(connect_id, HOST_NAME, HOST_PORT);
        if (ret == RIL_AT_SUCCESS)
        {
            DEBUG("MQTT Open\r\n");
            mqtt_state = STATE_MQTT_CONN;
        }
        break;

    case STATE_MQTT_CONN:
        ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, password);
        if (ret == RIL_AT_SUCCESS)
        {
            DEBUG("MQTT Connected\r\n");
            mqtt_state = STATE_MQTT_PUB;
        }
        break;

    case STATE_MQTT_PUB:
        pub_msg_id++;
        ret = RIL_MQTT_QMTPUB(
            connect_id,
            pub_msg_id,
            QOS1_AT_LEASET_ONCE,
            0,
            pub_topic,
            Ql_strlen(payload),
            payload
        );

        if (ret == RIL_AT_SUCCESS)
            DEBUG("Telemetry sent\r\n");

        mqtt_state = STATE_IDLE;
        break;

    case STATE_IDLE:
        break;
    }
}

/// ---------------- MAIN TASK ----------------
void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);
    DEBUG("MC60 MQTT ThingsBoard Start\r\n");

    Ql_Timer_Register(MQTT_TIMER_ID, Timer_Callback, NULL);

    while (1)
    {
        Ql_OS_GetMessage(&msg);
        if (msg.message == MSG_ID_RIL_READY)
        {
            Ql_RIL_Initialize();
        }
        else if (msg.message == MSG_ID_URC_INDICATION)
        {
            if (msg.param1 == URC_SIM_CARD_STATE_IND &&
                msg.param2 == SIM_STAT_READY)
            {
                Ql_Timer_Start(MQTT_TIMER_ID, MQTT_TIMER_PERIOD, TRUE);
            }
        }
    }
}

#endif
