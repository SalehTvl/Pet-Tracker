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
#define MQTT_TOPIC            "v1/devices/me/telemetry\0"

// ================= DEBUG =================
#define DEBUG_PORT  UART_PORT2
static char DBG_BUFFER[512];

#define APP_DEBUG(FORMAT,...) {\
    Ql_memset(DBG_BUFFER, 0, 512);\
    Ql_sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8*)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
}

// ================= GLOBAL VARS =================
static char RMC_BUF[512]; // Buffer for GPS Raw Data
float g_lat = 0.0;
float g_lon = 0.0;
float g_speed = 0.0;
bool  g_gps_powered = FALSE;

// ================= STATE MACHINE =================
typedef enum{
    STATE_NW_QUERY_STATE,
    STATE_MQTT_CFG,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONN,
    STATE_GPS_CHECK,    // <--- New State: Waiting for GPS Fix
    STATE_MQTT_PUB,
    STATE_MQTT_WAIT,
    STATE_IDLE
} Enum_MQTT_STATE;

static u8 m_mqtt_state = STATE_NW_QUERY_STATE;

Enum_ConnectID connect_id = ConnectID_0;
u32 pub_message_id = 0;
u32 gps_wait_counter = 0; // Counts seconds waiting for GPS
u32 loop_wait_tick = 0;   // Counts seconds between sends

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

// ================= MAIN TASK =================
void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    // 1. Init Debug UART
    Ql_UART_Register(DEBUG_PORT, CallBack_UART_Hdlr, NULL);
    Ql_UART_Open(DEBUG_PORT, 115200, FC_NONE);
    Ql_Sleep(1000);
    APP_DEBUG("\r\n<--- GPS TRACKER: STABLE BUILD --->\r\n");

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
                // NOTE: Do NOT turn on GPS here. Wait for MQTT.
                break;

            case MSG_ID_URC_INDICATION:
                switch (msg.param1)
                {
                    case URC_SIM_CARD_STATE_IND:
                        APP_DEBUG("SIM State: %d\r\n", msg.param2);
                        if(SIM_STAT_READY == msg.param2)
                        {
                            APP_DEBUG("SIM Ready -> Starting Main Timer\r\n");
                            Ql_Timer_Start(MQTT_TIMER_ID, MQTT_TIMER_PERIOD, TRUE);
                        }
                        break;

                    case URC_MQTT_OPEN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Open: OK\r\n");
                            m_mqtt_state = STATE_MQTT_CONN;
                        } else {
                            APP_DEBUG("MQTT Open Fail: %d\r\n", ((MQTT_Urc_Param_t*)msg.param2)->result);
                            m_mqtt_state = STATE_MQTT_OPEN;
                        }
                        break;

                    case URC_MQTT_CONN:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG("MQTT Connected: OK\r\n");
                            
                            // *** CRITICAL: Turn ON GPS Now ***
                            if(!g_gps_powered) {
                                APP_DEBUG(">>> MQTT Stable. Turning GPS ON...\r\n");
                                GPS_Power_Control(TRUE);
                            }
                            
                            // Start checking GPS
                            gps_wait_counter = 0;
                            m_mqtt_state = STATE_GPS_CHECK;
                            
                        } else {
                            APP_DEBUG("MQTT Connect Fail\r\n");
                            m_mqtt_state = STATE_MQTT_OPEN;
                        }
                        break;

                    case URC_MQTT_PUB:
                        if(0 == ((MQTT_Urc_Param_t*)msg.param2)->result) {
                            APP_DEBUG(">> Data Sent (ACK)\r\n");
                        } else {
                            APP_DEBUG(">> Send Failed\r\n");
                        }
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

    switch(m_mqtt_state)
    {
        case STATE_NW_QUERY_STATE:
        {
            s32 cgreg = 0;
            RIL_NW_GetGPRSState(&cgreg);
            // APP_DEBUG("Network Check: %d\r\n", cgreg); // Uncomment to debug network

            if((cgreg == NW_STAT_REGISTERED)||(cgreg == NW_STAT_REGISTERED_ROAMING))
            {
                APP_DEBUG("Network Found! Attaching GPRS...\r\n");
                RIL_NW_SetGPRSContext(0);
                RIL_NW_SetAPN(1, APN, USERID, PASSWD);
                ret = RIL_NW_OpenPDPContext();

                if(ret == RIL_AT_SUCCESS) {
                    APP_DEBUG("GPRS Attached.\r\n");
                    m_mqtt_state = STATE_MQTT_CFG;
                }
            }
            break;
        }
        case STATE_MQTT_CFG:
            APP_DEBUG("Config MQTT...\r\n");
            RIL_MQTT_QMTCFG_Showrecvlen(connect_id,ShowFlag_1);
            RIL_MQTT_QMTCFG_Version_Select(connect_id,Version_3_1_1);
            m_mqtt_state = STATE_MQTT_OPEN;
            break;

        case STATE_MQTT_OPEN:
            APP_DEBUG("Opening Socket...\r\n");
            ret = RIL_MQTT_QMTOPEN(connect_id, HOST_NAME, HOST_PORT);
            if(ret != RIL_AT_SUCCESS) {
                 APP_DEBUG("Open Cmd Failed. Retrying...\r\n");
            }
            m_mqtt_state = STATE_IDLE; // Wait for URC
            break;

        case STATE_MQTT_CONN:
            APP_DEBUG("Authenticating...\r\n");
            ret = RIL_MQTT_QMTCONN(connect_id, clientID, username, passwd);
            m_mqtt_state = STATE_IDLE; // Wait for URC
            break;

        // ----------------------------------------------------
        // NEW GPS LOGIC
        // ----------------------------------------------------
        case STATE_GPS_CHECK:
        {
            gps_wait_counter++;
            bool fix_found = FALSE;

            Ql_memset(RMC_BUF, 0, sizeof(RMC_BUF));
            ret = RIL_GPS_Read("RMC", RMC_BUF);
            
            if (ret == RIL_AT_SUCCESS) {
                 // Try to parse
                 if (ParseRMC_Safe(RMC_BUF, &g_lat, &g_lon, &g_speed)) {
                     APP_DEBUG("GPS FIX: Lat:%.6f, Lon:%.6f\r\n", g_lat, g_lon);
                     fix_found = TRUE;
                 }
            }

            // Logic:
            // 1. If Fix Found -> Publish immediately.
            // 2. If No Fix AND Time > 60s -> Publish 0,0 to keep connection alive.
            // 3. If No Fix AND Time < 60s -> Wait (return).

            if (fix_found) {
                m_mqtt_state = STATE_MQTT_PUB;
            } 
            else {
                if ((gps_wait_counter % 5) == 0) {
                    APP_DEBUG("Searching GPS... (%d/60s)\r\n", gps_wait_counter);
                }

                if (gps_wait_counter >= 60) {
                    APP_DEBUG("!!! GPS TIMEOUT. Sending 0.0 to Dashboard !!!\r\n");
                    g_lat = 0.0;
                    g_lon = 0.0;
                    g_speed = 0.0;
                    m_mqtt_state = STATE_MQTT_PUB;
                }
            }
            break;
        }

        case STATE_MQTT_PUB:
        {
            Ql_memset(mqtt_payload, 0, sizeof(mqtt_payload));
            
            // Format JSON carefully
            Ql_sprintf((char*)mqtt_payload, 
                "{\"lat\":%d.%06d, \"lon\":%d.%06d, \"speed\":%d}", 
                (int)g_lat, (int)((g_lat - (int)g_lat)*1000000),
                (int)g_lon, (int)((g_lon - (int)g_lon)*1000000),
                (int)g_speed
            );
            // Note: using integer split for float printing to be safe on all compilers

            APP_DEBUG("PUB Payload: %s\r\n", mqtt_payload);
            pub_message_id++;

            ret = RIL_MQTT_QMTPUB(connect_id, pub_message_id, 1, 0, (u8*)MQTT_TOPIC, Ql_strlen((char*)mqtt_payload), mqtt_payload);

            if (ret == RIL_AT_SUCCESS) {
                m_mqtt_state = STATE_IDLE; // Wait for URC ACK
            } else {
                APP_DEBUG("PUB Failed: %d\r\n", ret);
                // If fail, wait a bit and retry loop
                loop_wait_tick = 0;
                m_mqtt_state = STATE_MQTT_WAIT;
            }
            break;
        }

        case STATE_MQTT_WAIT:
            loop_wait_tick++;
            if(loop_wait_tick >= 10) { // Wait 10 seconds before next cycle
                loop_wait_tick = 0;
                gps_wait_counter = 0; // Reset GPS counter for next try
                m_mqtt_state = STATE_GPS_CHECK; // Go check GPS again
            }
            break;

        case STATE_IDLE:
            break;
    }
}

