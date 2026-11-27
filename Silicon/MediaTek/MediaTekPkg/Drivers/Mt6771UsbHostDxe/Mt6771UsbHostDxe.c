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
#include <Protocol/Usb2HostController.h>

//
// MMIO bases (see file header for provenance).
//

/*
 * SoC-side USB clock gates -- vendor clk-mt6771.c:
 *   GATE_INFRA0(INFRACFG_AO_ICUSB_CG, ..., 8)   set/reg +0x80 in INFRA_AO
 *   GATE_INFRA2(INFRACFG_AO_USB_CG,   ..., 1)   set/reg +0xA4 in INFRA_AO
 *   GATE_APMIXED(APMIXED_SSUSB26M,   ..., 4)   inverted gate at 0x1000C000+0x20
 * Without these the xHCI MMIO reads as live but the controller cannot drive
 * any bus activity, which matches the 'register xhci success but no device'
 * symptom on both powered hubs and ACM.
 */
#define MT6771_INFRA_AO_BASE     0x10001000u
#define INFRA0_CG_SET            0x0080u
#define INFRA2_CG_SET            0x00A4u
#define INFRA0_ICUSB_BIT         BIT8
#define INFRA2_USB_BIT           BIT1
#define MT6771_APMIXED_BASE      0x1000C000u
#define APMIXED_CG_REG           0x0020u
#define APMIXED_SSUSB26M_BIT     BIT4

#define MT6771_XHCI_BASE      0x11200000u
#define MT6771_XHCI_SIZE      0x00001000u
#define MT6771_IPPC_BASE      0x11203E00u

//
// IPPC registers and bits -- taken from the Android 8 vendor source for THIS
// SoC, not from upstream mtu3 (the two differ, see docs/porting.md):
//   drivers/misc/mediatek/ssusb/include/mu3d/ssusb_sifslv_ippc_c_header.h
//     U3D_SSUSB_IP_PW_CTRL0 +0x00  SSUSB_IP_SW_RST    bit0
//     U3D_SSUSB_IP_PW_CTRL1 +0x04  SSUSB_IP_HOST_PDN  bit0
//     U3D_SSUSB_IP_PW_CTRL2 +0x08  SSUSB_IP_DEV_PDN   bit0
//     U3D_SSUSB_IP_PW_STS1  +0x10  SYS125_RST_B_STS bit10, U3_MAC_RST_B_STS bit16
//     U3D_SSUSB_IP_PW_STS2  +0x14  U2_MAC_SYS_RST_B_STS bit0
//     U3D_SSUSB_U3_CTRL_0P  +0x30  HOST_SEL bit2, PDN bit1, DIS bit0
//     U3D_SSUSB_U2_CTRL_0P  +0x50  OTG_SEL bit7, HOST_SEL bit2, PDN bit1, DIS bit0
//   drivers/misc/mediatek/mu3phy/mt6771/mtk-phy-asic.h
//     U3D_U2PHYDTM1 FORCE_IDDIG bit9, RG_IDDIG bit1
//

//
// MAC operational-state registers. SSUSB_USB2_CSR_BASE is 0x2400 relative to
// the MAC base (mtu3_hw_regs.h), and toggle_opstate() in mtu3_dr.c is called
// ONLY on the switch to host mode -- the device path does not do it:
//   mtu3_setbits(mac_base, U3D_DEVICE_CONTROL,   DC_SESSION)  // BIT0
//   mtu3_setbits(mac_base, U3D_POWER_MANAGEMENT, SOFT_CONN)   // BIT6
//
#define MT6771_MAC_BASE          0x11201000u
#define U3D_POWER_MANAGEMENT     (MT6771_MAC_BASE + 0x2404u)
#define U3D_DEVICE_CONTROL       (MT6771_MAC_BASE + 0x240Cu)

