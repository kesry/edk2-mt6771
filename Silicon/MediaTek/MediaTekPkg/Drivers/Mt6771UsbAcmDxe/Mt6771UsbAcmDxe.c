/** @file

  MTU3 USB device-mode / CDC-ACM bring-up for OPPO A3 (MT6771)  --  SKELETON.

  Goal: expose the UEFI Shell through a USB serial port on the host PC, using
  the phone's USB device controller (no VBUS boost required, the host supplies
  5 V).

  This file is intentionally a skeleton: it contains only facts taken from the
  vendor/upstream sources and does NOT install any protocol, so it cannot
  disturb the on-screen shell. See docs/usb-acm.md for the staged plan.

  Facts and provenance
  --------------------
  * UDC is 11201000.usb3 (mainline docs/a3-usbnet.md:260); the MAC register
    block is 0x11201000 and the IPPC block is 0x11203E00 (stock DTB
    usb3@11200000 reg / reg-names = "mac","ippc").
  * Device registers: vendor drivers/misc/mediatek/mu3d/hal/ssusb_dev_c_header.h
      U3D_EP0CSR         SSUSB_DEV_BASE + 0x100
      U3D_RXCOUNT0       SSUSB_DEV_BASE + 0x108
      U3D_EPISR/EPIER    +0x080 / +0x084
    EP0CSR bits: SENDSTALL 25, DPHTX 20, DATAEND 19, TXPKTRDY 18,
    SETUPPKTRDY 17, RXPKTRDY 16, MAXPKTSZ0 9:0.
  * IPPC (Linux mtu3_hw_regs.h):
      IP_PW_CTRL0 +0x00  SSUSB_IP_SW_RST      bit0
      IP_PW_CTRL1 +0x04  SSUSB_IP_HOST_PDN    bit0
      IP_PW_CTRL2 +0x08  SSUSB_IP_DEV_PDN     bit0
      U2_CTRL_0P  +0x50  FORCE_IDDIG bit11, RG_IDDIG bit12, PORT_PDN bit1
  * Descriptor identity follows the mainline gadget that already works:
      idVendor 0x1d6b, idProduct 0x0105, bcdUSB 0x0200
      (mainline-linux-mt6771/script/a3-gadget-ecm.sh:117-120)
      serial 115200 8N1, no flow control (docs/a3-usbnet.md:86)

  TODO (not implemented, do not claim otherwise):
    - MTU3 device soft reset / power-on sequence from the vendor mu3d driver
    - USB PHY device-mode init (mu3phy/mt6771/mtk-phy-asic.c)
    - endpoint configuration and FIFO sizing
    - EP0 control state machine and descriptor responses
    - bulk data paths and EFI_SERIAL_IO_PROTOCOL for TerminalDxe

  Copyright (c) 2026, kesry. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

/* ---- controller bases (see file header) ---- */
#define MT6771_UDC_MAC_BASE      0x11201000u
#define MT6771_UDC_IPPC_BASE     0x11203E00u
#define MT6771_UDC_XHCI_BASE     0x11200000u
#define MT6771_UDC_PHY_BASE      0x11F40000u

/* ---- device registers, relative to MT6771_UDC_MAC_BASE ---- */
#define U3D_LV1ISR               0x0000u
#define U3D_LV1IER               0x0004u
#define U3D_LV1IESR              0x0008u
#define U3D_LV1IECR              0x000Cu
#define U3D_EPISR                0x0080u
#define U3D_EPIER                0x0084u
#define U3D_EPIESR               0x0088u
#define U3D_EPIECR               0x008Cu
#define U3D_EP0CSR               0x0100u
#define U3D_RXCOUNT0             0x0108u

/* ---- EP0CSR bits ---- */
#define EP0_SENDSTALL            BIT25
#define EP0_DPHTX                BIT20
#define EP0_DATAEND              BIT19
#define EP0_TXPKTRDY             BIT18
#define EP0_SETUPPKTRDY          BIT17
#define EP0_RXPKTRDY             BIT16
#define EP0_MAXPKTSZ0_MASK       0x3FFu

