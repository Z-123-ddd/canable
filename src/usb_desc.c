/**
 * @file    usb_desc.c
 * @brief   USB Descriptors for CANable2-N32 (CDC ACM)
 *
 * VID/PID match original CANable2: 0x16D0 / 0x117E
 */

#include "usbfsd_lib.h"
#include "usb_desc.h"
#include "usb_conf.h"

/* USB Standard Device Descriptor */
const uint8_t Virtual_Com_Port_DeviceDescriptor[] =
{
    0x12,                       /* bLength */
    USB_DEVICE_DESCRIPTOR_TYPE, /* bDescriptorType */
    0x00, 0x02,                 /* bcdUSB = 2.00 */
    0xFF,                       /* bDeviceClass: vendor specific */
    0x00,                       /* bDeviceSubClass */
    0x00,                       /* bDeviceProtocol */
    0x40,                       /* bMaxPacketSize0 = 64 */
    0xD0, 0x16,                 /* idVendor  = 0x16D0 (Openlight Labs) */
    0x7E, 0x11,                 /* idProduct = 0x117E (CANable2) */
    0x00, 0x02,                 /* bcdDevice = 2.00 */
    1,                          /* iManufacturer string index */
    2,                          /* iProduct string index */
    3,                          /* iSerialNumber string index */
    0x01                        /* bNumConfigurations */
};

/* USB Configuration + Interface + Endpoint Descriptors (vendor specific)
 *
 * Class code 0xFF with a single data interface: no CDC ACM wrapper, no
 * notification endpoint, no class requests.  Both bulk endpoints keep their
 * original addresses (0x01 OUT / 0x81 IN) so host code and the existing
 * libusb binding (matched on VID/PID) are unaffected.
 *
 * MS OS 2.0 descriptors (see usb_prop.c) tell Windows to bind WinUSB
 * automatically, so no Zadig step is needed on a fresh machine.
 */
const uint8_t Virtual_Com_Port_ConfigDescriptor[] =
{
    /* ---------- Configuration Descriptor ---------- */
    0x09,                                   /* bLength */
    USB_CONFIGURATION_DESCRIPTOR_TYPE,      /* bDescriptorType: Configuration */
    VIRTUAL_COM_PORT_SIZ_CONFIG_DESC,       /* wTotalLength (LSB) */
    0x00,                                   /* wTotalLength (MSB) */
    0x01,                                   /* bNumInterfaces: 1 */
    0x01,                                   /* bConfigurationValue */
    0x00,                                   /* iConfiguration */
    0x80,                                   /* bmAttributes: bus powered */
    0xFA,                                   /* bMaxPower: 500mA */

    /* ---------- Interface 0: Vendor Specific ---------- */
    0x09,                               /* bLength */
    USB_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType: Interface */
    0x00,                               /* bInterfaceNumber */
    0x00,                               /* bAlternateSetting */
    0x02,                               /* bNumEndpoints: 2 (bulk OUT + IN) */
    0xFF,                               /* bInterfaceClass: vendor specific */
    0x00,                               /* bInterfaceSubClass */
    0x00,                               /* bInterfaceProtocol */
    0x00,                               /* iInterface */

    /* Endpoint 1 OUT (bulk, host -> device) */
    0x07,                               /* bLength */
    USB_ENDPOINT_DESCRIPTOR_TYPE,       /* bDescriptorType: Endpoint */
    0x01,                               /* bEndpointAddress: EP1 OUT */
    0x02,                               /* bmAttributes: Bulk */
    VIRTUAL_COM_PORT_DATA_SIZE, 0x00,   /* wMaxPacketSize: 64 */
    0x00,                               /* bInterval (ignored for bulk) */

    /* Data uplink endpoint.  Default EP1 IN (0x81); the experimental
     * double-buffered build uses EP3 IN. */
    0x07,                               /* bLength */
    USB_ENDPOINT_DESCRIPTOR_TYPE,       /* bDescriptorType: Endpoint */
#if CDC_TX_USE_DBUF
    0x83,                               /* bEndpointAddress: EP3 IN */
#else
    0x81,                               /* bEndpointAddress: EP1 IN */
#endif
    0x02,                               /* bmAttributes: Bulk */
    VIRTUAL_COM_PORT_DATA_SIZE, 0x00,   /* wMaxPacketSize: 64 */
    0x00,                               /* bInterval (ignored for bulk) */
};