//
// USB2 MAC timing parameter -- vendor ssusb_usb2_csr_c_header.h and
// xhci-mtk.c:mtk_xhci_ip_init(), which programs it unconditionally:
//   U3D_USB20_TIMING_PARAMETER = SSUSB_USB2_CSR_BASE + 0x0040
//   TIME_VALUE_1US             = bits 7:0
//   MTK_TIME_VALUE_1US         = 63  ("62.5MHz:63, 80MHz:80, 100MHz:100, 125MHz:125")
// It is the 1 us base the USB2 MAC uses to time SOF/reset signalling.
//
#define U3D_USB20_TIMING_PARAMETER (MT6771_MAC_BASE + 0x2440u)
#define TIME_VALUE_1US             0x000000FFu
#define MTK_TIME_VALUE_1US         63u
#define DC_SESSION               BIT0
#define SOFT_CONN                BIT6
#define U3D_SSUSB_IP_PW_CTRL0 (MT6771_IPPC_BASE + 0x0000u)
#define U3D_SSUSB_IP_PW_CTRL1 (MT6771_IPPC_BASE + 0x0004u)
#define U3D_SSUSB_IP_PW_CTRL2 (MT6771_IPPC_BASE + 0x0008u)
#define U3D_SSUSB_IP_PW_STS1  (MT6771_IPPC_BASE + 0x0010u)
#define U3D_SSUSB_IP_PW_STS2  (MT6771_IPPC_BASE + 0x0014u)
#define U3D_SSUSB_U3_CTRL_0P  (MT6771_IPPC_BASE + 0x0030u)
#define U3D_SSUSB_U2_CTRL_0P  (MT6771_IPPC_BASE + 0x0050u)

#define SSUSB_IP_SW_RST       BIT0
#define SSUSB_IP_HOST_PDN     BIT0
#define SSUSB_IP_DEV_PDN      BIT0

#define SSUSB_SYS125_RST_B_STS    BIT10
#define SSUSB_U3_MAC_RST_B_STS    BIT16
#define SSUSB_U2_MAC_SYS_RST_B_STS BIT0

#define SSUSB_U3_PORT_HOST_SEL BIT2
#define SSUSB_U3_PORT_PDN     BIT1
#define SSUSB_U3_PORT_DIS     BIT0

#define SSUSB_U2_PORT_OTG_SEL  BIT7
#define SSUSB_U2_PORT_HOST_SEL BIT2
#define SSUSB_U2_PORT_PDN     BIT1
#define SSUSB_U2_PORT_DIS     BIT0

/* PHY base from the stock DTB usb0phy@11f40000; U2 PHY COM block at +0x300 */
#define MT6771_U3PHY_BASE     0x11F40000u

/* PHY U3D_U2PHYDTM1 (usb0phy@11f40000 + 0x300 + 0x6C) */
#define MT6771_UPHY_DTM1      (MT6771_U3PHY_BASE + 0x036Cu)

//
// USB2 PHY registers. u3_sif2_base is 0x11F40000 on MT6771 (the vendor header
// confirms it: "0x11F40B20" == u3_sif2_base + 0xb00 + 0x20), and the U2 PHY COM
// block sits at +0x300.
//
#define MT6771_UPHY_COM        (MT6771_U3PHY_BASE + 0x0300u)
#define MT6771_UPHY_DTM0       (MT6771_UPHY_COM + 0x0068u)
#define MT6771_UPHY_ACR4       (MT6771_UPHY_COM + 0x0020u)
#define MT6771_UPHY_ACR6       (MT6771_UPHY_COM + 0x0018u)

#define UPHY_DTM0_FORCE_UART_EN   BIT26
#define UPHY_DTM0_FORCE_DATAIN    BIT23
#define UPHY_DTM0_FORCE_DM_PD     BIT21
#define UPHY_DTM0_FORCE_DP_PD     BIT20
#define UPHY_DTM0_FORCE_XCVRSEL   BIT19
#define UPHY_DTM0_FORCE_SUSPENDM  BIT18
#define UPHY_DTM0_FORCE_TERMSEL   BIT17
#define UPHY_DTM0_RG_DATAIN       (0xFu << 10)
#define UPHY_DTM0_RG_DM_PD        BIT7
#define UPHY_DTM0_RG_DP_PD        BIT6
#define UPHY_DTM0_RG_XCVRSEL      (0x3u << 4)
#define UPHY_DTM0_RG_TERMSEL      BIT2
#define UPHY_DTM1_RG_UART_EN      BIT16
#define UPHY_DTM1_RG_VBUSVALID    BIT5
#define UPHY_ACR4_GPIO_CTL        BIT9
#define UPHY_ACR4_GPIO_MODE       BIT8
#define UPHY_ACR6_BC11_SW_EN      BIT23
#define UPHY_ACR6_OTG_VBUSCMP_EN  BIT20