/* ---- IPPC, relative to MT6771_UDC_IPPC_BASE ---- */
#define IP_PW_CTRL0              0x0000u
#define IP_PW_CTRL1              0x0004u
#define IP_PW_CTRL2              0x0008u
#define U2_CTRL_0P               0x0050u

#define SSUSB_IP_SW_RST          BIT0
#define SSUSB_IP_HOST_PDN        BIT0
#define SSUSB_IP_DEV_PDN         BIT0
#define SSUSB_U2_PORT_PDN        BIT1
#define SSUSB_U2_PORT_FORCE_IDDIG BIT11
#define SSUSB_U2_PORT_RG_IDDIG    BIT12

/* ---- CDC-ACM identity, same values the mainline gadget uses ---- */
#define ACM_ID_VENDOR            0x1D6Bu
#define ACM_ID_PRODUCT           0x0105u
#define ACM_BCD_USB              0x0200u
#define ACM_LINE_CODING_115200   115200u

#pragma pack(1)
typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT16  BcdUSB;
  UINT8   DeviceClass;
  UINT8   DeviceSubClass;
  UINT8   DeviceProtocol;
  UINT8   MaxPacketSize0;
  UINT16  IdVendor;
  UINT16  IdProduct;
  UINT16  BcdDevice;
  UINT8   IManufacturer;
  UINT8   IProduct;
  UINT8   ISerialNumber;
  UINT8   NumConfigurations;
} USB_DEVICE_DESCRIPTOR_ACM;
#pragma pack()

STATIC CONST USB_DEVICE_DESCRIPTOR_ACM  mDeviceDescriptor = {
  sizeof (USB_DEVICE_DESCRIPTOR_ACM), 0x01,      /* DEVICE */
  ACM_BCD_USB,
  0x02, 0x00, 0x00,                              /* CDC, class level */
  64,                                            /* EP0 max packet */
  ACM_ID_VENDOR, ACM_ID_PRODUCT,
  0x0100,                                        /* bcdDevice */
  1, 2, 3,                                       /* string indices */
  1
};

/* ---- USB PHY, vendor mu3phy/mt6771/mtk-phy-asic.h ---- */
#define MT6771_U3PHY_BASE        0x11F40000u
#define PHY_U2PHY_COM_BASE       (MT6771_U3PHY_BASE + 0x0300u)
#define PHY_U2PHYDTM0            (PHY_U2PHY_COM_BASE + 0x0068u)
#define PHY_U2PHYDTM1            (PHY_U2PHY_COM_BASE + 0x006Cu)
#define PHY_FORCE_IDDIG          BIT9
#define PHY_RG_IDDIG             BIT1

/* CDC-ACM class codes (USB CDC 1.2) */
#define CDC_CLASS_COMM           0x02
#define CDC_CLASS_DATA           0x0A
#define CDC_SUBCLASS_ACM         0x02
#define CDC_PROTOCOL_AT          0x01
#define USB_CLASS_CDC_DATA       0x0A
#define USB_DT_INTERFACE         0x04
#define USB_DT_ENDPOINT          0x05
#define USB_DT_CS_INTERFACE      0x24
#define USB_ENDPOINT_XFER_INT    0x03
#define USB_ENDPOINT_XFER_BULK   0x02

