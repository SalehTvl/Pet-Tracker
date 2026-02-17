#include "custom_feature_def.h"
#ifdef __OCPU_RIL_BLE_SUPPORT__
#ifdef __EXAMPLE_BLE_CLIENT__

#include "ql_stdlib.h"
#include "ql_uart.h"
#include "ql_trace.h"
#include "ql_type.h"
#include "ql_system.h"
#include "ril.h"
#include "ril_bluetooth.h"
#include "ril_ble_client.h"
#include "ql_timer.h"
#include "ql_error.h"
#include "ql_ble.h"
#include "ql_gnss.h" 



#define DEBUG_ENABLE 1
#if DEBUG_ENABLE > 0
#define DEBUG_PORT  UART_PORT1
#define DBG_BUF_LEN   512
static char DBG_BUFFER[DBG_BUF_LEN];
#define APP_DEBUG(FORMAT,...) {\
    Ql_memset(DBG_BUFFER, 0, DBG_BUF_LEN);\
    Ql_sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    if (UART_PORT2 == (DEBUG_PORT)) \
    {\
        Ql_Debug_Trace(DBG_BUFFER);\
    } else {\
        Ql_UART_Write((Enum_SerialPort)(DEBUG_PORT), (u8*)(DBG_BUFFER), Ql_strlen((const char *)(DBG_BUFFER)));\
    }\
}
#else
#define APP_DEBUG(FORMAT,...) 
#endif

#define SERIAL_RX_BUFFER_LEN  (2048)
#define BL_RX_BUFFER_LEN       (1024+1)

static Enum_SerialPort m_myUartPort  = UART_PORT1;
static u8 m_RxBuf_Uart1[SERIAL_RX_BUFFER_LEN];

ST_BLE_Client Qclient1= {0};



typedef struct
{
     s32 connect;
     u8  peer_addr[13];
} ST_BLE_Saved;
ST_BLE_Saved save = {0};

    
static void BLE_COM_Demo(void);
static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara);
static void BLE_Callback(s32 event, s32 errCode, void* param1, void* param2); 
static void Callback_ble_scan(st_bcm_gattc_dev *dev, u8 num, void *customizePara);
static s32 ATResponse_Handler(char* line, u32 len, void* userData);

 

static s32 ReadSerialPort(Enum_SerialPort port, /*[out]*/u8* pBuffer, /*[in]*/u32 bufLen)
{
    s32 rdLen = 0;
    s32 rdTotalLen = 0;
    
    if (NULL == pBuffer || 0 == bufLen)
    {
        return -1;
    }
    
    Ql_memset(pBuffer, 0x0, bufLen);
    
    while (1)
    {
        rdLen = Ql_UART_Read(port, pBuffer + rdTotalLen, bufLen - rdTotalLen);
        if (rdLen <= 0)  // All data is read out, or Serial Port Error!
        {
            break;
        }
        rdTotalLen += rdLen;
        // Continue to read...
    }
    if (rdLen < 0) // Serial Port Error!
    {
        APP_DEBUG("Fail to read from port[%d]\r\n", port);
        return -99;
    }
    return rdTotalLen;
}


static void Callback_ble_scan(st_bcm_gattc_dev *dev, u8 num, void *customizePara)
{
     u8 *customdata=customizePara;
	 u8 i=0;
     u8 j=0;

    /*If Ql_BLE_Scan_Register() is used, and scan_timeout and scan_num are not set
     *example: RIL_BT_Gatcscan(1,&Qclient1,0,0)
     */
    /*
	APP_DEBUG("RSSI:%d\r\n",dev->rssi);
	APP_DEBUG("BD_ADDR:");
	for(i=0;i<6;i++)
	{
		APP_DEBUG("%02x",dev->bd_addr[i]);
	}
	APP_DEBUG("\r\nEIR:");
	for(i=0;i<dev->eir_len;i++)
	{
		APP_DEBUG("%02x",dev->eir[i]);
	}
	APP_DEBUG("\r\n");
    */
    
    /*If Ql_BLE_Scan_Register() is used, and scan_timeout and scan_num are set
     *example: RIL_BT_Gatcscan(1,&Qclient1,5000,5)
     */
    /*
    for(i=0;i<num;i++)
    {
    	APP_DEBUG("BD_ADDR:");
	    for(j=0;j<6;j++)
	    {
		    APP_DEBUG("%02x",dev[i].bd_addr[j]);
	    }
	    APP_DEBUG("\r\n");
    }
    */

}


void proc_subtask1(s32 taskId)
{
	s32 ret;

	ST_MSG msg;
	u8 i=100;
	
	APP_DEBUG("\r\n<-- OpenCPU: proc_subtask1 -->\r\n");

	//Register here, will send a message to the current task execution callback
	//Ql_BLE_Scan_Register(Callback_ble_scan,&i);

	while (TRUE)
	{
		Ql_OS_GetMessage(&msg);
		switch(msg.message)
		{
			default:
                break;
		}
	}

}