/* ---------- MS OS 2.0 (WCID) descriptors --------------------------------
 *
 * Windows asks for string descriptor 0xEE, we answer "MSFT100" plus the
 * vendor request code it should use.  It then issues that vendor request with
 * wIndex = 7 (MS_OS_20_DESCRIPTOR_INDEX) and expects the descriptor set below.
 * The CompatibleID "WINUSB" makes Windows load winusb.sys without an INF, which
 * is what removes the Zadig step.
 */

/* String 0xEE: bLength | 0x03 | "MSFT100" UTF-16LE | bVendorCode | 0x00 */
const uint8_t Virtual_Com_Port_StringMSOS[VIRTUAL_COM_PORT_SIZ_STRING_MSOS] =
{
    0x12, 0x03,                                           /* bLength=18, STRING */
    'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0, /* "MSFT100" (7 chars) */
    USB_MSOS_VENDOR_CODE,                                 /* bVendorCode */
    0x00                                                  /* bReserved */
};

/* MS OS 2.0 descriptor set: Set header + Configuration subset + Function
 * subset with CompatibleID "WINUSB". */
const uint8_t Virtual_Com_Port_MSOS20DescriptorSet[VIRTUAL_COM_PORT_SIZ_MSOS20] =
{
    /* ---- Set Header ---- */
    0x0A, 0x00,             /* wLength = 10 */
    0x00, 0x00,             /* wDescriptorType = MS_OS_20_SET_HEADER */
    0x00, 0x00, 0x03, 0x06, /* dwWindowsVersion = 0x06030000 (Windows 8.1) */
    VIRTUAL_COM_PORT_SIZ_MSOS20, 0x00,  /* wTotalLength */

    /* ---- Configuration Subset Header ---- */
    0x08, 0x00,             /* wLength = 8 */
    0x01, 0x00,             /* wDescriptorType = MS_OS_20_SUBSET_HEADER_CONFIGURATION */
    0x01,                   /* bConfigurationValue = 1 */
    0x00,                   /* bReserved */
    0x20, 0x00,             /* wTotalLength = 32 (8 header + 24 function) */

    /* ---- Function Subset Header ---- */
    0x08, 0x00,             /* wLength = 8 */
    0x02, 0x00,             /* wDescriptorType = MS_OS_20_SUBSET_HEADER_FUNCTION */
    0x00,                   /* bFirstInterface = 0 */
    0x00,                   /* bReserved */
    0x18, 0x00,             /* wSubsetLength = 24 (8 header + 8 + 8 ids) */

    /* ---- Compatible ID ("WINUSB") ---- */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    /* ---- Sub-Compatible ID (unused) ---- */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ---------- String Descriptors ---------- */

const uint8_t Virtual_Com_Port_StringLangID[VIRTUAL_COM_PORT_SIZ_STRING_LANGID] =
{
    VIRTUAL_COM_PORT_SIZ_STRING_LANGID,
    USB_STRING_DESCRIPTOR_TYPE,
    0x09, 0x04     /* LangID = 0x0409: US English */
};

const uint8_t Virtual_Com_Port_StringVendor[VIRTUAL_COM_PORT_SIZ_STRING_VENDOR] =
{
    VIRTUAL_COM_PORT_SIZ_STRING_VENDOR,
    USB_STRING_DESCRIPTOR_TYPE,
    'C', 0, 'y', 0, 'b', 0, 'e', 0, 'r', 0, 'B', 0, 'e', 0, 'a', 0,
    's', 0, 't', 0
};

const uint8_t Virtual_Com_Port_StringProduct[VIRTUAL_COM_PORT_SIZ_STRING_PRODUCT] =
{
    VIRTUAL_COM_PORT_SIZ_STRING_PRODUCT,
    USB_STRING_DESCRIPTOR_TYPE,
    'C', 0, 'y', 0, 'b', 0, 'e', 0, 'r', 0, 'B', 0, 'e', 0, 'a', 0,
    's', 0, 't', 0, ' ', 0, 'U', 0, 'S', 0, 'B', 0, '2', 0, 'C', 0,
    'A', 0, 'N', 0
};

uint8_t Virtual_Com_Port_StringSerial[VIRTUAL_COM_PORT_SIZ_STRING_SERIAL] =
{
    VIRTUAL_COM_PORT_SIZ_STRING_SERIAL,
    USB_STRING_DESCRIPTOR_TYPE,
    'N', 0, '3', 0, '2', 0, 'H', 0, '4', 0, '7', 0, '3', 0, '-', 0,
    '0', 0, '0', 0, '0', 0, '1', 0,
};