#pragma pack(1)
typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT16  BcdUSB;
  UINT8   DeviceClass;
  UINT8   DeviceSubClass;
  UINT8   DeviceProtocol;
  UINT8   MaxPacketSize0;
  UINT16  IdVendor;
  UINT16  IdProduct;
  UINT16  BcdDevice;
  UINT8   IManufacturer;
  UINT8   IProduct;
  UINT8   ISerialNumber;
  UINT8   NumConfigurations;
} ACM_DEVICE_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT16  TotalLength;
  UINT8   NumInterfaces;
  UINT8   ConfigurationValue;
  UINT8   IConfiguration;
  UINT8   Attributes;
  UINT8   MaxPower;
} ACM_CONFIG_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   InterfaceNumber;
  UINT8   AlternateSetting;
  UINT8   NumEndpoints;
  UINT8   InterfaceClass;
  UINT8   InterfaceSubClass;
  UINT8   InterfaceProtocol;
  UINT8   IInterface;
} ACM_INTERFACE_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   Subtype;
  UINT8   Data[5];              /* header functional descriptor payload */
} ACM_HEADER_FUNC_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   Subtype;
  UINT8   Capabilities;
  UINT8   Data[3];
} ACM_CALL_MGMT_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   Subtype;
  UINT8   Data[4];              /* ACM functional descriptor payload */
} ACM_ACM_FUNC_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   Subtype;
  UINT8   Data[2];
} ACM_UNION_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   EndpointAddress;
  UINT8   Attributes;
  UINT16  MaxPacketSize;
  UINT8   Interval;
} ACM_ENDPOINT_DESC;
#pragma pack()

/*
 * Endpoint numbers and packet sizes are the standard CDC-ACM layout; they are
 * NOT yet validated against MTU3 FIFO sizing (TODO in docs/usb-acm.md).
 */
#define ACM_EP_NOTIFY   0x83u   /* EP1 IN  interrupt */
#define ACM_EP_DATA_OUT 0x02u   /* EP2 OUT bulk */
#define ACM_EP_DATA_IN  0x82u   /* EP2 IN  bulk */

STATIC CONST UINT8  mConfigDescriptor[] = {
  /* Configuration */
  0x09, 0x02, 0x43, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
  /* Interface 0: CDC Communication */
  0x09, USB_DT_INTERFACE, 0x00, 0x00, 0x01, CDC_CLASS_COMM,
  CDC_SUBCLASS_ACM, CDC_PROTOCOL_AT, 0x00,
  /* Header functional */
  0x05, USB_DT_CS_INTERFACE, 0x00, 0x10, 0x01,
  /* Call management */
  0x05, USB_DT_CS_INTERFACE, 0x01, 0x00, 0x01,
  /* ACM functional */
  0x04, USB_DT_CS_INTERFACE, 0x02, 0x02,
  /* Union */
  0x05, USB_DT_CS_INTERFACE, 0x06, 0x00, 0x01,
  /* EP1 IN interrupt */
  0x07, USB_DT_ENDPOINT, ACM_EP_NOTIFY, USB_ENDPOINT_XFER_INT, 0x08, 0x00, 0x10,
  /* Interface 1: CDC Data */
  0x09, USB_DT_INTERFACE, 0x01, 0x00, 0x02, CDC_CLASS_DATA, 0x00, 0x00, 0x00,
  /* EP2 OUT bulk */
  0x07, USB_DT_ENDPOINT, ACM_EP_DATA_OUT, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0x00,
  /* EP2 IN bulk */
  0x07, USB_DT_ENDPOINT, ACM_EP_DATA_IN, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0x00,
};

/* ------------------------------------------------------------------ */
/* EP0 PIO helpers (vendor mu3d_hal_write_fifo/read_fifo)               */
/* ------------------------------------------------------------------ */
STATIC
VOID
Ep0WriteFifo (
  IN CONST UINT8  *Buf,
  IN UINTN        Length
  )
{
  UINTN  i;
  UINT32 Word;

  for (i = 0; i < Length; i += 4) {
    Word = (UINT32)Buf[i];
    if (i + 1 < Length) Word |= (UINT32)Buf[i + 1] << 8;
    if (i + 2 < Length) Word |= (UINT32)Buf[i + 2] << 16;
    if (i + 3 < Length) Word |= (UINT32)Buf[i + 3] << 24;
    MmioWrite32 (U3D_FIFO0, Word);
  }
}

