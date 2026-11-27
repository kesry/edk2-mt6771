/** @file

  MT6771 USB host (MTU3 / xHCI) bring-up for OPPO A3.

  Purpose: make the phone's USB-C port work in HOST mode under UEFI so that
  a USB keyboard (input), USB mass storage (ISOs) and USB-serial adapters can
  be used from the UEFI Shell.

  Hardware facts (decoded from the bootable device tree
  mainline-linux-mt6771/vendor/a3-v186-tgpio.dtb, node offsets 0x9b80 / 0x9cd4
  / 0x9dd0):

    usb3@11200000          compatible "mediatek,mt6771-mtu3"
      reg = <0x11201000 0x3000>, <0x11203e00 0x100>
      reg-names = "mac", "ippc"
    usb3_xhci@11200000     compatible "mediatek,mt677x-xhci"
      reg = <0x11200000 0x1000>          <- xHCI MMIO window
    usb0phy@11f40000       compatible "mediatek,mt6771-phy"
      reg = <0x11f40000 0x10000>, <0x11203e00 0x100>
      reg-names = "sif_base", "ippc"

  IPPC register offsets/bits are from Linux v6.12
  drivers/usb/mtu3/mtu3_hw_regs.h (SSUSB_SIFSLV_IPPC_BASE = 0x0):
    U3D_SSUSB_IP_PW_CTRL0  +0x0000   SSUSB_IP_SW_RST     BIT(0)
    U3D_SSUSB_IP_PW_CTRL1  +0x0004   SSUSB_IP_HOST_PDN   BIT(0)
    U3D_SSUSB_IP_PW_CTRL2  +0x0008   SSUSB_IP_DEV_PDN    BIT(0)
    U3D_SSUSB_U2_CTRL_0P   +0x0050   FORCE_IDDIG BIT(11), RG_IDDIG BIT(12),
                                     PORT_PDN BIT(1)
  and the host sequence from mtu3_plat.c:ssusb_ip_sw_reset() plus
  mtu3_dr.c:ssusb_set_force_mode()/switch_port_to_host().

  TODO / UNVERIFIED on hardware:
    - MTU3 MAC clock and PHY power are assumed to be left enabled by LK.
      There is no MT6771 clock driver in this firmware; if the controller
      stays unclocked, xHCI register reads return 0 and no USB device appears.
    - The USB PHY (0x11f40000) is not programmed here. If host mode needs an
      explicit PHY power-up on this board, it must be added.
    - VBUS/OTG supply switching is not implemented.

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
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PrintLib.h>

//
// MMIO bases (see file header for provenance).
//
#define MT6771_XHCI_BASE      0x11200000u
#define MT6771_XHCI_SIZE      0x00001000u
#define MT6771_IPPC_BASE      0x11203E00u

//
// IPPC registers.
//
#define U3D_SSUSB_IP_PW_CTRL0 (MT6771_IPPC_BASE + 0x0000u)
#define U3D_SSUSB_IP_PW_CTRL1 (MT6771_IPPC_BASE + 0x0004u)
#define U3D_SSUSB_IP_PW_CTRL2 (MT6771_IPPC_BASE + 0x0008u)
#define U3D_SSUSB_U3_CTRL_0P  (MT6771_IPPC_BASE + 0x0030u)
#define U3D_SSUSB_U2_CTRL_0P  (MT6771_IPPC_BASE + 0x0050u)
#define U3D_SSUSB_IP_PW_STS1  (MT6771_IPPC_BASE + 0x0010u)

#define SSUSB_IP_SW_RST       BIT0
#define SSUSB_IP_HOST_PDN     BIT0
#define SSUSB_IP_DEV_PDN      BIT0
#define SSUSB_U2_PORT_DIS     BIT0
#define SSUSB_U2_PORT_HOST    BIT2
#define SSUSB_U2_PORT_PDN     BIT1
#define SSUSB_U2_PORT_VBUSVALID BIT9
#define SSUSB_U2_PORT_FORCE_IDDIG BIT11
#define SSUSB_U2_PORT_RG_IDDIG   BIT12
#define SSUSB_U2_PORT_HOST_SEL   (SSUSB_U2_PORT_VBUSVALID | SSUSB_U2_PORT_HOST)
#define SSUSB_U3_PORT_DIS     BIT0
#define SSUSB_U3_PORT_PDN     BIT1
#define SSUSB_U3_PORT_HOST_SEL BIT2
#define SSUSB_XHCI_RST_B_STS  BIT11

//
// The RELEASE build maps SerialPortLib to UART0, which is not wired on this
// board, so DEBUG() output is invisible. Print to the UEFI console instead:
// this driver depends on SimpleTextOut, so ConOut exists when it runs.
//
#define USB_LOG_LINES 16

STATIC CHAR16  mLogBuf[USB_LOG_LINES][144];
STATIC UINTN   mLogUsed;
STATIC UINTN   mLogReplays;

STATIC
VOID
ScreenLog (
  IN CONST CHAR16  *Text
  )
{
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
  }
  if (mLogUsed < USB_LOG_LINES) {
    StrCpyS (mLogBuf[mLogUsed], 144, (CHAR16 *)Text);
    mLogUsed++;
  }
}

STATIC
VOID
EFIAPI
UsbLogReplay (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN  i;

  if (mLogReplays >= 3) {
    gBS->SetTimer (Event, TimerCancel, 0);
    return;
  }
  mLogReplays++;

  if ((gST == NULL) || (gST->ConOut == NULL)) {
    return;
  }
  gST->ConOut->OutputString (gST->ConOut, L"\r\n==== A3 USB host trace ====\r\n");
  for (i = 0; i < mLogUsed; i++) {
    gST->ConOut->OutputString (gST->ConOut, mLogBuf[i]);
  }
  gST->ConOut->OutputString (gST->ConOut, L"==== end USB host trace ====\r\n");
}

STATIC
VOID
Mtu3SetBits (
  IN UINTN  Address,
  IN UINT32 Bits
  )
{
  MmioWrite32 (Address, MmioRead32 (Address) | Bits);
}

STATIC
VOID
Mtu3ClearBits (
  IN UINTN  Address,
  IN UINT32 Bits
  )
{
  MmioWrite32 (Address, MmioRead32 (Address) & ~Bits);
}

/**
  Put the MTU3 wrapper into host mode and power up the xHCI block.

  Sequence mirrors Linux mtu3_plat.c / mtu3_dr.c:
    1. whole-IP software reset
    2. power down the device (gadget) IP - LK leaves it powered for fastboot
    3. power up the host IP
    4. force the U2 port IDDIG to host and clear the port power-down
**/
STATIC
VOID
Mt6771UsbHostInit (
  VOID
  )
{
  UINT32  Value;
  UINT32  PwSts;
  UINTN   Wait;

  ScreenLog (L"USB: host init (upstream mtu3_host sequence)\r\n");

  // 1. whole-IP software reset (mtu3_plat.c:ssusb_ip_sw_reset)
  Mtu3SetBits (U3D_SSUSB_IP_PW_CTRL0, SSUSB_IP_SW_RST);
  MicroSecondDelay (10);
  Mtu3ClearBits (U3D_SSUSB_IP_PW_CTRL0, SSUSB_IP_SW_RST);
  MicroSecondDelay (1000);

  // 2. the device IP is up because LK used fastboot: power it down so the
  //    host-only configuration can reach ip-sleep (mtu3_plat.c comment).
  Mtu3SetBits (U3D_SSUSB_IP_PW_CTRL2, SSUSB_IP_DEV_PDN);

  // 3. power on host IP (mtu3_host.c:ssusb_host_setup)
  Mtu3ClearBits (U3D_SSUSB_IP_PW_CTRL1, SSUSB_IP_HOST_PDN);

  // 4. U2 port0: power off+disable, then power on+enable and select host mode.
  //    HOST_SEL = VBUSVALID | HOST is what actually puts the port in host mode;
  //    last night's version cleared PDN only and never set HOST_SEL.
  Value  = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
  Value |= (SSUSB_U2_PORT_PDN | SSUSB_U2_PORT_DIS);
  MmioWrite32 (U3D_SSUSB_U2_CTRL_0P, Value);

  Value  = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
  Value &= ~(SSUSB_U2_PORT_PDN | SSUSB_U2_PORT_DIS);
  Value |= SSUSB_U2_PORT_HOST_SEL;
  MmioWrite32 (U3D_SSUSB_U2_CTRL_0P, Value);

  // 5. U3 port0, same treatment (mtu3_dr.c:ssusb_port0_switch USB3 branch)
  Value  = MmioRead32 (U3D_SSUSB_U3_CTRL_0P);
  Value |= (SSUSB_U3_PORT_PDN | SSUSB_U3_PORT_DIS);
  MmioWrite32 (U3D_SSUSB_U3_CTRL_0P, Value);

  Value  = MmioRead32 (U3D_SSUSB_U3_CTRL_0P);
  Value &= ~(SSUSB_U3_PORT_PDN | SSUSB_U3_PORT_DIS);
  Value |= SSUSB_U3_PORT_HOST_SEL;
  MmioWrite32 (U3D_SSUSB_U3_CTRL_0P, Value);

  // 6. force the ID pin to host (mtu3_dr.c:ssusb_set_force_mode FORCE_HOST)
  Value  = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
  Value |= SSUSB_U2_PORT_FORCE_IDDIG;
  Value &= ~SSUSB_U2_PORT_RG_IDDIG;
  MmioWrite32 (U3D_SSUSB_U2_CTRL_0P, Value);

  // 7. wait for the xHCI reset status bit: this is the authoritative
  //    "the host controller is really clocked" signal
  //    (mtu3_host.c:ssusb_check_clocks, SSUSB_XHCI_RST_B_STS).
  PwSts = 0;
  for (Wait = 0; Wait < 200; Wait++) {
    PwSts = MmioRead32 (U3D_SSUSB_IP_PW_STS1);
    if ((PwSts & SSUSB_XHCI_RST_B_STS) != 0) {
      break;
    }
    MicroSecondDelay (1000);
  }

  {
    CHAR16  Buf[192];
    UnicodeSPrint (
      Buf, sizeof (Buf),
      L"USB: IPPC1=%08x U2=%08x U3=%08x PWSTS1=%08x xhci_rst=%u\r\n",
      MmioRead32 (U3D_SSUSB_IP_PW_CTRL1),
      MmioRead32 (U3D_SSUSB_U2_CTRL_0P),
      MmioRead32 (U3D_SSUSB_U3_CTRL_0P),
      PwSts,
      (PwSts & SSUSB_XHCI_RST_B_STS) ? 1u : 0u
      );
    ScreenLog (Buf);
    if ((PwSts & SSUSB_XHCI_RST_B_STS) == 0) {
      ScreenLog (L"USB: xHCI clock NOT ready (U3 MAC or xhci clock off)\r\n");
    } else {
      ScreenLog (L"USB: xHCI clock ready -> host controller running\r\n");
    }
  }
}

