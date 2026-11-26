/** @file

  MT6771 VBUS 5V (OTG boost) enable via MT6370 PMU charger.

  Problem this solves: in USB host mode the phone must source 5V VBUS on the
  Type-C port. Without it a keyboard / 2.4G receiver / U-disk never powers up
  (observed: NUM LOCK LED stays dark). The 3.3V `ldo_vusb` only feeds the
  phone's own USB PHY and is not VBUS.

  Facts and provenance
  --------------------
  Vendor kernel: https://github.com/oppo-source/A3-8.1-kernel-source
    branch master, commit da97cfa58a40dd13bb30472f2c02eab2ff0c85df

  * Charger is an external MT6370 PMU, not the MT6358 internal charger:
    device tree node `mediatek,mt6370_pmu_charger`.
  * MT6370 sits on I2C6 and the vendor DTS provides a dedicated
    `i2c6@11005000 { compatible = "mediatek,i2c6"; }` node.
  * MT6370 register access uses SMBus block reads/writes:
    drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_i2c.c
      i2c_smbus_write_i2c_block_data(client, reg, len, data)
      i2c_smbus_read_i2c_block_data(client, reg, len, data)
  * OTG enable sequence: mt6370_pmu_charger.c:1903 mt6370_enable_otg()
  * Hidden-mode password: mt6370_pmu_charger.c:185
      regs HIDDENPASCODE1..4 (0x07..0x0A) = 0x96, 0x69, 0xC3, 0x3C
  * MTK I2C v1 register map: drivers/i2c/busses/i2c-mtk.h
      (enum I2C_REGS_OFFSET, values are MMIO offsets)

  UNVERIFIED: LK already uses I2C6 to talk to MT6370 while charging, so this
  driver assumes the controller timing/clock is left configured and only
  performs transfers. If transfers time out, explicit timing setup is needed
  (see TODO at the bottom).

  Copyright (c) 2026, kesry. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Guid/EventGroup.h>

//
// MT6771 I2C6 controller (vendor DTS i2c6@11005000).
//
#define MT6771_I2C6_BASE        0x11005000u

//
// MT6370 7-bit slave address on I2C6.
//
#define MT6370_SLAVE_ADDR       0x34u

//
// MTK I2C v1 offsets (i2c-mtk.h, enum I2C_REGS_OFFSET).
//
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
#define I2C_FIFO_STAT           0x30u
#define I2C_FIFO_ADDR_CLR       0x38u
#define I2C_IO_CONFIG           0x40u
#define I2C_SOFTRESET           0x50u
#define I2C_TRANSFER_LEN_AUX    0x6Cu
#define I2C_CLOCK_DIV           0x70u

//
// MTK I2C control/status bits (i2c-mtk.h).
//
#define I2C_TRANSAC_COMP        BIT0
#define I2C_ACKERR              BIT1
#define I2C_HS_NACKERR          BIT2
#define I2C_ARB_LOST            BIT3
#define I2C_TIMEOUT_BIT         BIT5
#define I2C_TRANSAC_START       BIT0
#define I2C_CONTROL_RS          BIT1
#define I2C_CONTROL_DIR_CHANGE  BIT4
#define I2C_CONTROL_ACKERR_DET  BIT5
#define I2C_CONTROL_CLK_EXT_EN  BIT3
#define I2C_INTR_ALL            (BIT8|BIT7|BIT6|BIT5|BIT4|BIT3|BIT2|BIT1|BIT0)

//
// MT6370 registers (mt6370_pmu.h).
//
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
// Direct framebuffer console.
//
// The UEFI Shell clears the screen when it starts, which wiped our earlier
// ReadyToBoot diagnostics. Fill the panel black and draw our own status text,
// starting a few rows down so nothing is clipped, and repeat it on a timer so
// the shell cannot erase it before the user reads it.
//
#define FB_WIDTH   1080u
#define FB_HEIGHT  2280u
#define FB_STRIDE  4352u
#define FB_START_ROW 5u

STATIC UINT8  *mFb;
STATIC UINTN  mRedraws;

STATIC
VOID
FbFillBlack (
  VOID
  )
{
  UINTN  y;
  UINT8  *Row;

  if (mFb == NULL) {
    return;
  }
  for (y = 0; y < FB_HEIGHT; y++) {
    Row = mFb + (UINTN)y * FB_STRIDE;
    SetMem (Row, FB_STRIDE, 0x00);
  }
}

STATIC
VOID
ScreenClearAndHome (
  VOID
  )
{
  FbFillBlack ();
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, FB_START_ROW);
  }
}

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
I2cWriteReg16 (
  IN UINTN   Offset,
  IN UINT16  Value
  )
{
  MmioWrite16 (MT6771_I2C6_BASE + Offset, Value);
}

STATIC
UINT16
I2cReadReg16 (
  IN UINTN  Offset
  )
{
  return MmioRead16 (MT6771_I2C6_BASE + Offset);
}

/**
  Wait for transfer completion.

  @retval EFI_SUCCESS  transaction completed without reported error.
**/
STATIC
EFI_STATUS
I2cWaitCompletion (
  OUT UINT16  *FinalStatus
  )
{
  UINTN   Elapsed;
  UINT16  Status;

  Elapsed = 0;
  while (Elapsed < I2C_TIMEOUT_US) {
    Status = I2cReadReg16 (I2C_INTR_STAT);
    if ((Status & I2C_INTR_ALL) != 0) {
      // acknowledge/clear
      I2cWriteReg16 (I2C_INTR_STAT, I2C_INTR_ALL);
      if (FinalStatus != NULL) {
        *FinalStatus = Status;
      }
      if ((Status & (I2C_ACKERR | I2C_HS_NACKERR | I2C_ARB_LOST | I2C_TIMEOUT_BIT)) != 0) {
        return EFI_DEVICE_ERROR;
      }
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
    Elapsed += 10;
  }

  if (FinalStatus != NULL) {
    *FinalStatus = I2cReadReg16 (I2C_INTR_STAT);
  }
  return EFI_TIMEOUT;
}

/**
  Minimal controller sanity setup. Assumes LK already programmed timing/clock
  for I2C6 (it uses this bus to talk to the charger); we only clear state.
**/
STATIC
VOID
I2cPrepare (
  VOID
  )
{
  I2cWriteReg16 (I2C_SOFTRESET, 0x0001);
  MicroSecondDelay (1000);
  I2cWriteReg16 (I2C_SOFTRESET, 0x0000);
  I2cWriteReg16 (I2C_FIFO_ADDR_CLR, 0x0001);
  I2cWriteReg16 (I2C_INTR_STAT, I2C_INTR_ALL);
}

/**
  Write bytes to a slave: START, addr+W, data..., STOP.
**/
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

  I2cWriteReg16 (I2C_SLAVE_ADDR, (UINT16)(Slave << 1));
  I2cWriteReg16 (I2C_INTR_MASK, 0x0000);
  I2cWriteReg16 (I2C_CONTROL, I2C_CONTROL_ACKERR_DET);
  I2cWriteReg16 (I2C_TRANSAC_LEN, 0x0000);
  I2cWriteReg16 (I2C_TRANSFER_LEN, (UINT16)Length);
  I2cWriteReg16 (I2C_TRANSFER_LEN_AUX, 0x0000);

  for (i = 0; i < Length; i++) {
    I2cWriteReg16 (I2C_DATA_PORT, Data[i]);
  }

  I2cWriteReg16 (I2C_START, I2C_TRANSAC_START);
  return I2cWaitCompletion (NULL);
}