STATIC
UINTN
Ep0ReadFifo (
  OUT UINT8  *Buf,
  IN  UINTN  MaxLength
  )
{
  UINTN  Count;
  UINTN  i;
  UINT32 Word;

  Count = MmioRead32 (MT6771_UDC_MAC_BASE + U3D_RXCOUNT0) & 0x3FFu;
  if (Count > MaxLength) {
    Count = MaxLength;
  }

  for (i = 0; i < Count; i += 4) {
    Word = MmioRead32 (U3D_FIFO0);
    Buf[i] = (UINT8)(Word & 0xFF);
    if (i + 1 < Count) Buf[i + 1] = (UINT8)((Word >> 8) & 0xFF);
    if (i + 2 < Count) Buf[i + 2] = (UINT8)((Word >> 16) & 0xFF);
    if (i + 3 < Count) Buf[i + 3] = (UINT8)((Word >> 24) & 0xFF);
  }
  return Count;
}

STATIC
VOID
Ep0SetTx (
  IN CONST UINT8  *Data,
  IN UINTN        Length,
  IN UINTN        MaxPacket
  )
{
  UINTN  Send;

  Send = Length < MaxPacket ? Length : MaxPacket;
  Ep0WriteFifo (Data, Send);

  if (Send == 0) {
    MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
                 MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) |
                 EP0_DATAEND | EP0_TXPKTRDY);
  } else {
    MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
                 MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) | EP0_TXPKTRDY);
  }
}

/* String descriptors: minimal, index 0/1/2 */
STATIC CONST UINT8  mString0[] = { 4, 0x03, 0x09, 0x04 };
STATIC CONST UINT8  mString1[] = { 8, 0x03, 'k', 0, 'e', 0, 's', 0 };
STATIC CONST UINT8  mString2[] = { 14, 0x03, 'A', 0, '3', 0, ' ', 0, 'A', 0, 'C', 0, 'M', 0 };

/**
  Handle one EP0 SETUP packet. This implements the standard requests needed to
  enumerate a CDC-ACM device. Class-specific requests (SET_LINE_CODING etc.)
  are ACKed with a zero-length status stage; the defaults already match
  115200 8N1 that the mainline gadget used.
**/
STATIC
VOID
HandleSetup (
  IN CONST USB_SETUP_PACKET  *Setup
  )
{
  UINT8  Type = Setup->RequestType & 0x1F;

  DEBUG ((DEBUG_INFO, "AcmEp0: req=%02x type=%02x val=%04x idx=%04x len=%u
",
          Setup->Request, Setup->RequestType, Setup->Value,
          Setup->Index, Setup->Length));

  switch (Setup->Request) {
  case USB_REQ_GET_STATUS:
    {
      UINT8 Status[2] = { 0, 0 };
      Ep0SetTx (Status, 2, 64);
    }
    return;

  case USB_REQ_SET_ADDRESS:
    /* the controller latches the address when the status stage completes */
    MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
                 MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) | EP0_DATAEND);
    return;

  case USB_REQ_GET_DESCRIPTOR:
    switch (Setup->Value >> 8) {
    case USB_DT_DEVICE:
      Ep0SetTx ((CONST UINT8 *)&mDeviceDescriptor,
                sizeof (mDeviceDescriptor), 64);
      return;
    case USB_DT_CONFIG:
      Ep0SetTx (mConfigDescriptor, sizeof (mConfigDescriptor), 64);
      return;
    case USB_DT_STRING:
      switch (Setup->Value & 0xFF) {
      case 0: Ep0SetTx (mString0, sizeof (mString0), 64); return;
      case 1: Ep0SetTx (mString1, sizeof (mString1), 64); return;
      case 2: Ep0SetTx (mString2, sizeof (mString2), 64); return;
      }
      break;
    }
    break;

  case USB_REQ_GET_CONFIGURATION:
    {
      UINT8 Config = 1;
      Ep0SetTx (&Config, 1, 64);
    }
    return;

  case USB_REQ_SET_CONFIGURATION:
    MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
                 MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) | EP0_DATAEND);
    return;

  default:
    break;
  }

  /* class/vendor requests and unhandled standard ones: ACK with a ZLP */
  MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
               MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) | EP0_DATAEND);
}