#define PHY_FORCE_IDDIG       BIT9
#define PHY_RG_IDDIG          BIT1

//
// The RELEASE build maps SerialPortLib to UART0, which is not wired on this
// board, so DEBUG() output is invisible. Print to the UEFI console instead:
// this driver depends on SimpleTextOut, so ConOut exists when it runs.
//
#define USB_LOG_LINES 16

STATIC CHAR16  mLogBuf[USB_LOG_LINES][144];
STATIC UINTN   mLogUsed;
STATIC UINTN   mLogReplays;
STATIC BOOLEAN mPortsPowered;

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


//
// PORTSC bits, from MdeModulePkg/Bus/Pci/XhciDxe/XhciReg.h:
//   CCS BIT0, PED BIT1, OCA BIT3, PLS (BIT5|BIT6|BIT7|BIT8), PP BIT9,
//   LWS BIT16, CSC BIT17, PEC BIT18, WRC BIT19, OCC BIT20, PRC BIT21,
//   PLC BIT22, CEC BIT23
//
#define XHC_PORTSC_CCS   BIT0
#define XHC_PORTSC_PED   BIT1
#define XHC_PORTSC_PLS   (BIT5 | BIT6 | BIT7 | BIT8)
#define XHC_PORTSC_PP    BIT9
#define XHC_PORTSC_CSC   BIT17
#define XHC_PORTSC_PEC   BIT18
#define XHC_PORTSC_WRC   BIT19
#define XHC_PORTSC_OCC   BIT20
#define XHC_PORTSC_PRC   BIT21
#define XHC_PORTSC_PLC   BIT22
#define XHC_PORTSC_CEC   BIT23

STATIC
VOID
XhcPowerPorts (
  IN UINT32  Caplen,
  IN UINT32  MaxPorts
  );
