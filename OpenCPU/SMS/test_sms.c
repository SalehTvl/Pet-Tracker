#ifdef __CUSTOMER_CODE__

#include "custom_feature_def.h"

#include "ril.h"
#include "ril_util.h"
#include "ril_sms.h"
#include "ril_system.h"

#include "ql_stdlib.h"
#include "ql_trace.h"
#include "ql_system.h"
#include "ql_uart.h"

/* ================= Configuration ================= */

static char DEST_PHONE[] = "+989378043205";
static char SMS_TEXT[]   = "sms from opencpu 1";

static bool sms_sent = FALSE;

/* ================= Debug Macro ================= */

#define DEBUG_PORT  UART_PORT1
#define DBG_BUF_LEN 256
static char DBG_BUFFER[DBG_BUF_LEN];

#define APP_DEBUG(FORMAT, ...)                                \
    do {                                                      \
        Ql_memset(DBG_BUFFER, 0, DBG_BUF_LEN);                \
        Ql_sprintf(DBG_BUFFER, FORMAT, ##__VA_ARGS__);        \
        Ql_UART_Write(DEBUG_PORT,                             \
            (u8*)DBG_BUFFER,                                 \
            Ql_strlen(DBG_BUFFER));                           \
    } while(0)

/* ================= SMS Send Function ================= */

static void Send_One_SMS(void)
{
    u32 msgRef = 0;

    APP_DEBUG("Sending SMS...\r\n");

    RIL_SMS_SendSMS_Text(
        DEST_PHONE,
        Ql_strlen(DEST_PHONE),
        LIB_SMS_CHARSET_GSM,
        SMS_TEXT,
        Ql_strlen(SMS_TEXT),
        &msgRef
    );

    APP_DEBUG("SMS Send Requested, ref=%d\r\n", msgRef);
}

/* ================= Main Task ================= */

void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    /* Open UART for debug */
    Ql_UART_Open(UART_PORT1, 115200, FC_NONE);

    APP_DEBUG("MC60 OpenCPU SMS App Started\r\n");

    while (TRUE)
    {
        Ql_OS_GetMessage(&msg);

        switch (msg.message)
        {
        case MSG_ID_RIL_READY:
            APP_DEBUG("<-- RIL READY -->\r\n");
            Ql_RIL_Initialize();
            break;

        case MSG_ID_URC_INDICATION:
            if (msg.param1 == URC_SYS_INIT_STATE_IND)
            {
                APP_DEBUG("SYS INIT STATE = %d\r\n", msg.param2);

                if ((msg.param2 == SYS_STATE_SMSOK) && !sms_sent)
                {
                    Send_One_SMS();
                    sms_sent = TRUE;
                }
            }
            break;

        default:
            break;
        }
    }
}

#endif /* __CUSTOMER_CODE__ */
