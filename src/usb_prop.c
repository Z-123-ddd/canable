/**
 * @file    usb_prop.c
 * @brief   USB Device Property callbacks for CANable2-N32 (vendor class)
 *
 * Based on ODrive usb_prop.c.  Provides Device_Property and
 * User_Standard_Requests structures required by the N32 USB library.
 */

#include "usbfsd_lib.h"
#include "usb_conf.h"
#include "usb_prop.h"
#include "usb_desc.h"
#include "usb_pwr.h"
#include "usbd_cdc_if.h"

/* Private variables */
static uint8_t Request = 0;

/* -------------------------------------------------------------------------- */
/* Device descriptor wrapper structures                                       */
/* -------------------------------------------------------------------------- */

USB_OneDescriptor Device_Descriptor =
{
    (uint8_t*)Virtual_Com_Port_DeviceDescriptor,
    VIRTUAL_COM_PORT_SIZ_DEVICE_DESC
};

USB_OneDescriptor Config_Descriptor =
{
    (uint8_t*)Virtual_Com_Port_ConfigDescriptor,
    VIRTUAL_COM_PORT_SIZ_CONFIG_DESC
};

USB_OneDescriptor String_Descriptor[4] =
{
    {(uint8_t*)Virtual_Com_Port_StringLangID,  VIRTUAL_COM_PORT_SIZ_STRING_LANGID},
    {(uint8_t*)Virtual_Com_Port_StringVendor,   VIRTUAL_COM_PORT_SIZ_STRING_VENDOR},
    {(uint8_t*)Virtual_Com_Port_StringProduct,  VIRTUAL_COM_PORT_SIZ_STRING_PRODUCT},
    {(uint8_t*)Virtual_Com_Port_StringSerial,   VIRTUAL_COM_PORT_SIZ_STRING_SERIAL},
};
/* MS OS 2.0 string descriptor, published as index 0xEE so Windows can learn the
 * vendor request code it must use to fetch the WCID descriptor set. */
static USB_OneDescriptor Msos_String_Descriptor =
{
    (uint8_t*)Virtual_Com_Port_StringMSOS,
    VIRTUAL_COM_PORT_SIZ_STRING_MSOS
};

static USB_OneDescriptor Msos_Set_Descriptor =
{
    (uint8_t*)Virtual_Com_Port_MSOS20DescriptorSet,
    VIRTUAL_COM_PORT_SIZ_MSOS20
};


/* -------------------------------------------------------------------------- */
/* Core structures required by N32 USB library                                */
/* -------------------------------------------------------------------------- */

USB_Device Device_Table =
{
    EP_NUM,
    1
};

DEVICE_PROP Device_Property =
{
    Virtual_Com_Port_init,
    Virtual_Com_Port_Reset,
    Virtual_Com_Port_Status_In,
    Virtual_Com_Port_Status_Out,
    Virtual_Com_Port_Data_Setup,
    Virtual_Com_Port_NoData_Setup,
    Virtual_Com_Port_Get_Interface_Setting,
    Virtual_Com_Port_GetDeviceDescriptor,
    Virtual_Com_Port_GetConfigDescriptor,
    Virtual_Com_Port_GetStringDescriptor,
    0,
    0x40 /* MAX PACKET SIZE */
};

USER_STANDARD_REQUESTS User_Standard_Requests =
{
    Virtual_Com_Port_GetConfiguration,
    Virtual_Com_Port_SetConfiguration,
    Virtual_Com_Port_GetInterface,
    Virtual_Com_Port_SetInterface,
    Virtual_Com_Port_GetStatus,
    Virtual_Com_Port_ClearFeature,
    Virtual_Com_Port_SetEndPointFeature,
    Virtual_Com_Port_SetDeviceFeature,
    Virtual_Com_Port_SetDeviceAddress
};

/* -------------------------------------------------------------------------- */
/* Callback implementations                                                    */
/* -------------------------------------------------------------------------- */

void Virtual_Com_Port_init(void)
{
    pInformation->CurrentConfiguration = 0;
    PowerOn();
    USB_SilInit();

    /* Pull up DP to signal connection to host */
    _EnPortPullup();

    bDeviceState = UNCONNECTED;
}