STATIC
VOID
EFIAPI
UsbLogReplay (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  Cap0;
  UINT32  Cap4;
  UINT32  Caplen;
  UINT32  MaxPorts;
  CHAR16  Buf[160];

  //
  // One shot. The controller is live by now (XhciDxe bound), so the only thing
  // left to do is power the ports, because EDK2 never sets PORTSC.PP. After
  // that this callback cancels itself and stops touching the controller: the
  // repeated dumps were disturbing a running port and the machine rebooted.
  //
  gBS->SetTimer (Event, TimerCancel, 0);

  Cap0     = MmioRead32 (MT6771_XHCI_BASE + 0x00);
  Cap4     = MmioRead32 (MT6771_XHCI_BASE + 0x04);
  Caplen   = Cap0 & 0xFFu;
  MaxPorts = (Cap4 >> 24) & 0xFFu;

  if (!mPortsPowered) {
    XhcPowerPorts (Caplen, MaxPorts);
    mPortsPowered = TRUE;
    MicroSecondDelay (20000);
  }

  if (Caplen != 0) {
    UINT32  PortSc = MmioRead32 (MT6771_XHCI_BASE + Caplen + 0x400u);
    UnicodeSPrint (Buf, sizeof (Buf),
      L"USB: port power done, PORTSC1=%08x CCS=%u PED=%u PP=%u\r\n",
      PortSc,
      (PortSc & XHC_PORTSC_CCS) ? 1u : 0u,
      (PortSc & XHC_PORTSC_PED) ? 1u : 0u,
      (PortSc & XHC_PORTSC_PP) ? 1u : 0u);
    gST->ConOut->OutputString (gST->ConOut, Buf);
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

/**
  Put the USB2 PHY into host/OTG mode. This is the USB-function half of the
  vendor usb_phy_recover() (mu3phy/mt6771/mtk-phy-asic.c), including every
  field it clears: leaving UART mode, releasing force suspend and the DP/DM
  pull-downs, disabling the BC1.1 path, enabling the OTG VBUS comparator and
  finally asserting VBUSVALID so the port starts detecting devices.

  It must run while VBUS 5V is already present, which is why it lives in the
  host driver: the VBUS driver runs first and enables the charger boost.
**/

/**
  Power every xHCI port.

  MdeModulePkg/Bus/Pci/XhciDxe never writes PORTSC at all (no reference to
  XHC_PORTSC_PP in Xhci.c), so it cannot power a port on a controller whose
  HCCPARAMS1.PPC is 1, which is the case here (PPC=1). Without PORTSC.PP the
  port stays unpowered, PLS reads a reserved value and CCS never asserts, so no
  device is ever detected.

  Change bits (CSC..CAS, bits 17..24) are written as 0 so they are not cleared.
**/
STATIC
VOID
XhcPowerPorts (
  IN UINT32  Caplen,
  IN UINT32  MaxPorts
  )
{
  UINT32  OpBase = MT6771_XHCI_BASE + Caplen;
  UINTN   P;

  for (P = 1; P <= MaxPorts && P <= 8; P++) {
    UINT32  Reg   = OpBase + 0x400u + 0x10u * (P - 1u);
    UINT32  PortSc = MmioRead32 (Reg);

    if ((PortSc & (UINT32)XHC_PORTSC_PP) == 0) {
      PortSc &= ~(UINT32)(XHC_PORTSC_CSC | XHC_PORTSC_PEC | XHC_PORTSC_WRC |
                          XHC_PORTSC_OCC | XHC_PORTSC_PRC | XHC_PORTSC_PLC |
                          XHC_PORTSC_CEC);
      PortSc |= (UINT32)XHC_PORTSC_PP;
      MmioWrite32 (Reg, PortSc);
    }
  }
}
STATIC
VOID
Mt6771UsbPhyHostMode (
  VOID
  )
{
  UINT32  Reg;

  // leave UART mode (DTM0 FORCE_UART_EN, DTM1 RG_UART_EN)
  MmioWrite32 (MT6771_UPHY_DTM0, MmioRead32 (MT6771_UPHY_DTM0) & ~(UINT32)UPHY_DTM0_FORCE_UART_EN);
  MmioWrite32 (MT6771_UPHY_DTM1, MmioRead32 (MT6771_UPHY_DTM1) & ~(UINT32)UPHY_DTM1_RG_UART_EN);

  // leave GPIO mode on the PHY pins
  MmioWrite32 (MT6771_UPHY_ACR4, MmioRead32 (MT6771_UPHY_ACR4) & ~(UINT32)(UPHY_ACR4_GPIO_CTL | UPHY_ACR4_GPIO_MODE));

  // release force-suspend and every forced DP/DM/XCVRSEL/TERMSEL/DATAIN value
  Reg  = MmioRead32 (MT6771_UPHY_DTM0);
  Reg &= ~(UINT32)(UPHY_DTM0_FORCE_SUSPENDM | UPHY_DTM0_RG_DP_PD | UPHY_DTM0_RG_DM_PD |
                   UPHY_DTM0_RG_XCVRSEL | UPHY_DTM0_RG_TERMSEL | UPHY_DTM0_RG_DATAIN |
                   UPHY_DTM0_FORCE_DP_PD | UPHY_DTM0_FORCE_DM_PD | UPHY_DTM0_FORCE_XCVRSEL |
                   UPHY_DTM0_FORCE_TERMSEL | UPHY_DTM0_FORCE_DATAIN);
  MmioWrite32 (MT6771_UPHY_DTM0, Reg);

  // disable the DP/DM BC1.1 path, enable the OTG VBUS comparator
  Reg  = MmioRead32 (MT6771_UPHY_ACR6);
  Reg &= ~(UINT32)UPHY_ACR6_BC11_SW_EN;
  Reg |= (UINT32)UPHY_ACR6_OTG_VBUSCMP_EN;
  MmioWrite32 (MT6771_UPHY_ACR6, Reg);

  // vendor waits 800 us here
  MicroSecondDelay (800);

  // VBUS is good: let the PHY run host mode
  MmioWrite32 (MT6771_UPHY_DTM1, MmioRead32 (MT6771_UPHY_DTM1) | (UINT32)UPHY_DTM1_RG_VBUSVALID);
  MicroSecondDelay (1000);
}
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

  //
  // 2. Keep the device IP POWERED. Upstream mtu3 powers it down
  //    (mtu3_plat.c:ssusb_ip_sw_reset) purely so ip-sleep works in host-only
  //    mode, but the MT6771 vendor host driver (ssusb/mu3d/xhci-mtk.c) never
  //    touches SSUSB_IP_DEV_PDN. The whole USB2 CSR block lives in the device
  //    IP, so powering it down makes U3D_USB20_TIMING_PARAMETER and
  //    U3D_DEVICE_CONTROL read back as 0 and any write there is lost.
  //
  Mtu3ClearBits (U3D_SSUSB_IP_PW_CTRL2, SSUSB_IP_DEV_PDN);

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

  //
  // 6. Force IDDIG to host the way the vendor driver does
  //    (xhci-mtk.c:ssusb_otg_iddig_en / ssusb_otg_plug_in):
  //      PHY U3D_U2PHYDTM1 |= FORCE_IDDIG, &= ~RG_IDDIG
  //      U2_CTRL(0) |= OTG_SEL
  //
  Value  = MmioRead32 (MT6771_UPHY_DTM1);
  Value |= PHY_FORCE_IDDIG;
  Value &= ~PHY_RG_IDDIG;
  MmioWrite32 (MT6771_UPHY_DTM1, Value);

  Value |= SSUSB_U2_PORT_OTG_SEL;
  MmioWrite32 (U3D_SSUSB_U2_CTRL_0P, Value);

  //
  // 7. Wait for the IP clock status bits exactly as the vendor does
  //    (xhci-mtk.c:mtk_chk_usb_ip_ck_sts). A port whose PDN is set stays in
  //    reset, so its status bit is not meaningful.
  //
  for (Wait = 0; Wait < 100; Wait++) {
    if ((MmioRead32 (U3D_SSUSB_IP_PW_STS1) & SSUSB_SYS125_RST_B_STS) != 0) {
      break;
    }
    MicroSecondDelay (1000);
  }
  if ((MmioRead32 (U3D_SSUSB_U2_CTRL_0P) & SSUSB_U2_PORT_PDN) == 0) {
    for (Wait = 0; Wait < 100; Wait++) {
      if ((MmioRead32 (U3D_SSUSB_IP_PW_STS2) & SSUSB_U2_MAC_SYS_RST_B_STS) != 0) {
        break;
      }
      MicroSecondDelay (1000);
    }
  }
  if ((MmioRead32 (U3D_SSUSB_U3_CTRL_0P) & SSUSB_U3_PORT_PDN) == 0) {
    for (Wait = 0; Wait < 100; Wait++) {
      if ((MmioRead32 (U3D_SSUSB_IP_PW_STS1) & SSUSB_U3_MAC_RST_B_STS) != 0) {
        break;
      }
      MicroSecondDelay (1000);
    }
  }

  //
  // toggle_opstate(): the host-only step from mtu3_dr.c. switch_port_to_device()
  // does not call it, so it is specific to entering host mode. Without it the
  // MAC never reaches the operational state and the port never leaves reset,
  // which is consistent with CCS staying 0 while PPC/PP are fine.
  //
  {
    CHAR16  Dbuf[128];
    UINT32  DevBefore = MmioRead32 (U3D_DEVICE_CONTROL);
    MmioWrite32 (U3D_DEVICE_CONTROL, DevBefore | (UINT32)DC_SESSION);
    UnicodeSPrint (Dbuf, sizeof (Dbuf), L"USB: DEVCTL %08x -> %08x\r\n",
                   DevBefore, MmioRead32 (U3D_DEVICE_CONTROL));
    ScreenLog (Dbuf);
  }
  MmioWrite32 (U3D_POWER_MANAGEMENT, MmioRead32 (U3D_POWER_MANAGEMENT) | (UINT32)SOFT_CONN);
  MicroSecondDelay (1000);

  //
  // USB2 MAC 1 us timing base (vendor mtk_xhci_ip_init). Required for the
  // USB2 MAC to operate at all; it was never programmed before.
  //
  MmioWrite32 (U3D_USB20_TIMING_PARAMETER,
               (MmioRead32 (U3D_USB20_TIMING_PARAMETER) & ~(UINT32)TIME_VALUE_1US) |
               (UINT32)MTK_TIME_VALUE_1US);

  PwSts = MmioRead32 (U3D_SSUSB_IP_PW_STS1);
  {
    CHAR16  Buf[192];
    UnicodeSPrint (
      Buf, sizeof (Buf),
      L"USB: IPPC1=%08x U2=%08x U3=%08x STS1=%08x STS2=%08x sys125=%u u3mac=%u u2mac=%u\r\n",
      MmioRead32 (U3D_SSUSB_IP_PW_CTRL1),
      MmioRead32 (U3D_SSUSB_U2_CTRL_0P),
      MmioRead32 (U3D_SSUSB_U3_CTRL_0P),
      PwSts,
      MmioRead32 (U3D_SSUSB_IP_PW_STS2),
      (PwSts & SSUSB_SYS125_RST_B_STS) ? 1u : 0u,
      (PwSts & SSUSB_U3_MAC_RST_B_STS) ? 1u : 0u,
      (MmioRead32 (U3D_SSUSB_IP_PW_STS2) & SSUSB_U2_MAC_SYS_RST_B_STS) ? 1u : 0u
      );
    ScreenLog (Buf);
    if ((PwSts & SSUSB_SYS125_RST_B_STS) == 0) {
      ScreenLog (L"USB: sys125 clock NOT ready -> host IP not clocked\r\n");
    } else {
      ScreenLog (L"USB: sys125 clock ready\r\n");
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

  Mt6771UsbPhyHostMode ();
  ScreenLog (L"USB: PHY in host mode\r\n");

  //
  // The dump showed IPPC U2_CTRL ending up as 0x280: OTG_SEL (bit7) and
  // OTG_HOST_VBUSVALID_SEL (bit9) set but HOST_SEL (bit2) cleared, while the
  // U3 port kept its HOST_SEL. Something between the port switch and here
  // clears bit2, so re-assert it as the very last step before xHCI takes over
  // and log the before/after value.
  //
  {
    CHAR16  Ubuf[128];
    UINT32  Before = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
    UINT32  After;

    MmioWrite32 (U3D_SSUSB_U2_CTRL_0P,
                 (Before & ~(UINT32)(SSUSB_U2_PORT_PDN | SSUSB_U2_PORT_DIS)) |
                 (UINT32)(SSUSB_U2_PORT_HOST_SEL | SSUSB_U2_PORT_OTG_SEL));
    MicroSecondDelay (100);
    After = MmioRead32 (U3D_SSUSB_U2_CTRL_0P);
    UnicodeSPrint (Ubuf, sizeof (Ubuf), L"USB: U2CTRL %08x -> %08x host_sel=%u\r\n",
                   Before, After, (After & SSUSB_U2_PORT_HOST_SEL) ? 1u : 0u);
    ScreenLog (Ubuf);
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
      // fire once, a few seconds in, so XhciDxe has already bound and
      // initialised the controller before we power the ports
      gBS->SetTimer (ReplayEvent, TimerRelative, 30000000u);
    }
  }

  return Status;
}
