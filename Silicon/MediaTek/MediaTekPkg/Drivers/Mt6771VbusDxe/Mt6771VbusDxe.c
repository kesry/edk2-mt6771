/** @file

  MT6771 VBUS 5V (OTG boost) enable via the MT6370 PMU charger on I2C6.

  Why: in USB host mode the phone must source 5V VBUS. The 3.3V ldo_vusb only
  powers the phone's own USB PHY. Without VBUS no device powers up.

  Provenance
  ----------
  Vendor kernel https://github.com/oppo-source/A3-8.1-kernel-source @da97cfa
    drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_charger.c:1903 mt6370_enable_otg
    drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_charger.c:185 hidden-mode password
    drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_i2c.c        SMBus block access
    drivers/clk/mediatek/clk-mt6771.c:1302                   INFRA_AO_I2C6_CG, shift 6
  drivers/i2c/busses/i2c-mtk.h            MTK I2C v1 register map
  arch/arm64/boot/dts/mediatek/mt6771.dts i2c6@11005000, clock-div = <5>

  Register facts used
  -------------------
  I2C6 base                 0x11005000   (second reg 0x11000600 = pad region)
  INFRA_AO (clock gates)    0x10001000
    infra3 CG set           +0xC0   bit 6 = I2C6 gate (write 1 to ungate)
    infra3 CG status        +0xC8
  MT6370 slave address      0x34 (7-bit)

  Timing for a 26 MHz i2c_sel parent at 100 kHz (mtk_i2c_calculate_speed):
    sample = 5, step = 26  -> TIMING  = 0x0003 | (5 << 12) | (26 << 8)
    fast-mode equivalent   -> LTIMING = 0x0003 | (1 << 12) | (34 << 8)

  Copyright (c) 2026, kesry. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#define MT6771_I2C6_BASE        0x11005000u
#define MT6771_INFRA_AO_BASE    0x10001000u
#define INFRA3_CG_SET           0x00C0u
#define INFRA3_CG_CLR           0x00C4u
#define INFRA3_CG_STA           0x00C8u
#define INFRA3_I2C6_BIT         BIT6

#define MT6370_SLAVE_ADDR       0x34u

/*
 * MTK I2C **v2** offsets.  mt6771_compat in the vendor driver sets
 *   .ver = 0x2, .set_dt_div = 1, .set_ltiming = 1, .idvfs_i2c = 1
 * so the v1 layout used earlier was wrong for this controller.
 * Source: drivers/i2c/busses/i2c-mtk.h  (enum I2C_REGS_OFFSET_V2)
 */
#define I2C_DATA_PORT           0x00u
#define I2C_SLAVE_ADDR          0x04u
#define I2C_INTR_MASK           0x08u
#define I2C_INTR_STAT           0x0Cu
#define I2C_CONTROL             0x10u
#define I2C_TRANSFER_LEN        0x14u
#define I2C_TRANSAC_LEN         0x18u
#define I2C_DELAY_LEN           0x1Cu
#define I2C_TIMING              0x20u
#define I2C_START               0x24u
#define I2C_EXT_CONF            0x28u
#define I2C_LTIMING             0x2Cu
#define I2C_HS                  0x30u
#define I2C_IO_CONFIG           0x34u
#define I2C_FIFO_ADDR_CLR       0x38u
#define I2C_TRANSFER_LEN_AUX    0x44u
#define I2C_CLOCK_DIV           0x48u
#define I2C_HW_TIMEOUT          0x4Cu
#define I2C_SOFTRESET           0x50u
#define I2C_FIFO_STAT           0xF4u

#define I2C_TRANSAC_COMP        BIT0
#define I2C_ACKERR              BIT1
#define I2C_HS_NACKERR          BIT2
#define I2C_ARB_LOST            BIT3
#define I2C_TIMEOUT_BIT         BIT5
#define I2C_TRANSAC_START       BIT0
#define I2C_CONTROL_DIR_CHANGE  BIT4
#define I2C_CONTROL_ACKERR_DET  BIT5
#define I2C_CONTROL_CLK_EXT_EN  BIT3
#define I2C_INTR_ALL            0x01FFu