//main function
void proc_main_task(s32 taskId)
{
    s32 ret;
    ST_MSG msg;
	u8 strAT[100];


    // Register & open UART port
    // UART port1
    ret = Ql_UART_Register(m_myUartPort, CallBack_UART_Hdlr, NULL);
    if (ret < QL_RET_OK)
    {
        Ql_Debug_Trace("Fail to register serial port[%d], ret=%d\r\n", m_myUartPort, ret);
    }
    ret = Ql_UART_Open(m_myUartPort, 115200, FC_NONE);
    if (ret < QL_RET_OK)
    {
        Ql_Debug_Trace("Fail to open serial port[%d], ret=%d\r\n", m_myUartPort, ret);
    }
	APP_DEBUG("\r\n<-- OpenCPU: BLE CLIENT Test Example-->\r\n");

    while (TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            case MSG_ID_RIL_READY:
                
                APP_DEBUG("<-- RIL is ready -->\r\n");
                
                Ql_RIL_Initialize();
                
                BLE_COM_Demo();
            	break;
            default:
                APP_DEBUG("<-- Other URC: type=%d\r\n", msg.param1);
                break;
        }
    }
}

static void CallBack_UART_Hdlr(Enum_SerialPort port, Enum_UARTEventType msg, bool level, void* customizedPara)
{
	s32 ret = RIL_AT_SUCCESS ;
    char *p = NULL;

    switch (msg)
    {
		case EVENT_UART_READY_TO_READ:
    	{
			if (m_myUartPort == port)
        	{
				s32 totalBytes = ReadSerialPort(m_myUartPort, m_RxBuf_Uart1, sizeof(m_RxBuf_Uart1));
            	if (totalBytes <= 0)
            	{
					Ql_Debug_Trace("<-- No data in UART buffer! -->\r\n");
                	return;
            	}
                // Echo
                Ql_UART_Write(m_myUartPort, m_RxBuf_Uart1, totalBytes);
                
				if(0 == Ql_memcmp(m_RxBuf_Uart1,"gatcscan=0",10))
				{
					ret=RIL_BT_Gatcscan(0,&Qclient1,0,0);
					APP_DEBUG("\r\n<--close RIL_BT_Gatcscan: ret=%d -->\r\n",ret);
                    break;
				}
				if(0 == Ql_memcmp(m_RxBuf_Uart1,"gatcscan=1",10))
				{
					ret=RIL_BT_Gatcscan(1,&Qclient1,0,0);
					APP_DEBUG("\r\n<--open RIL_BT_Gatcscan: ret=%d -->\r\n",ret);
                    break;
                }
                ret = Ql_RIL_SendATCmd((char*)m_RxBuf_Uart1, totalBytes, ATResponse_Handler, NULL, 0);
        	}    
      		break;      
		}
    	case EVENT_UART_READY_TO_WRITE:
        	break;
    	default:
        	break;
    }
}

static s32 ATResponse_Handler(char* line, u32 len, void* userData)
{
    Ql_UART_Write(UART_PORT1, (u8*)line, len);
    
    if (Ql_RIL_FindLine(line, len, "OK"))
    {  
        return  RIL_ATRSP_SUCCESS;
    }
    else if (Ql_RIL_FindLine(line, len, "ERROR"))
    {  
        return  RIL_ATRSP_FAILED;
    }
    else if (Ql_RIL_FindString(line, len, "+CME ERROR"))
    {
        return  RIL_ATRSP_FAILED;
    }
    else if (Ql_RIL_FindString(line, len, "+CMS ERROR:"))
    {
        return  RIL_ATRSP_FAILED;
    }
    return RIL_ATRSP_CONTINUE; //continue wait
}