void Virtual_Com_Port_Reset(void)
{
    pInformation->CurrentConfiguration = 0;
    pInformation->CurrentFeature = Virtual_Com_Port_ConfigDescriptor[7];
    pInformation->CurrentInterface = 0;

    USB_SetBuftab(BTABLE_ADDRESS);

    /* The host just reset the bus: endpoints lost whatever was armed, so the
     * software TX pipeline must drop its bookkeeping too. */
    cdc_tx_reset();

    /* Initialize Endpoint 0 */
    USB_SetEpType(ENDP0, EP_CONTROL);
    SetEPTxStatus(ENDP0, EP_TX_STALL);
    USB_SetEpRxAddr(ENDP0, ENDP0_RXADDR);
    USB_SetEpTxAddr(ENDP0, ENDP0_TXADDR);
    USB_ClrStsOut(ENDP0);
    USB_SetEpRxCnt(ENDP0, Device_Property.MaxPacketSize);
    USB_SetEpRxValid(ENDP0);

    /* Initialize Endpoint 1 OUT (bulk, host -> device) */
    USB_SetEpType(ENDP1, EP_BULK);
    USB_SetEpRxAddr(ENDP1, ENDP1_RXADDR);
    USB_SetEpRxCnt(ENDP1, Device_Property.MaxPacketSize);
    SetEPRxStatus(ENDP1, EP_RX_VALID);

    /* Initialize the uplink IN endpoint.  TX status stays NAK until the
     * pipeline has a packet queued (cdc_tx_arm()); arming it here would make
     * the core send uninitialised PMA on the first IN token. */
#if CDC_TX_USE_DBUF
    /* Experimental double-buffered uplink: EP3 IN owns both PMA buffers, so it
     * cannot carry OUT data (see usb_conf.h). */
    USB_SetEpType(ENDP3, EP_BULK);
    USB_SetEpDoubleBufer(ENDP3);
    USB_SetEpDblBuferAddr(ENDP3, ENDP3_TXBUF0_ADDR, ENDP3_TXBUF1_ADDR);
    USB_SetEpDblBuferCnt(ENDP3, EP_DBUF_IN, Device_Property.MaxPacketSize);
    SetEPRxStatus(ENDP3, EP_RX_NAK);
    SetEPTxStatus(ENDP3, EP_TX_NAK);
#else
    /* Default: single-buffered EP1 IN, EP1 OUT keeps the downlink. */
    USB_SetEpType(ENDP1, EP_BULK);
    USB_SetEpTxAddr(ENDP1, ENDP1_TXADDR);
    USB_SetEpTxCnt(ENDP1, Device_Property.MaxPacketSize);
    SetEPTxStatus(ENDP1, EP_TX_NAK);
#endif

    USB_SetDeviceAddress(0);
    bDeviceState = ATTACHED;
}

void Virtual_Com_Port_SetConfiguration(void)
{
    if (pInformation->CurrentConfiguration != 0)
    {
        bDeviceState = CONFIGURED;
        USB_ClrDattogTx(ENDP1);
        USB_ClrDattogRx(ENDP1);
#if CDC_TX_USE_DBUF
        USB_ClrDattogTx(ENDP3);
        USB_ClrDattogRx(ENDP3);
#endif
    }
}

void Virtual_Com_Port_SetDeviceAddress(void)
{
    bDeviceState = ADDRESSED;
}

void Virtual_Com_Port_Status_In(void)
{
    if (Request == SET_LINE_CODING)
        Request = 0;
}

void Virtual_Com_Port_Status_Out(void)
{
}

uint8_t *Virtual_Com_Port_GetMSOS20Set(uint16_t Length)
{
    return Standard_GetDescriptorData(Length, &Msos_Set_Descriptor);
}

/* IN data stage.  The only thing we serve here is the MS OS 2.0 descriptor set
 * that makes Windows bind WinUSB without an INF file. */
USB_Result Virtual_Com_Port_Data_Setup(uint8_t RequestNo)
{
    if (Type_Recipient == (VENDOR_REQUEST | DEVICE_RECIPIENT) &&
        RequestNo == USB_MSOS_VENDOR_CODE &&
        pInformation->USBwIndex0 == USB_MSOS_DESCRIPTOR_INDEX)
    {
        pInformation->Ctrl_Info.CopyData    = Virtual_Com_Port_GetMSOS20Set;
        pInformation->Ctrl_Info.Usb_wOffset = 0;
        Virtual_Com_Port_GetMSOS20Set(0);
        return Success;
    }
    return UnSupport;
}

/* No-data stage: nothing to accept in a vendor-class build (the CDC line-coding
 * and control-line requests are gone with the class). */
USB_Result Virtual_Com_Port_NoData_Setup(uint8_t RequestNo)
{
    (void)RequestNo;
    return UnSupport;
}

uint8_t *Virtual_Com_Port_GetDeviceDescriptor(uint16_t Length)
{
    return Standard_GetDescriptorData(Length, &Device_Descriptor);
}

uint8_t *Virtual_Com_Port_GetConfigDescriptor(uint16_t Length)
{
    return Standard_GetDescriptorData(Length, &Config_Descriptor);
}

uint8_t *Virtual_Com_Port_GetStringDescriptor(uint16_t Length)
{
    uint8_t wValue0 = pInformation->USBwValue0;

    if (wValue0 == USB_MSOS_STRING_INDEX)
        return Standard_GetDescriptorData(Length, &Msos_String_Descriptor);

    if (wValue0 > 4)
        return NULL;
    return Standard_GetDescriptorData(Length, &String_Descriptor[wValue0]);
}

USB_Result Virtual_Com_Port_Get_Interface_Setting(uint8_t Interface, uint8_t AlternateSetting)
{
    if (AlternateSetting > 0)
        return UnSupport;
    return Success;
}

/* Standard request stubs */
void Virtual_Com_Port_GetConfiguration(void) {}
void Virtual_Com_Port_GetInterface(void)     {}
void Virtual_Com_Port_SetInterface(void)     {}
void Virtual_Com_Port_GetStatus(void)        {}
void Virtual_Com_Port_ClearFeature(void)     {}
void Virtual_Com_Port_SetEndPointFeature(void) {}
void Virtual_Com_Port_SetDeviceFeature(void)  {}