/* MT6370 registers */
#define MT6370_REG_HIDDENPASCODE1  0x07u
#define MT6370_REG_HIDDENPASCODE2  0x08u
#define MT6370_REG_HIDDENPASCODE3  0x09u
#define MT6370_REG_HIDDENPASCODE4  0x0Au
#define MT6370_REG_CHGCTRL1        0x11u
#define MT6370_REG_LG_CONTROL      0x33u
#define MT6370_REG_CHGHIDDENCTRL6  0x35u
#define MT6370_OPA_MODE            BIT0

#define I2C_TIMEOUT_US          100000u

//
// 0 = quiet mode: no black screen, no bus scan, no on-screen spam. The VBUS
// attempt and the PHY bits are still applied so USB keeps a chance to work.
// 1 = full diagnostics (black panel + I2C/PHY dump), used while debugging.
//
#define A3_USB_DIAG             0

//
// USB PHY (MT6771). Base from vendor DTS usb0phy@11f40000 (sif_base).
// Offsets from vendor drivers/misc/mediatek/mu3phy/mt6771/mtk-phy-asic.h:
//   SSUSB_SIFSLV_U2PHY_COM_BASE     u3_sif2_base + 0x300
//   SSUSB_USB30_PHYA_SIV_B_BASE     u3_sif2_base + 0xB00
//   U3D_USBPHYACR6  +0x18   RG_USB20_OTG_VBUSCMP_EN bit20, BC11_SW_EN bit23
//   U3D_U2PHYACR4   +0x20   RG_USB20_GPIO_CTL bit9, USB20_GPIO_MODE bit8
//   U3D_U2PHYDTM0   +0x68   FORCE_UART_EN bit26, FORCE_SUSPENDM bit18
//   U3D_U2PHYDTM1   +0x6C   RG_VBUSVALID bit5
//
#define MT6771_U3PHY_BASE       0x11F40000u
#define PHY_U2PHY_COM           0x0300u
#define PHY_USBPHYACR6          (MT6771_U3PHY_BASE + PHY_U2PHY_COM + 0x0018u)
#define PHY_U2PHYACR4           (MT6771_U3PHY_BASE + PHY_U2PHY_COM + 0x0020u)
#define PHY_U2PHYDTM0           (MT6771_U3PHY_BASE + PHY_U2PHY_COM + 0x0068u)
#define PHY_U2PHYDTM1           (MT6771_U3PHY_BASE + PHY_U2PHY_COM + 0x006Cu)
#define PHY_ACR6_OTG_VBUSCMP_EN BIT20
#define PHY_ACR6_BC11_SW_EN     BIT23
#define PHY_ACR4_GPIO_CTL       BIT9
#define PHY_ACR4_GPIO_MODE      BIT8
#define PHY_DTM0_FORCE_UART_EN  BIT26
#define PHY_DTM0_FORCE_SUSPENDM BIT18
#define PHY_DTM1_VBUSVALID      BIT5

/*
 * v2 register values, derived from the vendor algorithm for a 26 MHz i2c_sel
 * parent at 100 kHz (sample = 5, step = 26; fast-mode reference step = 34,
 * sample = 1):
 *   timing_reg  = (sample << 8) | step              = 0x051A, plus TIMEOUT_EN
 *   ltiming_reg = (lsample << 6) | lstep | (sample << 12) | (step << 9)
 *   high_speed  = 0x0003 | (sample << 12) | (step << 8)
 *   clock_div   = ((div - 1) << 8) | (div - 1), div = 5 from DT
 */
#define I2C_TIMEOUT_EN          0x0001u
#define I2C_IO_CONFIG_OPEN_DRAIN 0x0003u
#define I2C_SOFT_RST            0x0001u
#define I2C_CLK_SRC_DIV         5u
#define I2C6_TIMING_VAL         (((5u << 8) | 26u) | I2C_TIMEOUT_EN)
#define I2C6_LTIMING_VAL        ((1u << 6) | 34u | (5u << 12) | (26u << 9))
#define I2C6_HS_VAL             (0x0003u | (5u << 12) | (26u << 8))
#define I2C6_CLOCK_DIV_VAL      (((I2C_CLK_SRC_DIV - 1u) << 8) | (I2C_CLK_SRC_DIV - 1u))
#define I2C6_EXT_CONF_VAL       0x1801u
#define I2C6_HW_TIMEOUT_VAL     0xFFFFu