/**
  EP0 interrupt handler (called from the main polling loop in this bring-up
  version; no EFI_EVENT is used so it works before the shell starts).
**/
STATIC
VOID
AcmEp0Poll (
  VOID
  )
{
  UINT32  Lv1;
  UINT32  Csr;

  Lv1 = MmioRead32 (U3D_LV1ISR);
  if ((Lv1 & LV1_EP0) == 0) {
    return;
  }
  MmioWrite32 (U3D_LV1ISR, LV1_EP0);

  Csr = MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR);
  if ((Csr & EP0_SETUPPKTRDY) != 0) {
    USB_SETUP_PACKET  Setup;
    UINTN             Got;

    Got = Ep0ReadFifo ((UINT8 *)&Setup, sizeof (Setup));
    MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
                 MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) | EP0_SETUPPKTRDY);
    if (Got == sizeof (Setup)) {
      HandleSetup (&Setup);
    }
  } else if ((Csr & EP0_RXPKTRDY) != 0) {
    UINT8  Sink[64];
    Ep0ReadFifo (Sink, sizeof (Sink));
    MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
                 MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) | EP0_RXPKTRDY);
  }
}

STATIC
VOID
IpccWrite (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (MT6771_UDC_IPPC_BASE + Offset, Value);
}

STATIC
UINT32
IpccRead (
  IN UINTN  Offset
  )
{
  return MmioRead32 (MT6771_UDC_IPPC_BASE + Offset);
}

/**
  Put the MTU3 wrapper into device (peripheral) mode.

  Mirrors Linux mtu3_dr.c ssusb_set_force_mode(MTU3_DR_FORCE_DEVICE):
  force IDDIG and pull the U2 port out of power-down, with the host IP powered
  down and the device IP powered up. Register bits: see file header.
**/
STATIC
VOID
Mtu3SwitchToDevice (
  VOID
  )
{
  UINT32  Value;

  /* whole-IP software reset */
  IpccWrite (IP_PW_CTRL0, IpccRead (IP_PW_CTRL0) | SSUSB_IP_SW_RST);
  MicroSecondDelay (10);
  IpccWrite (IP_PW_CTRL0, IpccRead (IP_PW_CTRL0) & ~SSUSB_IP_SW_RST);
  MicroSecondDelay (1000);

  /* power down host IP, power up device IP */
  IpccWrite (IP_PW_CTRL1, IpccRead (IP_PW_CTRL1) | SSUSB_IP_HOST_PDN);
  IpccWrite (IP_PW_CTRL2, IpccRead (IP_PW_CTRL2) & ~SSUSB_IP_DEV_PDN);

  /* force device role and enable the U2 port */
  Value  = IpccRead (U2_CTRL_0P);
  Value |= SSUSB_U2_PORT_FORCE_IDDIG | SSUSB_U2_PORT_RG_IDDIG;
  Value &= ~SSUSB_U2_PORT_PDN;
  IpccWrite (U2_CTRL_0P, Value);

  MicroSecondDelay (20000);
}