static void BLE_Callback(s32 event, s32 errCode, void* param1, void* param2)
{
	u8 s_index,c_index;
    u8 i,j,k,l,h;

    switch(event)
    {
		case MSG_BLE_SCAN:
        {
            ST_BLE_Scan *scan = (ST_BLE_Scan *)param2;

			APP_DEBUG("client_id:%s,Peer_addr:%s,RSSI:%d,EIR:%s\r\n",Qclient1.gclient_id,scan->peer_addr,scan->RSSI,scan->EIR);    
			//APP_DEBUG("param1:%s\r\n",param1);
			
			break;
        }
        case MSG_BLE_CCON :
        {
            ST_BLE_ConnStatus *conn = (ST_BLE_ConnStatus *)param2;
            if(Ql_StrPrefixMatch((char *)param1, Qclient1.gclient_id))
            {
                Ql_memcpy(&Qclient1.conn_status,conn,sizeof(ST_BLE_ConnStatus));
            }
            APP_DEBUG("connect_status:%d,client_id:%s,Peer_addr:%s,connect_id:%d\r\n",Qclient1.conn_status.connect_status,Qclient1.gclient_id,conn->peer_addr,Qclient1.conn_status.connect_id);  
	

            break;
        }
       
        case MSG_BLE_CSS :
        {
            ST_BLE_Clie_Service *service_temp = (ST_BLE_Clie_Service *)param2;
			u8 i=0;
            
            if(Ql_StrPrefixMatch((char *)param1, Qclient1.gclient_id))
            {
				Qclient1.service_id[i].is_used=1;
                Qclient1.service_id[i].is_primary = service_temp->is_primary;
                Qclient1.service_id[i].service_inst = service_temp->service_inst;
                Ql_memcpy(&Qclient1.service_id[i].service_uuid,service_temp->service_uuid,32);
                APP_DEBUG("result:%d,client_id:%s,connect_id:%d,service:%s,inst:%d,is_primary:%d\r\n",Qclient1.result,Qclient1.gclient_id,Qclient1.conn_status.connect_id,Qclient1.service_id[i].service_uuid,Qclient1.service_id[i].service_inst,Qclient1.service_id[i].is_primary);                   
            }
			break;
        }

    	case MSG_BLE_CGC :
        {
			ST_BLE_Clie_Char *char_temp = (ST_BLE_Client *)param2;
			u8 i=0,j=0;
			
         	APP_DEBUG("Qclient1.service_id[%d].service_uuid=%s,%s\r\n",i,Qclient1.service_id[i].service_uuid,param1);
			break;
        }
        case MSG_BLE_CGD :
        {
            ST_BLE_Clie_Char *desc_temp = (ST_BLE_Clie_Char *)param2;
			u8 i=0,j=0;
      
         	APP_DEBUG("Qclient1.service_id[%d].service_uuid=%s,%s\r\n",i,Qclient1.service_id[i].service_uuid,param1);

			break;
        }
       case MSG_BLE_CRC :
        {       
            APP_DEBUG("char value from %s: %s\r\n",param1,param2);    

			break;
        }
		case MSG_BLE_CRD :
		{
		    
		    APP_DEBUG("desc value from %s:%s\r\n",param1,param2);
			break;
		}
		case MSG_BLE_CN:
		{
			ST_BLE_Notify notify_value;
			memcpy(&notify_value,(ST_BLE_Notify*)param2,sizeof(ST_BLE_Notify));
		
			APP_DEBUG("notify value from %s:%s\r\n",notify_value.char_uuid,notify_value.value);
			APP_DEBUG("from %s,%s,%d,%d\r\n",notify_value.peer_addr,notify_value.service_uuid,notify_value.conn_id,notify_value.is_notify);

			break;
		}
        default :
            break;
    }

    
}

static void BLE_COM_Demo(void)
{
    s32 cur_pwrstate = 0 ;
    s32 ret = RIL_AT_SUCCESS ;
	char strAT[100];
    
    ret = RIL_BT_GetPwrState(&cur_pwrstate);
    
    if(RIL_AT_SUCCESS != ret) 
    {
        APP_DEBUG("Get BT device power status failed.\r\n");
        //if run to here, some erros maybe occur, need reset device;
        return;
    }

    if(1 == cur_pwrstate)
    {
        APP_DEBUG("BT device already power on.\r\n");
    }
    else if(0 == cur_pwrstate)
    {
       ret = RIL_BT_Switch(1);
       if(RIL_AT_SUCCESS != ret)
       {
            APP_DEBUG("BT power on failed,ret=%d.\r\n",ret);
            return;
       }
       APP_DEBUG("BT device power on.\r\n");
    }

    RIL_BT_GetPwrState(&cur_pwrstate);
    APP_DEBUG("BT power  cur_pwrstate=%d.\r\n",cur_pwrstate);
	
	ret = RIL_BT_SetVisble(0,0);   //set BT invisble           
    if(RIL_AT_SUCCESS != ret) 
    {
        APP_DEBUG("visible failed!\r\n");
    }

    ret = RIL_BLE_Client_Initialize(BLE_Callback);

    if(RIL_AT_SUCCESS != ret) 
    {
        APP_DEBUG("BT initialization failed.\r\n");
        return;
    }
    APP_DEBUG("BT callback function register successful.\r\n");

	//for test register a client.
    Qclient1.gclient_id[0]='B';
    Qclient1.gclient_id[1]='0';
    Qclient1.gclient_id[2]='0';
    Qclient1.gclient_id[3]='1';
    Qclient1.gclient_id[4]='\0';
    Qclient1.sid=0;
    Qclient1.service_id[Qclient1.sid].cid=0;
	ret = RIL_BT_Gatcreg(1,&Qclient1);
	APP_DEBUG("\r\n<--RIL_BT_Gatcreg: ret=%d -->\r\n",ret);

	ret = RIL_BT_Gatcscan(1,&Qclient1,0,0);//start ble client scan
	APP_DEBUG("\r\n<--RIL_BT_Gatcscan: ret=%d -->\r\n",ret);
/*
    ret = RIL_BT_Gatcscan(1,&Qclient1,5000,10);//scan_timeout 5000ms  
	APP_DEBUG("\r\n<--RIL_BT_Gatcscan: ret=%d -->\r\n",ret);
*/

}

#endif
#endif