/**
  Write one byte (register address), repeated START, read Length bytes.

  Uses the MTK DIR_CHANGE (write/read) mode: TRANSFER_LEN = read length,
  TRANSFER_LEN_AUX = write length (1).
**/
STATIC
EFI_STATUS
I2cWriteReadBytes (
  IN  UINT8  Slave,
  IN  UINT8  RegAddr,
  OUT UINT8  *Data,
  IN  UINTN  Length
  )
{
  UINTN  i;

  I2cPrepare ();

  I2cWriteReg16 (I2C_SLAVE_ADDR, (UINT16)((Slave << 1) | 0x0001));
  I2cWriteReg16 (I2C_INTR_MASK, 0x0000);
  I2cWriteReg16 (I2C_CONTROL, I2C_CONTROL_ACKERR_DET | I2C_CONTROL_DIR_CHANGE);
  I2cWriteReg16 (I2C_TRANSAC_LEN, 0x0001);
  I2cWriteReg16 (I2C_TRANSFER_LEN, (UINT16)Length);
  I2cWriteReg16 (I2C_TRANSFER_LEN_AUX, 0x0001);

  I2cWriteReg16 (I2C_DATA_PORT, RegAddr);

  I2cWriteReg16 (I2C_START, I2C_TRANSAC_START);

  if (EFI_ERROR (I2cWaitCompletion (NULL))) {
    return EFI_DEVICE_ERROR;
  }

  for (i = 0; i < Length; i++) {
    Data[i] = (UINT8)(I2cReadReg16 (I2C_DATA_PORT) & 0xFFu);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Mt6370Write (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  UINT8  buf[2];

  buf[0] = Reg;
  buf[1] = Value;
  return I2cWriteBytes (MT6370_SLAVE_ADDR, buf, 2);
}

STATIC
EFI_STATUS
Mt6370Read (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  return I2cWriteReadBytes (MT6370_SLAVE_ADDR, Reg, Value, 1);
}

STATIC
EFI_STATUS
Mt6370UpdateBits (
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Value
  )
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
Mt6370EnableHiddenMode (
  VOID
  )
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

/**
  Enable OTG boost so the port sources 5V VBUS.
  Sequence: mt6370_pmu_charger.c:1903 mt6370_enable_otg(true).
**/
STATIC
EFI_STATUS
Mt6370EnableOtg (
  OUT UINT8  *LgCtrlAfter,
  OUT UINT8  *ChgCtrl1After
  )
{
  EFI_STATUS  Status;
  UINT8       Val;

  Status = Mt6370EnableHiddenMode ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Low-side MOS gate slew rate workaround, entering OTG.
  Status = Mt6370Write (MT6370_REG_LG_CONTROL, 0x7Cu);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Switch OPA mode to boost.
  Status = Mt6370UpdateBits (MT6370_REG_CHGCTRL1, MT6370_OPA_MODE, MT6370_OPA_MODE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (20000);

  if (LgCtrlAfter != NULL) {
    Mt6370Read (MT6370_REG_LG_CONTROL, &Val);
    *LgCtrlAfter = Val;
  }
  if (ChgCtrl1After != NULL) {
    Mt6370Read (MT6370_REG_CHGCTRL1, &Val);
    *ChgCtrl1After = Val;
  }

  // Hidden CTRL6 workaround after entering OTG.
  return Mt6370Write (MT6370_REG_CHGHIDDENCTRL6, 0x00u);
}

STATIC
VOID
DrawDiagnostics (
  VOID
  )
{
  CHAR16      Buf[192];
  EFI_STATUS  Status;
  UINT8       LgCtrl;
  UINT8       ChgCtrl1;
  UINT32      Cap;

  ScreenClearAndHome ();

  UnicodeSPrint (Buf, sizeof (Buf), L"MT6771 VBUS/USB diagnostic   pass %u\r\n", mRedraws);
  ScreenLog (Buf);
  ScreenLog (L"--------------------------------------\r\n");

  UnicodeSPrint (
    Buf, sizeof (Buf),
    L"I2C6   base=0x%08x  INTR_STAT=0x%04x\r\n",
    MT6771_I2C6_BASE, I2cReadReg16 (I2C_INTR_STAT)
    );
  ScreenLog (Buf);

  LgCtrl = 0;
  ChgCtrl1 = 0;
  Status = Mt6370Read (MT6370_REG_CHGCTRL1, &ChgCtrl1);
  UnicodeSPrint (Buf, sizeof (Buf), L"MT6370 CHGCTRL1 read: %r val=0x%02x\r\n", Status, ChgCtrl1);
  ScreenLog (Buf);

  Status = Mt6370EnableOtg (&LgCtrl, &ChgCtrl1);
  UnicodeSPrint (Buf, sizeof (Buf), L"MT6370 OTG enable : %r\r\n", Status);
  ScreenLog (Buf);
  UnicodeSPrint (Buf, sizeof (Buf), L"       LG_CONTROL=0x%02x CHGCTRL1=0x%02x\r\n", LgCtrl, ChgCtrl1);
  ScreenLog (Buf);

  if (!EFI_ERROR (Status) && ((ChgCtrl1 & MT6370_OPA_MODE) != 0)) {
    ScreenLog (L"       => boost ON, port should source 5V\r\n");
  } else {
    ScreenLog (L"       => boost NOT on (i2c or pmic problem)\r\n");
  }

  ScreenLog (L"--------------------------------------\r\n");
  Cap = MmioRead32 (0x11200000u);
  UnicodeSPrint (Buf, sizeof (Buf), L"xHCI   CAP0=0x%08x caplen=%u\r\n", Cap, Cap & 0xFFu);
  ScreenLog (Buf);
  UnicodeSPrint (
    Buf, sizeof (Buf),
    L"IPPC   ctrl0=0x%08x ctrl1=0x%08x\r\n",
    MmioRead32 (0x11203E00u), MmioRead32 (0x11203E04u)
    );
  ScreenLog (Buf);

  ScreenLog (L"\r\nPlug a USB mouse: LED on => VBUS ok\r\n");
}

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mRedraws >= 10) {
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

  //
  // Try to bring VBUS up immediately so a device plugged before the timer
  // fires is already powered.
  //
  Status = Mt6370EnableOtg (NULL, NULL);
  DEBUG ((DEBUG_INFO, "Mt6771Vbus: early OTG enable -> %r\n", Status));

  Event = NULL;
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnTimer,
                  NULL,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  //
  // Redraw a few times: the shell clears the screen when it starts, so the
  // first pass may be wiped.
  //
  gBS->SetTimer (Event, TimerPeriodic, 20000000u);   // 2 s
  return EFI_SUCCESS;
}