// ================= HELPERS =================

static void GPS_Power_Control(bool on) {
    if (on) {
        RIL_GPS_Open(1); // Cold start usually, 1=Active
        g_gps_powered = TRUE;
    } else {
        Ql_RIL_SendATCmd("AT+QGNSSC=0", 11, NULL, NULL, 0);
        g_gps_powered = FALSE;
    }
}

// Adapted from test_gps.c (Safer than strtok)
static bool ParseRMC_Safe(char* rmc, float* lat, float* lon, float* speed)
{
    char* p = Ql_strstr(rmc, "RMC");
    if(!p) return FALSE;

    // Validate if it is valid (A = Valid, V = Invalid)
    // Format: $GNRMC,time,status,lat,NS,lon,EW,speed...
    // Count commas to find fields
    
    int comma_cnt = 0;
    char* ptr = p;
    char* lat_ptr = NULL;
    char* lon_ptr = NULL;
    char* spd_ptr = NULL;
    char status = 'V';
    char ns = 'N';
    char ew = 'E';

    while(*ptr) {
        if(*ptr == ',') {
            comma_cnt++;
            if(comma_cnt == 2) status = *(ptr+1); // Status is after 2nd comma
            if(comma_cnt == 3) lat_ptr = ptr+1;
            if(comma_cnt == 4) ns = *(ptr+1);
            if(comma_cnt == 5) lon_ptr = ptr+1;
            if(comma_cnt == 6) ew = *(ptr+1);
            if(comma_cnt == 7) spd_ptr = ptr+1;
        }
        ptr++;
    }

    if(status != 'A') return FALSE; // Not Valid

    // Convert
    if(lat_ptr && lon_ptr) {
        float raw_lat = Ql_atof(lat_ptr);
        int lat_d = (int)(raw_lat / 100);
        *lat = lat_d + (raw_lat - lat_d*100)/60.0;
        if(ns == 'S') *lat = -(*lat);

        float raw_lon = Ql_atof(lon_ptr);
        int lon_d = (int)(raw_lon / 100);
        *lon = lon_d + (raw_lon - lon_d*100)/60.0;
        if(ew == 'W') *lon = -(*lon);
        
        if(spd_ptr) *speed = Ql_atof(spd_ptr) * 1.852; // Knots to Km/h
        
        return TRUE;
    }
    return FALSE;
}

static void mqtt_recv(u8* buffer, u32 length) {
    APP_DEBUG("RX MQTT: %s\r\n", buffer);
}

static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara) {}
