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
#define U3D_SSUSB_U2_CTRL_0P  (MT6771_IPPC_BASE + 0x0050u)

#define SSUSB_IP_SW_RST       BIT0
#define SSUSB_IP_HOST_PDN     BIT0
#define SSUSB_IP_DEV_PDN      BIT0
#define SSUSB_U2_PORT_PDN     BIT1
#define SSUSB_U2_PORT_FORCE_IDDIG BIT11
#define SSUSB_U2_PORT_RG_IDDIG   BIT12

//
// The RELEASE build maps SerialPortLib to UART0, which is not wired on this
// board, so DEBUG() output is invisible. Print to the UEFI console instead:
// this driver depends on SimpleTextOut, so ConOut exists when it runs.
//
STATIC
VOID
ScreenLog (
  IN CONST CHAR16  *Text
  )
{
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
  }
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
  UINT32 Ctrl0;
  UINT32 Ctrl1;
  UINT32 Ctrl2;
  UINT32 U2Ctrl;

  Ctrl0 = MmioRead32 (U3D_SSUSB_IP_PW_CTRL0);
  {
    CHAR16 Buf[128];
    UnicodeSPrint (Buf, sizeof (Buf), L"USB: IPPC before  CTRL0=%08x CTRL1=%08x CTRL2=%08x U2=%08x\r\n",
                   MmioRead32 (U3D_SSUSB_IP_PW_CTRL0), MmioRead32 (U3D_SSUSB_IP_PW_CTRL1),
                   MmioRead32 (U3D_SSUSB_IP_PW_CTRL2), MmioRead32 (U3D_SSUSB_U2_CTRL_0P));
    ScreenLog (Buf);
  }

  // 1. reset whole IP (xHCI + U3D)
  Mtu3SetBits (U3D_SSUSB_IP_PW_CTRL0, SSUSB_IP_SW_RST);
  MicroSecondDelay (10);
  Mtu3ClearBits (U3D_SSUSB_IP_PW_CTRL0, SSUSB_IP_SW_RST);
  MicroSecondDelay (1000);

  // 2. power down gadget IP (it is up because LK used fastboot)
  Mtu3SetBits (U3D_SSUSB_IP_PW_CTRL2, SSUSB_IP_DEV_PDN);

  // 3. power up host IP
  Mtu3ClearBits (U3D_SSUSB_IP_PW_CTRL1, SSUSB_IP_HOST_PDN);

  // 4. force host role on U2 port 0 and power the port
  U2Ctrl = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
  U2Ctrl |= SSUSB_U2_PORT_FORCE_IDDIG;
  U2Ctrl &= ~SSUSB_U2_PORT_RG_IDDIG;
  U2Ctrl &= ~SSUSB_U2_PORT_PDN;
  MmioWrite32 (U3D_SSUSB_U2_CTRL_0P, U2Ctrl);

  MicroSecondDelay (20000);

  Ctrl0 = MmioRead32 (U3D_SSUSB_IP_PW_CTRL0);
  Ctrl1 = MmioRead32 (U3D_SSUSB_IP_PW_CTRL1);
  Ctrl2 = MmioRead32 (U3D_SSUSB_IP_PW_CTRL2);
  {
    CHAR16 Buf[128];
    UnicodeSPrint (Buf, sizeof (Buf), L"USB: IPPC after   CTRL0=%08x CTRL1=%08x CTRL2=%08x U2=%08x\r\n",
                   Ctrl0, Ctrl1, Ctrl2, MmioRead32 (U3D_SSUSB_U2_CTRL_0P));
    ScreenLog (Buf);
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

  return Status;
}