typedef struct {
  UINT32  GateStaBefore;
  UINT32  GateStaAfter;
  UINT16  ClkDivBefore;
  UINT16  TimingBefore;
  UINT16  ClkDivAfter;
  UINT16  TimingAfter;
  EFI_STATUS  ReadStatus;
  UINT8   ChgCtrl1Read;
  EFI_STATUS  OtgStatus;
  UINT8   LgCtrl;
  UINT8   ChgCtrl1;
  UINT32  Dtm0Before;
  UINT32  Dtm1Before;
  UINT32  Acr6Before;
  UINT32  Dtm0After;
  UINT32  Dtm1After;
  UINT32  Acr6After;
  UINT8   ScanList[16];
  UINT16  ScanCount;
  BOOLEAN Reprogrammed;
  UINT32  Pad000;
  UINT32  Pad004;
  UINT32  Pad008;
  UINT32  Pad00C;
} VBUS_DIAG;

STATIC VBUS_DIAG  mDiag;

/* ------------------------------------------------------------------ */
/* Framebuffer console                                                 */
/* ------------------------------------------------------------------ */
#define FB_HEIGHT     2280u
#define FB_STRIDE     4352u
#define FB_START_ROW  5u

STATIC UINT8  *mFb;
STATIC UINTN  mRedraws;

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
ScreenClearAndHome (
  VOID
  )
{
  UINTN  y;

  if (mFb != NULL) {
    for (y = 0; y < FB_HEIGHT; y++) {
      SetMem (mFb + (UINTN)y * FB_STRIDE, FB_STRIDE, 0x00);
    }
  }
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, FB_START_ROW);
  }
}

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */
STATIC
VOID
I2cWrite (IN UINTN Offset, IN UINT16 Value)
{
  MmioWrite16 (MT6771_I2C6_BASE + Offset, Value);
}

STATIC
UINT16
I2cRead (IN UINTN Offset)
{
  return MmioRead16 (MT6771_I2C6_BASE + Offset);
}

STATIC
UINT32
InfraAoRead (IN UINTN Offset)
{
  return MmioRead32 (MT6771_INFRA_AO_BASE + Offset);
}

/**
  Open the I2C6 clock gate (INFRA_AO infra3 CG, bit 6).
**/
STATIC
VOID
I2c6Ungate (
  VOID
  )
{
  // INFRA_AO clock-gate registers are 32-bit.
  MmioWrite32 (MT6771_INFRA_AO_BASE + INFRA3_CG_SET, (UINT32)INFRA3_I2C6_BIT);
  MicroSecondDelay (1000);
  DEBUG ((DEBUG_INFO, "Mt6771Vbus: INFRA3 CG STA=0x%08x\n",
          InfraAoRead (INFRA3_CG_STA)));
}

/**
  Per-transfer preparation.

  It deliberately does NOT issue I2C_SOFTRESET: LK already configured this bus
  (it talks to MT6370 while charging) and a soft reset clears TIMING,
  CLOCK_DIV, LTIMING, HS and IO_CONFIG, which is why every earlier transfer
  stalled. Only clear the FIFO and interrupt state here.
**/
STATIC
VOID
I2cPrepare (
  VOID
  )
{
  I2cWrite (I2C_INTR_MASK, I2C_INTR_ALL);
  I2cWrite (I2C_INTR_STAT, I2C_INTR_ALL);
  I2cWrite (I2C_FIFO_ADDR_CLR, 0x0001);
  I2cWrite (I2C_INTR_STAT, I2C_INTR_ALL);
}

STATIC
VOID
I2cProgramTiming (
  VOID
  )
{
  I2cWrite (I2C_IO_CONFIG, I2C_IO_CONFIG_OPEN_DRAIN);
  I2cWrite (I2C_TIMING, I2C6_TIMING_VAL);
  I2cWrite (I2C_LTIMING, I2C6_LTIMING_VAL);
  I2cWrite (I2C_HS, I2C6_HS_VAL);
  I2cWrite (I2C_CLOCK_DIV, I2C6_CLOCK_DIV_VAL);
  I2cWrite (I2C_HW_TIMEOUT, I2C6_HW_TIMEOUT_VAL);
  I2cWrite (I2C_EXT_CONF, I2C6_EXT_CONF_VAL);
}