/**
  Device-mode power-on, following the vendor mu3d_hal_ssusb_en() /
  mu3d_hal_rst_dev() exactly. See docs/usb-acm.md for provenance.
**/
STATIC
EFI_STATUS
AcmDeviceInit (
  VOID
  )
{
  UINT32  Value;
  UINTN   Wait;

  /* whole-IP reset */
  Mtu3SwitchToDevice ();

  /* device IP power on, U3/U2 port0 out of reset, device role (not host) */
  MmioWrite32 (U3D_SSUSB_IP_PW_CTRL2,
               MmioRead32 (U3D_SSUSB_IP_PW_CTRL2) & ~SSUSB_IP_DEV_PDN);

  Value  = MmioRead32 (U3D_SSUSB_U3_CTRL_0P);
  Value &= ~(SSUSB_U3_PORT_DIS | SSUSB_U3_PORT_PDN | SSUSB_U3_PORT_HOST_SEL);
  MmioWrite32 (U3D_SSUSB_U3_CTRL_0P, Value);

  /*
   * OTG: MAC auto-select + OTG_SEL, and bring U2 out of reset (vendor
   * mu3d_hal_ssusb_en with SUPPORT_OTG).
   */
  Value  = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
  Value |= (SSUSB_U2_PORT_OTG_MAC_AUTO_SEL | SSUSB_U2_PORT_OTG_SEL);
  Value &= ~(SSUSB_U2_PORT_DIS | SSUSB_U2_PORT_PDN | SSUSB_U2_PORT_HOST_SEL);
  MmioWrite32 (U3D_SSUSB_U2_CTRL_0P, Value);

  /* release reference clock gates (vendor sets these four bits) */
  Value  = MmioRead32 (U3D_SSUSB_REF_CK_CTRL);
  Value |= (SSUSB_REF_MAC_CK_GATE_EN | SSUSB_REF_PHY_CK_GATE_EN |
            SSUSB_REF_CK_GATE_EN | SSUSB_REF_MAC3_CK_GATE_EN);
  MmioWrite32 (U3D_SSUSB_REF_CK_CTRL, Value);

  /* device controller soft reset (vendor mu3d_hal_rst_dev) */
  MmioWrite32 (U3D_SSUSB_DEV_RST_CTRL, SSUSB_DEV_SW_RST);
  MmioWrite32 (U3D_SSUSB_DEV_RST_CTRL, 0);

  /* wait for sys125 / U3 MAC / U2 MAC reset-deassert bits */
  for (Wait = 0; Wait < 100; Wait++) {
    if ((MmioRead32 (U3D_SSUSB_IP_PW_STS1) & SSUSB_SYS125_RST_B_STS) != 0) {
      break;
    }
    MicroSecondDelay (1000);
  }
  for (Wait = 0; Wait < 100; Wait++) {
    if ((MmioRead32 (U3D_SSUSB_IP_PW_STS2) & SSUSB_U2_MAC_SYS_RST_B_STS) != 0) {
      break;
    }
    MicroSecondDelay (1000);
  }

  /* configure EP0 max packet and enable the EP0 interrupt */
  MmioWrite32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR,
               MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR) |
               EP0_MAXPKTSZ0_MASK);
  MmioWrite32 (U3D_LV1IESR, LV1_EP0);

  DEBUG ((DEBUG_INFO,
    "AcmDevice: STS1=%08x STS2=%08x EP0CSR=%08x
",
    MmioRead32 (U3D_SSUSB_IP_PW_STS1),
    MmioRead32 (U3D_SSUSB_IP_PW_STS2),
    MmioRead32 (MT6771_UDC_MAC_BASE + U3D_EP0CSR)));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Mt6771UsbAcmDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       Iterations;

  Status = AcmDeviceInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /*
   * Poll EP0 for a bounded window. This bring-up version does not install
   * EFI_SERIAL_IO_PROTOCOL yet; the loop is here so a device attached before
   * the shell starts can enumerate while the firmware is still loading.
   * TODO: replace with a TPL_CALLBACK timer once the bulk data path exists.
   */
  for (Iterations = 0; Iterations < 20000; Iterations++) {
    AcmEp0Poll ();
    MicroSecondDelay (10);
  }

  return EFI_SUCCESS;
}