EFI_STATUS
EFIAPI
Mt6771UsbHostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      CapLength;

  ScreenLog (L"USB: Mt6771UsbHostDxe start\r\n");

  //
  // Sanity check before touching the wrapper: a live xHCI controller reports a
  // non-zero CAPLENGTH in the low byte of its capability register.
  //
  CapLength = MmioRead32 (MT6771_XHCI_BASE) & 0xFFu;
  {
    CHAR16 Buf[128];
    UnicodeSPrint (Buf, sizeof (Buf), L"USB: xHCI CAP0=%08x caplen=%u\r\n",
                   MmioRead32 (MT6771_XHCI_BASE), CapLength);
    ScreenLog (Buf);
  }

  Mt6771UsbHostInit ();

  CapLength = MmioRead32 (MT6771_XHCI_BASE) & 0xFFu;
  {
    CHAR16 Buf[128];
    UnicodeSPrint (Buf, sizeof (Buf), L"USB: xHCI after init CAP0=%08x caplen=%u\r\n",
                   MmioRead32 (MT6771_XHCI_BASE), CapLength);
    ScreenLog (Buf);
    if (CapLength == 0) {
      ScreenLog (L"USB: caplen=0 -> controller NOT clocked / PHY off\r\n");
    } else {
      ScreenLog (L"USB: caplen!=0 -> controller alive, registering\r\n");
    }
  }

  //
  // Expose the xHCI MMIO window as a non-discoverable PCI device so that the
  // stock MdeModulePkg XhciDxe driver can bind to it.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             (UINTN)MT6771_XHCI_BASE,
             (UINTN)MT6771_XHCI_SIZE
             );
  {
    CHAR16 Buf[128];
    UnicodeSPrint (Buf, sizeof (Buf), L"USB: register xHCI -> %r\r\n", Status);
    ScreenLog (Buf);
  }

  {
    EFI_EVENT  ReplayEvent = NULL;
    if (!EFI_ERROR (gBS->CreateEvent (
                      EVT_TIMER | EVT_NOTIFY_SIGNAL,
                      TPL_CALLBACK,
                      UsbLogReplay,
                      NULL,
                      &ReplayEvent
                      )))
    {
      gBS->SetTimer (ReplayEvent, TimerPeriodic, 30000000u);
    }
  }

  return Status;
}