STATIC
EFI_STATUS
I2cWaitCompletion (
  IN  UINTN   TimeoutUs,
  OUT UINT16  *FinalStatus
  )
{
  UINTN   Elapsed;
  UINT16  Status;

  Elapsed = 0;
  while (Elapsed < TimeoutUs) {
    Status = I2cRead (I2C_INTR_STAT);
    if ((Status & I2C_INTR_ALL) != 0) {
      I2cWrite (I2C_INTR_STAT, I2C_INTR_ALL);
      if (FinalStatus != NULL) {
        *FinalStatus = Status;
      }
      if ((Status & (I2C_ACKERR | I2C_HS_NACKERR | I2C_ARB_LOST | I2C_TIMEOUT_BIT)) != 0) {
        return EFI_DEVICE_ERROR;
      }
      return EFI_SUCCESS;
    }

    //
    // The START bit is self-clearing when the transaction finishes. Some
    // configurations do not latch an interrupt status for a NAK, so accept
    // "START cleared" as completion and report the error bits we did see.
    //
    if ((I2cRead (I2C_START) & I2C_TRANSAC_START) == 0) {
      if (Elapsed >= 50) {
        Status = I2cRead (I2C_INTR_STAT);
        I2cWrite (I2C_INTR_STAT, I2C_INTR_ALL);
        if (FinalStatus != NULL) {
          *FinalStatus = Status;
        }
        if ((Status & (I2C_ACKERR | I2C_HS_NACKERR | I2C_ARB_LOST | I2C_TIMEOUT_BIT)) != 0) {
          return EFI_DEVICE_ERROR;
        }
        //
        // Only a real completion bit counts. Without this check a NAK looks
        // like success and every address on the bus appears to respond.
        //
        if ((Status & I2C_TRANSAC_COMP) != 0) {
          return EFI_SUCCESS;
        }
        return EFI_DEVICE_ERROR;
      }
    }

    MicroSecondDelay (10);
    Elapsed += 10;
  }

  if (FinalStatus != NULL) {
    *FinalStatus = I2cRead (I2C_INTR_STAT);
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
I2cWriteBytes (
  IN UINT8  Slave,
  IN UINT8  *Data,
  IN UINTN  Length
  )
{
  UINTN  i;

  I2cPrepare ();
  I2cWrite (I2C_SLAVE_ADDR, (UINT16)(Slave << 1));
  I2cWrite (I2C_INTR_MASK, I2C_INTR_ALL);
  I2cWrite (I2C_CONTROL, I2C_CONTROL_ACKERR_DET | I2C_CONTROL_CLK_EXT_EN);
  I2cWrite (I2C_TRANSAC_LEN, 0x0001);
  I2cWrite (I2C_TRANSFER_LEN, (UINT16)Length);
  I2cWrite (I2C_TRANSFER_LEN_AUX, 0x0000);
  for (i = 0; i < Length; i++) {
    I2cWrite (I2C_DATA_PORT, Data[i]);
  }
  I2cWrite (I2C_START, I2C_TRANSAC_START);
  return I2cWaitCompletion (I2C_TIMEOUT_US, NULL);
}

STATIC
EFI_STATUS
I2cWriteReadBytesEx (
  IN  UINT8  Slave,
  IN  UINT8  RegAddr,
  OUT UINT8  *Data,
  IN  UINTN  Length,
  IN  UINTN  TimeoutUs
  )
{
  UINTN  i;

  I2cPrepare ();
  I2cWrite (I2C_SLAVE_ADDR, (UINT16)((Slave << 1) | 0x0001));
  I2cWrite (I2C_INTR_MASK, I2C_INTR_ALL);
  I2cWrite (I2C_CONTROL, I2C_CONTROL_ACKERR_DET | I2C_CONTROL_CLK_EXT_EN | I2C_CONTROL_DIR_CHANGE);
  I2cWrite (I2C_TRANSAC_LEN, 0x0002);
  I2cWrite (I2C_TRANSFER_LEN, (UINT16)Length);
  I2cWrite (I2C_TRANSFER_LEN_AUX, 0x0001);
  I2cWrite (I2C_DATA_PORT, RegAddr);
  I2cWrite (I2C_START, I2C_TRANSAC_START);

  if (EFI_ERROR (I2cWaitCompletion (TimeoutUs, NULL))) {
    return EFI_DEVICE_ERROR;
  }
  for (i = 0; i < Length; i++) {
    Data[i] = (UINT8)(I2cRead (I2C_DATA_PORT) & 0xFFu);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
I2cWriteReadBytes (
  IN  UINT8  Slave,
  IN  UINT8  RegAddr,
  OUT UINT8  *Data,
  IN  UINTN  Length
  )
{
  return I2cWriteReadBytesEx (Slave, RegAddr, Data, Length, I2C_TIMEOUT_US);
}

/* ------------------------------------------------------------------ */
/* MT6370                                                             */
/* ------------------------------------------------------------------ */
STATIC
EFI_STATUS
Mt6370Write (IN UINT8 Reg, IN UINT8 Value)
{
  UINT8  buf[2];

  buf[0] = Reg;
  buf[1] = Value;
  return I2cWriteBytes (MT6370_SLAVE_ADDR, buf, 2);
}

STATIC
EFI_STATUS
Mt6370Read (IN UINT8 Reg, OUT UINT8 *Value)
{
  return I2cWriteReadBytes (MT6370_SLAVE_ADDR, Reg, Value, 1);
}

STATIC
EFI_STATUS
Mt6370UpdateBits (IN UINT8 Reg, IN UINT8 Mask, IN UINT8 Value)
{
  EFI_STATUS  Status;
  UINT8       Old;

  Status = Mt6370Read (Reg, &Old);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Mt6370Write (Reg, (UINT8)((Old & ~Mask) | (Value & Mask)));
}

STATIC
EFI_STATUS
Mt6370EnableHiddenMode (VOID)
{
  STATIC CONST UINT8  Regs[4] = {
    MT6370_REG_HIDDENPASCODE1, MT6370_REG_HIDDENPASCODE2,
    MT6370_REG_HIDDENPASCODE3, MT6370_REG_HIDDENPASCODE4
  };
  STATIC CONST UINT8  Vals[4] = { 0x96, 0x69, 0xC3, 0x3C };
  EFI_STATUS          Status;
  UINTN               i;

  for (i = 0; i < 4; i++) {
    Status = Mt6370Write (Regs[i], Vals[i]);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Mt6370EnableOtg (OUT UINT8 *LgCtrl, OUT UINT8 *ChgCtrl1)
{
  EFI_STATUS  Status;
  UINT8       Val;

  Status = Mt6370EnableHiddenMode ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Mt6370Write (MT6370_REG_LG_CONTROL, 0x7Cu);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Mt6370UpdateBits (MT6370_REG_CHGCTRL1, MT6370_OPA_MODE, MT6370_OPA_MODE);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MicroSecondDelay (20000);

  if (LgCtrl != NULL) {
    Mt6370Read (MT6370_REG_LG_CONTROL, &Val);
    *LgCtrl = Val;
  }
  if (ChgCtrl1 != NULL) {
    Mt6370Read (MT6370_REG_CHGCTRL1, &Val);
    *ChgCtrl1 = Val;
  }
  return Mt6370Write (MT6370_REG_CHGHIDDENCTRL6, 0x00u);
}

/* ------------------------------------------------------------------ */
/* I2C bus scan                                                        */
/* ------------------------------------------------------------------ */
STATIC
VOID
I2cScan (
  VOID
  )
{
  UINT8   Addr;
  UINT8   Val;
  EFI_STATUS  Status;

  mDiag.ScanCount = 0;
  for (Addr = 0x08; Addr <= 0x77; Addr++) {
    Status = I2cWriteReadBytesEx (Addr, 0x00, &Val, 1, 5000u);
    if (!EFI_ERROR (Status)) {
      if (mDiag.ScanCount < 16) {
        mDiag.ScanList[mDiag.ScanCount] = Addr;
      }
      mDiag.ScanCount++;
    }
  }
}

/* ------------------------------------------------------------------ */
/* USB PHY: dump + minimal host-mode switch (experimental)             */
/* ------------------------------------------------------------------ */
STATIC
VOID
PhyBringup (
  VOID
  )
{
  UINT32  Reg;

  mDiag.Dtm0Before = MmioRead32 (PHY_U2PHYDTM0);
  mDiag.Dtm1Before = MmioRead32 (PHY_U2PHYDTM1);
  mDiag.Acr6Before = MmioRead32 (PHY_USBPHYACR6);

  //
  // Mirror the USB-function half of vendor usb_phy_recover(): leave UART mode,
  // release force suspend, enable OTG VBUS comparator, then assert VBUSVALID.
  // TODO: the full vendor sequence also clears many DTM0/DTM1 fields and sets
  // analog tuning; only the role-relevant bits are handled here.
  //
  Reg = MmioRead32 (PHY_U2PHYDTM0);
  Reg &= ~(PHY_DTM0_FORCE_UART_EN | PHY_DTM0_FORCE_SUSPENDM);
  MmioWrite32 (PHY_U2PHYDTM0, Reg);

  Reg = MmioRead32 (PHY_U2PHYACR4);
  Reg &= ~(PHY_ACR4_GPIO_CTL | PHY_ACR4_GPIO_MODE);
  MmioWrite32 (PHY_U2PHYACR4, Reg);

  Reg = MmioRead32 (PHY_USBPHYACR6);
  Reg &= ~PHY_ACR6_BC11_SW_EN;
  Reg |= PHY_ACR6_OTG_VBUSCMP_EN;
  MmioWrite32 (PHY_USBPHYACR6, Reg);

  MicroSecondDelay (800);

  Reg = MmioRead32 (PHY_U2PHYDTM1);
  Reg |= PHY_DTM1_VBUSVALID;
  MmioWrite32 (PHY_U2PHYDTM1, Reg);

  MicroSecondDelay (1000);

  mDiag.Dtm0After = MmioRead32 (PHY_U2PHYDTM0);
  mDiag.Dtm1After = MmioRead32 (PHY_U2PHYDTM1);
  mDiag.Acr6After = MmioRead32 (PHY_USBPHYACR6);
}

/* ------------------------------------------------------------------ */
/* Bring-up sequence (runs once)                                       */
/* ------------------------------------------------------------------ */
STATIC
VOID
VbusBringup (
  VOID
  )
{
  EFI_STATUS  Status;

  ZeroMem (&mDiag, sizeof (mDiag));

  mDiag.Pad000 = MmioRead32 (0x11000600u);
  mDiag.Pad004 = MmioRead32 (0x11000604u);
  mDiag.Pad008 = MmioRead32 (0x11000608u);
  mDiag.Pad00C = MmioRead32 (0x1100060Cu);

  mDiag.GateStaBefore = InfraAoRead (INFRA3_CG_STA);
  mDiag.ClkDivBefore  = I2cRead (I2C_CLOCK_DIV);
  mDiag.TimingBefore  = I2cRead (I2C_TIMING);

  // 1. open the I2C6 clock gate
  I2c6Ungate ();

  mDiag.GateStaAfter = InfraAoRead (INFRA3_CG_STA);
  mDiag.ClkDivAfter  = I2cRead (I2C_CLOCK_DIV);
  mDiag.TimingAfter  = I2cRead (I2C_TIMING);

  //
  // LK normally leaves this bus fully configured (CLOCK_DIV/TIMING non-zero).
  // Only program a fallback when the registers really read back as zero.
  //
  if ((mDiag.ClkDivAfter == 0) || (mDiag.TimingAfter == 0)) {
    I2cProgramTiming ();
    mDiag.ClkDivAfter = I2cRead (I2C_CLOCK_DIV);
    mDiag.TimingAfter = I2cRead (I2C_TIMING);
    mDiag.Reprogrammed = TRUE;
  } else {
    mDiag.Reprogrammed = FALSE;
  }

  // DELAY_LEN is required by the controller but is not always left non-zero.
  if (I2cRead (I2C_DELAY_LEN) == 0) {
    I2cWrite (I2C_DELAY_LEN, 0x0002);
  }

  // 3. probe MT6370
  mDiag.ReadStatus = Mt6370Read (MT6370_REG_CHGCTRL1, &mDiag.ChgCtrl1Read);

  if (A3_USB_DIAG) {
    // 3b. scan the bus so we can tell whether the slave address is wrong
    I2cScan ();
  }

  // 4. turn on the boost
  mDiag.OtgStatus = Mt6370EnableOtg (&mDiag.LgCtrl, &mDiag.ChgCtrl1);

  // 5. try to put the U2 PHY into host-capable state
  PhyBringup ();
}

STATIC
VOID
DrawDiagnostics (
  VOID
  )
{
  CHAR16  Buf[192];
  UINT32  Cap;

  ScreenClearAndHome ();

  UnicodeSPrint (Buf, sizeof (Buf), L"MT6771 VBUS diagnostic   pass %u\r\n", mRedraws);
  ScreenLog (Buf);
  ScreenLog (L"------------------------------------------\r\n");

  UnicodeSPrint (Buf, sizeof (Buf), L"INFRA3_CG_STA  before=0x%08x after=0x%08x\r\n",
                 mDiag.GateStaBefore, mDiag.GateStaAfter);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"I2C6 CLOCK_DIV  before=0x%04x after=0x%04x\r\n",
                 mDiag.ClkDivBefore, mDiag.ClkDivAfter);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"I2C6 TIMING     before=0x%04x after=0x%04x\r\n",
                 mDiag.TimingBefore, mDiag.TimingAfter);
  ScreenLog (Buf);

  {
    UINTN  i;
    CHAR16  Line[128];
    CHAR16  Item[16];

    UnicodeSPrint (Line, sizeof (Line), L"I2C6 scan    : %u device(s) ", mDiag.ScanCount);
    for (i = 0; i < mDiag.ScanCount && i < 16; i++) {
      UnicodeSPrint (Item, sizeof (Item), L"0x%02x ", mDiag.ScanList[i]);
      StrCatS (Line, sizeof (Line) / sizeof (CHAR16), Item);
    }
    if (mDiag.ScanCount == 0) {
      StrCatS (Line, sizeof (Line) / sizeof (CHAR16), L"(bus silent)");
    }
    StrCatS (Line, sizeof (Line) / sizeof (CHAR16), L"\r\n");
    ScreenLog (Line);
  }

  ScreenLog (L"------------------------------------------\r\n");
  UnicodeSPrint (Buf, sizeof (Buf), L"MT6370 read  : %r val=0x%02x\r\n",
                 mDiag.ReadStatus, mDiag.ChgCtrl1Read);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"MT6370 OTG   : %r\r\n", mDiag.OtgStatus);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"   LG=0x%02x CHGCTRL1=0x%02x\r\n",
                 mDiag.LgCtrl, mDiag.ChgCtrl1);
  ScreenLog (Buf);
  if ((mDiag.ChgCtrl1 & MT6370_OPA_MODE) != 0) {
    ScreenLog (L"   => boost ON, port sources 5V\r\n");
  } else {
    ScreenLog (L"   => boost OFF\r\n");
  }

  ScreenLog (L"------------------------------------------\r\n");
  UnicodeSPrint (Buf, sizeof (Buf), L"PHY DTM0 %08x -> %08x\r\n", mDiag.Dtm0Before, mDiag.Dtm0After);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"PHY DTM1 %08x -> %08x\r\n", mDiag.Dtm1Before, mDiag.Dtm1After);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"PHY ACR6 %08x -> %08x\r\n", mDiag.Acr6Before, mDiag.Acr6After);
  ScreenLog (Buf);

  ScreenLog (L"------------------------------------------\r\n");
  Cap = MmioRead32 (0x11200000u);
  UnicodeSPrint (Buf, sizeof (Buf), L"xHCI CAP0=0x%08x caplen=%u\r\n", Cap, Cap & 0xFFu);
  ScreenLog (Buf);
}

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mRedraws >= 6) {
    gBS->SetTimer (Event, TimerCancel, 0);
    return;
  }
  mRedraws++;
  DrawDiagnostics ();
}

EFI_STATUS
EFIAPI
Mt6771VbusDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  mFb      = (UINT8 *)(UINTN)FixedPcdGet32 (PcdMipiFrameBufferAddress);
  mRedraws = 0;

  VbusBringup ();

  Event = NULL;
  if (A3_USB_DIAG) {
    Status = gBS->CreateEvent (
                    EVT_TIMER | EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    OnTimer,
                    NULL,
                    &Event
                    );
    if (!EFI_ERROR (Status)) {
      gBS->SetTimer (Event, TimerPeriodic, 20000000u);
    }
  }

  //
  // Quiet mode: the VBUS and PHY attempts above still happen, but nothing is
  // painted over the shell so its startup.nsh output stays readable.
  //
  return EFI_SUCCESS;
}
