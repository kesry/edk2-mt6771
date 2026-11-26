/** @file

  OPPO A3 (MT6771) eMMC DXE host driver.

  Installs EFI_MMC_HOST_PROTOCOL so that
  EmbeddedPkg/Universal/MmcDxe/MmcDxe.inf can enumerate the non-removable
  eMMC and publish BlockIo. This driver is PIO-only and assumes LK has already
  configured clocks, regulators and pinmux before UEFI handoff.

  Register names/bit definitions and the low-level PIO model were derived from
  the GPL-2.0-only MediaTek MSDC WIP driver in edk2-mtk, branch origin/next-msdc:

    Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.h
    Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c
    Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcUtils.c

  Copyright (c) 2025, Arseniy Velikanov <me@adomerle.pw> (register definitions
    and low-level MSDC access model; GPL-2.0-only)
  SPDX-License-Identifier: GPL-2.0-only

**/

#include "Mt6771MmcHostDxe.h"

#include <Library/PrintLib.h>

STATIC UINTN  mMmcTraceCount;

//
// Print to the UEFI console: the RELEASE build routes DEBUG() to the unwired
// UART0, so on-screen tracing is the only way to see what the card init does.
//
STATIC
VOID
MmcTrace (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buffer[160];

  if ((gST == NULL) || (gST->ConOut == NULL)) {
    return;
  }
  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);
  gST->ConOut->OutputString (gST->ConOut, Buffer);
}


STATIC MT6771_MMC_HOST_DATA  mHost;
STATIC EFI_HANDLE             mHostHandle;

STATIC EFI_GUID gMt6771EmmcHostDevicePathGuid = {
  0x4bb9b3e8, 0x5f66, 0x49c9, { 0x9a, 0x51, 0x67, 0xa2, 0x03, 0x8e, 0x5c, 0x21 }
};

STATIC
UINT32
MsdcRead (
  IN UINT32 Offset
  )
{
  return MmioRead32 (mHost.MsdcBase + Offset);
}

STATIC
VOID
MsdcWrite (
  IN UINT32 Offset,
  IN UINT32 Value
  )
{
  MmioWrite32 (mHost.MsdcBase + Offset, Value);
}

STATIC
UINT32
TopRead (
  IN UINT32 Offset
  )
{
  return MmioRead32 (mHost.TopBase + Offset);
}

STATIC
VOID
TopWrite (
  IN UINT32 Offset,
  IN UINT32 Value
  )
{
  MmioWrite32 (mHost.TopBase + Offset, Value);
}

STATIC
VOID
MsdcSetBits (
  IN UINT32 Offset,
  IN UINT32 Mask
  )
{
  MsdcWrite (Offset, MsdcRead (Offset) | Mask);
}

STATIC
VOID
MsdcClearBits (
  IN UINT32 Offset,
  IN UINT32 Mask
  )
{
  MsdcWrite (Offset, MsdcRead (Offset) & ~Mask);
}

STATIC
VOID
TopSetBits (
  IN UINT32 Offset,
  IN UINT32 Mask
  )
{
  if (mHost.UseTop) {
    TopWrite (Offset, TopRead (Offset) | Mask);
  }
}

STATIC
VOID
TopClearBits (
  IN UINT32 Offset,
  IN UINT32 Mask
  )
{
  if (mHost.UseTop) {
    TopWrite (Offset, TopRead (Offset) & ~Mask);
  }
}

STATIC
EFI_STATUS
WaitRegisterBitsClear (
  IN UINTN   Base,
  IN UINT32  Offset,
  IN UINT32  Mask,
  IN UINT32  TimeoutUs
  )
{
  UINT32  Value;
  UINT32  Elapsed;

  Value = 0;
  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    Value = MmioRead32 (Base + Offset);
    if ((Value & Mask) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }

  DEBUG ((DEBUG_ERROR, "Mt6771Mmc: timeout clearing 0x%08x at 0x%p+0x%x (0x%08x)\n",
          Mask, (VOID *)Base, Offset, Value));
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
WaitRegisterBitsSet (
  IN UINTN   Base,
  IN UINT32  Offset,
  IN UINT32  Mask,
  IN UINT32  TimeoutUs
  )
{
  UINT32  Value;
  UINT32  Elapsed;

  Value = 0;
  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    Value = MmioRead32 (Base + Offset);
    if ((Value & Mask) != 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }

  DEBUG ((DEBUG_ERROR, "Mt6771Mmc: timeout waiting 0x%08x at 0x%p+0x%x (0x%08x)\n",
          Mask, (VOID *)Base, Offset, Value));
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
MsdcResetHardware (
  VOID
  )
{
  EFI_STATUS  Status;

  MsdcSetBits (MSDC_CFG, MSDC_CFG_RST);
  Status = WaitRegisterBitsClear (mHost.MsdcBase, MSDC_CFG, MSDC_CFG_RST,
                                  MT6771_BUSY_WAIT_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MsdcSetBits (MSDC_FIFOCS, MSDC_FIFOCS_CLR);
  Status = WaitRegisterBitsClear (mHost.MsdcBase, MSDC_FIFOCS, MSDC_FIFOCS_CLR,
                                  MT6771_BUSY_WAIT_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MsdcWrite (MSDC_INT, 0xffffffffu);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MsdcSetBusWidth (
  IN UINT32 Width
  )
{
  UINT32  Value;

  switch (Width) {
    case 1:
      Value = MSDC_BUS_WIDTH_1;
      break;
    case 4:
      Value = MSDC_BUS_WIDTH_4;
      break;
    case 8:
      Value = MSDC_BUS_WIDTH_8;
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  Value <<= SDC_CFG_BUS_WIDTH_SHIFT;
  MsdcWrite (
    SDC_CFG,
    (MsdcRead (SDC_CFG) & ~(3u << SDC_CFG_BUS_WIDTH_SHIFT)) |
    (Value & (3u << SDC_CFG_BUS_WIDTH_SHIFT))
    );
  mHost.BusWidth = Width;
  return EFI_SUCCESS;
}

STATIC
VOID
MsdcSetTimeout (
  VOID
  )
{
  UINT32  Cfg;
  UINT32  Timeout;

  //
  // Conservative data timeout: 100 ms worth of card time plus
  // 3 * 2^20 controller clocks, copied from next-msdc:MsdcDxe.c.
  //
  Timeout = 0;
  if (mHost.SclkHz != 0) {
    Timeout = 100000000u / mHost.SclkHz;
    Timeout += 3u << SCLK_CYCLES_SHIFT;
    Timeout = (Timeout + (1u << SCLK_CYCLES_SHIFT) - 1u) >> SCLK_CYCLES_SHIFT;
    if (Timeout > 0) {
      Timeout--;
    }
    if (Timeout > 0xff) {
      Timeout = 0xff;
    }
  }

  Cfg = MsdcRead (SDC_CFG) & ~(0xffu << 24);
  MsdcWrite (SDC_CFG, Cfg | (Timeout << 24));
}

STATIC
EFI_STATUS
MsdcSetClock (
  IN UINT32 RequestedHz
  )
{
  EFI_STATUS  Status;
  UINT32      Cfg;
  UINT32      Div;
  UINT32      Mode;

  //
  // Do NOT call any EDK2 clock protocol. LK is assumed to have selected
  // clk_msdcpll (~400 MHz) and enabled hclk. SourceClockHz is a PCD so a
  // board bring-up can correct it without code changes.
  //
  if ((RequestedHz == 0) || (mHost.SourceClockHz == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (RequestedHz >= mHost.SourceClockHz) {
    Mode         = MSDC_MCLK_NO_DIV;
    Div          = 0;
    mHost.SclkHz = mHost.SourceClockHz;
  } else if (RequestedHz >= (mHost.SourceClockHz >> 1)) {
    Mode         = MSDC_MCLK_DIV;
    Div          = 0;
    mHost.SclkHz = mHost.SourceClockHz >> 1;
  } else {
    Mode = MSDC_MCLK_DIV;
    Div  = (mHost.SourceClockHz + ((RequestedHz << 2) - 1u)) / (RequestedHz << 2);
    if (Div == 0) {
      Div = 1;
    }
    mHost.SclkHz = (mHost.SourceClockHz >> 2) / Div;
  }

  Cfg  = MsdcRead (MSDC_CFG);
  Cfg &= ~(3u << MSDC_CFG_CKMD_SHIFT);
  Cfg &= ~(0xfffu << MSDC_CFG_CKDIV_SHIFT);
  Cfg &= ~MSDC_CFG_HS400CKMD;
  Cfg |= (Mode << MSDC_CFG_CKMD_SHIFT) | (Div << MSDC_CFG_CKDIV_SHIFT);

  MsdcClearBits (MSDC_CFG, MSDC_CFG_CCKPD);
  MsdcWrite (MSDC_CFG, Cfg);

  Status = WaitRegisterBitsSet (mHost.MsdcBase, MSDC_CFG, MSDC_CFG_CCKSB,
                                MT6771_BUSY_WAIT_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Mt6771Mmc: clock not stable: %r\n", Status));
    return Status;
  }

  MsdcSetBits (MSDC_CFG, MSDC_CFG_CCKPD);
  MsdcSetTimeout ();

  DEBUG ((DEBUG_INFO, "Mt6771Mmc: source=%uHz request=%uHz sclk=%uHz mode=%u div=%u\n",
          mHost.SourceClockHz, RequestedHz, mHost.SclkHz, Mode, Div));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MsdcWaitCommandDone (
  OUT UINT32 *IntStatus
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINT32      Elapsed;

  Value = 0;
  for (Elapsed = 0; Elapsed < MT6771_CMD_WAIT_TIMEOUT_US; Elapsed += 10) {
    Value = MsdcRead (MSDC_INT);
    if ((Value & MSDC_INT_CMDSTS) != 0) {
      *IntStatus = Value;
      MsdcWrite (MSDC_INT, Value & MSDC_INT_CMDSTS);
      if ((Value & MSDC_INT_CMDRDY) != 0) {
        return EFI_SUCCESS;
      }
      if ((Value & (MSDC_INT_CMDTMO | MSDC_INT_ACMDTMO)) != 0) {
        return EFI_TIMEOUT;
      }
      if ((Value & (MSDC_INT_CMDCRCERR | MSDC_INT_ACMDCRCERR)) != 0) {
        return EFI_CRC_ERROR;
      }
      return EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (10);
  }

  *IntStatus = Value;
  Status = EFI_TIMEOUT;
  DEBUG ((DEBUG_ERROR, "Mt6771Mmc: command timeout, MSDC_INT=0x%08x\n", Value));
  return Status;
}

STATIC
EFI_STATUS
MsdcWaitDataDone (
  OUT UINT32 *IntStatus
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINT32      Elapsed;

  Value = 0;
  for (Elapsed = 0; Elapsed < MT6771_DAT_WAIT_TIMEOUT_US; Elapsed += 10) {
    Value = MsdcRead (MSDC_INT);
    if ((Value & MSDC_INT_DATTMO) != 0) {
      *IntStatus = Value;
      MsdcWrite (MSDC_INT, Value & MSDC_INT_DATSTS);
      return EFI_TIMEOUT;
    }
    if ((Value & MSDC_INT_DATCRCERR) != 0) {
      *IntStatus = Value;
      MsdcWrite (MSDC_INT, Value & MSDC_INT_DATSTS);
      return EFI_CRC_ERROR;
    }
    if ((Value & MSDC_INT_XFER_COMPL) != 0) {
      *IntStatus = Value;
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }

  *IntStatus = Value;
  Status = EFI_TIMEOUT;
  DEBUG ((DEBUG_ERROR, "Mt6771Mmc: data timeout, MSDC_INT=0x%08x\n", Value));
  return Status;
}

STATIC
EFI_STATUS
MsdcPumpRxFifo (
  IN OUT UINT8  **Buffer,
  IN OUT UINTN  *Remaining
  )
{
  UINTN  Chunk;
  UINT8  *Byte;

  Byte = *Buffer;
  while (*Remaining > 0) {
    Chunk = MsdcRead (MSDC_FIFOCS) & MSDC_FIFO_RXBYTES_MASK;
    if (Chunk == 0) {
      break;
    }
    if (Chunk > *Remaining) {
      Chunk = *Remaining;
    }

    if ((Chunk >= 4) && (((UINTN)Byte & 3u) == 0)) {
      while (Chunk >= 4) {
        *(UINT32 *)Byte = MmioRead32 (mHost.MsdcBase + MSDC_RXDATA);
        Byte += 4;
        *Remaining -= 4;
        Chunk -= 4;
      }
    }
    while (Chunk > 0) {
      *Byte++ = (UINT8)MmioRead32 (mHost.MsdcBase + MSDC_RXDATA);
      (*Remaining)--;
      Chunk--;
    }
  }

  *Buffer = Byte;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MsdcPumpTxFifo (
  IN OUT UINT8  **Buffer,
  IN OUT UINTN  *Remaining
  )
{
  UINTN  Space;
  UINTN  Chunk;
  UINT8  *Byte;

  Byte = *Buffer;
  while (*Remaining > 0) {
    Space = (MsdcRead (MSDC_FIFOCS) & MSDC_FIFO_TXBYTES_MASK) >>
            MSDC_FIFO_TXBYTES_SHIFT;
    if (Space >= MT6771_MSDC_FIFO_SIZE) {
      break;
    }
    Space = MT6771_MSDC_FIFO_SIZE - Space;
    Chunk = (*Remaining < Space) ? *Remaining : Space;

    if ((Chunk >= 4) && (((UINTN)Byte & 3u) == 0)) {
      Chunk &= ~3u;
      while (Chunk >= 4) {
        MmioWrite32 (mHost.MsdcBase + MSDC_TXDATA, *(UINT32 *)Byte);
        Byte += 4;
        *Remaining -= 4;
        Chunk -= 4;
      }
    }
    while (Chunk > 0) {
      MmioWrite32 (mHost.MsdcBase + MSDC_TXDATA, *Byte++);
      (*Remaining)--;
      Chunk--;
    }
  }

  *Buffer = Byte;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MsdcPioRead (
  IN VOID   *Buffer,
  IN UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINT8       *Byte;
  UINTN       Remaining;
  UINT32      IntStatus;

  if ((Buffer == NULL) || ((Length % MT6771_MSDC_BLOCK_SIZE) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Byte      = Buffer;
  Remaining = Length;

  while (TRUE) {
    Status = MsdcWaitDataDone (&IntStatus);
    if (Status == EFI_SUCCESS) {
      MsdcPumpRxFifo (&Byte, &Remaining);
    } else {
      DEBUG ((DEBUG_ERROR, "Mt6771Mmc: read data error/timeout %r, left=%u\n",
              Status, Remaining));
    }

    MsdcWrite (MSDC_INT, IntStatus & MSDC_INT_XFER_COMPL);
    MsdcClearBits (MSDC_INTEN, MSDC_INT_DATSTS);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Remaining != 0) {
      DEBUG ((DEBUG_ERROR, "Mt6771Mmc: XFER_COMPL with %u bytes left\n", Remaining));
      return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
  }
}

STATIC
EFI_STATUS
MsdcPioWrite (
  IN VOID   *Buffer,
  IN UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINT8       *Byte;
  UINTN       Remaining;
  UINT32      IntStatus;

  if ((Buffer == NULL) || ((Length % MT6771_MSDC_BLOCK_SIZE) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Byte      = Buffer;
  Remaining = Length;

  while (TRUE) {
    MsdcPumpTxFifo (&Byte, &Remaining);
    Status = MsdcWaitDataDone (&IntStatus);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Mt6771Mmc: write data error/timeout %r, left=%u\n",
              Status, Remaining));
    }

    MsdcWrite (MSDC_INT, IntStatus & MSDC_INT_XFER_COMPL);
    MsdcClearBits (MSDC_INTEN, MSDC_INT_DATSTS);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Remaining != 0) {
      DEBUG ((DEBUG_ERROR, "Mt6771Mmc: XFER_COMPL with %u bytes left\n", Remaining));
      return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
  }
}

STATIC
BOOLEAN
EFIAPI
Mt6771MmcIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL *This
  )
{
  return TRUE;
}

STATIC
BOOLEAN
EFIAPI
Mt6771MmcIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL *This
  )
{
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcBuildDevicePath (
  IN  EFI_MMC_HOST_PROTOCOL    *This,
  OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath
  )
{
  VENDOR_DEVICE_PATH  *Node;

  if (DevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Node = AllocateZeroPool (sizeof (VENDOR_DEVICE_PATH));
  if (Node == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Node->Header.Type     = HARDWARE_DEVICE_PATH;
  Node->Header.SubType  = HW_VENDOR_DP;
  SetDevicePathNodeLength (&Node->Header, sizeof (VENDOR_DEVICE_PATH));

  //
  // Stable vendor DP GUID. MmcDxe appends this node to an End node.
  //
  CopyMem (&Node->Guid, &gMt6771EmmcHostDevicePathGuid, sizeof (EFI_GUID));

  *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Node;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcNotifyState (
  IN EFI_MMC_HOST_PROTOCOL *This,
  IN MMC_STATE             State
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcSendCommand (
  IN EFI_MMC_HOST_PROTOCOL *This,
  IN MMC_CMD               Cmd,
  IN UINT32                Argument
  )
{
  EFI_STATUS  Status;
  UINT32      Index;
  UINT32      RawCmd;
  UINT32      ResponseType;
  UINT32      IntStatus;

  Index = MMC_GET_INDX (Cmd);
  if (Index > 0x3f) {
    return EFI_INVALID_PARAMETER;
  }

  if (Index == 23) {
    mHost.PendingBlocks = Argument;
  }

  mHost.LastCommand = Cmd;
  ZeroMem (mHost.Response, sizeof (mHost.Response));

  RawCmd = Index & 0x3f;

  if ((Cmd & MMC_CMD_NO_CRC_RESPONSE) != 0) {
    ResponseType = 0;
  } else if ((Cmd & MMC_CMD_LONG_RESPONSE) != 0) {
    ResponseType = 2;
  } else {
    ResponseType = 1;
  }

  //
  // CMD12 is an R1b stop command on eMMC; mainline mtk-sd uses response type 7.
  // See mtk-sd.c:msdc_cmd_find_resp() and msdc_cmd_prepare_raw_cmd().
  //
  if (Index == 12) {
    ResponseType = 7;
  }
  RawCmd |= ResponseType << SDC_CMD_RSP_TYPE_SHIFT;

  switch (Index) {
    case 8:
      //
      // eMMC CMD8 sends EXTCSD as a 512-byte data block.
      // See MmcIdentification.c:EmmcIdentificationMode().
      //
      RawCmd |= SDC_CMD_SINGLE_BLK | (MT6771_MSDC_BLOCK_SIZE << SDC_CMD_BLK_SIZE_SHIFT);
      MsdcWrite (SDC_BLK_NUM, 1);
      break;
    case 17:
      RawCmd |= SDC_CMD_SINGLE_BLK | (MT6771_MSDC_BLOCK_SIZE << SDC_CMD_BLK_SIZE_SHIFT);
      MsdcWrite (SDC_BLK_NUM, 1);
      break;
    case 18:
      RawCmd |= SDC_CMD_MULTIPLE_BLK | (MT6771_MSDC_BLOCK_SIZE << SDC_CMD_BLK_SIZE_SHIFT);
      //
      // MmcDxe issues CMD23 with the block count just before CMD18/CMD25.
      // Use the controller's AutoCMD23 mode (mtk-sd.c rawcmd bit 29).
      //
      RawCmd |= BIT29;
      if (mHost.PendingBlocks > 0) {
        MsdcWrite (SDC_BLK_NUM, mHost.PendingBlocks);
      }
      break;
    case 24:
      RawCmd |= SDC_CMD_RW | SDC_CMD_SINGLE_BLK |
                (MT6771_MSDC_BLOCK_SIZE << SDC_CMD_BLK_SIZE_SHIFT);
      MsdcWrite (SDC_BLK_NUM, 1);
      break;
    case 25:
      RawCmd |= SDC_CMD_RW | SDC_CMD_MULTIPLE_BLK |
                (MT6771_MSDC_BLOCK_SIZE << SDC_CMD_BLK_SIZE_SHIFT);
      RawCmd |= BIT29;
      if (mHost.PendingBlocks > 0) {
        MsdcWrite (SDC_BLK_NUM, mHost.PendingBlocks);
      }
      break;
    case 12:
      RawCmd |= SDC_CMD_STOP_CMD;
      break;
    default:
      break;
  }

  Status = MsdcResetHardware ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MsdcClearBits (MSDC_INTEN, MSDC_INT_CMDSTS);
  MsdcWrite (SDC_ARG, Argument);
  MsdcWrite (SDC_CMD, RawCmd);

  DEBUG ((DEBUG_BLKIO, "Mt6771Mmc: CMD%d arg=0x%08x raw=0x%08x\n",
          Index, Argument, RawCmd));

  Status = MsdcWaitCommandDone (&IntStatus);

  if (mMmcTraceCount < 32) {
    TraceInt = MsdcRead (MSDC_INT);
    MmcTrace (
      L"MMC CMD%u arg=%08x raw=%04x -> %r  INT=%04x\r\n",
      MMC_GET_INDX (Cmd), Argument, RawCmd, Status, TraceInt
      );
    mMmcTraceCount++;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Cmd & MMC_CMD_NO_CRC_RESPONSE) == 0) {
    if ((Cmd & MMC_CMD_LONG_RESPONSE) != 0) {
      mHost.Response[3] = MsdcRead (SDC_RESP3);
      mHost.Response[2] = MsdcRead (SDC_RESP2);
      mHost.Response[1] = MsdcRead (SDC_RESP1);
      mHost.Response[0] = MsdcRead (SDC_RESP0);
    } else {
      mHost.Response[0] = MsdcRead (SDC_RESP0);
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcReceiveResponse (
  IN  EFI_MMC_HOST_PROTOCOL *This,
  IN  MMC_RESPONSE_TYPE     Type,
  OUT UINT32                *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mHost.Response, sizeof (mHost.Response));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcReadBlockData (
  IN  EFI_MMC_HOST_PROTOCOL *This,
  IN  EFI_LBA               Lba,
  IN  UINTN                 Length,
  OUT UINT32                *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return MsdcPioRead (Buffer, Length);
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcWriteBlockData (
  IN  EFI_MMC_HOST_PROTOCOL *This,
  IN  EFI_LBA               Lba,
  IN  UINTN                 Length,
  IN  UINT32                *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return MsdcPioWrite (Buffer, Length);
}

STATIC
EFI_STATUS
EFIAPI
Mt6771MmcSetIos (
  IN EFI_MMC_HOST_PROTOCOL *This,
  IN UINT32                BusClockFreq,
  IN UINT32                BusWidth,
  IN UINT32                TimingMode
  )
{
  EFI_STATUS  Status;

  //
  // MmcDxe calls SetIos(0, 1, EMMCBACKWARD) during identification. Keep the
  // clock produced by LK in that case; only change the bus width.
  //
  if (BusClockFreq != 0) {
    Status = MsdcSetClock (BusClockFreq);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return MsdcSetBusWidth (BusWidth);
}

STATIC
BOOLEAN
EFIAPI
Mt6771MmcIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL *This
  )
{
  return TRUE;
}

STATIC
EFI_STATUS
Mt6771MmcHostInit (
  VOID
  )
{
  EFI_STATUS  Status;

  MmcTrace (
    L"MSDC regs before: CFG=%04x SDC_CFG=%04x PS=%04x INTEN=%04x\r\n",
    MmioRead16 (FixedPcdGet32 (PcdMsdcMmioRegBase) + MSDC_CFG),
    MmioRead16 (FixedPcdGet32 (PcdMsdcMmioRegBase) + SDC_CFG),
    MmioRead16 (FixedPcdGet32 (PcdMsdcMmioRegBase) + MSDC_PS),
    MmioRead16 (FixedPcdGet32 (PcdMsdcMmioRegBase) + MSDC_INTEN)
    );

  mHost.MsdcBase      = FixedPcdGet32 (PcdMsdcMmioRegBase);
  mHost.TopBase       = FixedPcdGet32 (PcdMsdcTopMmioRegBase);
  mHost.UseTop        = FixedPcdGetBool (PcdMsdcUseTop);
  mHost.SourceClockHz = FixedPcdGet32 (PcdMsdcSourceClockHz);
  mHost.BusWidth      = 1;

  //
  // Leave clock/pin/regulator setup to LK. We only select PIO and reset the
  // controller. Clock is set later by MmcDxe through SetIos().
  //
  MsdcSetBits (MSDC_CFG, MSDC_CFG_MODE);
  MsdcSetBits (MSDC_CFG, MSDC_CFG_PIO);

  Status = MsdcResetHardware ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MsdcWrite (MSDC_INTEN, 0);

  //
  // FIXME: these quirks came from the MT6789 next-msdc WIP. They are marked
  // FIXME in that driver too. Keep them for first silicon bring-up, but
  // remove/disable individually if eMMC enumeration fails.
  //
  MsdcSetBits (SDC_CFG, SDC_CFG_SDIO);
  MsdcClearBits (SDC_CFG, SDC_CFG_SDIOIDE);

  MsdcWrite (MSDC_IOCON, 0);
  MsdcWrite (MSDC_PATCH_BIT0, 0x403c0046u);
  MsdcWrite (MSDC_PATCH_BIT1, 0xffff4089u);
  MsdcSetBits (MSDC_PATCH_BIT0, BIT10);
  MsdcSetBits (EMMC50_CFG0, EMMC50_CFG0_CRCSTSSEL);
  MsdcSetBits (MSDC_PATCH_BIT1, BIT8 | BIT9);
  MsdcClearBits (SDC_FIFO_CFG, SDC_FIFO_CFG_WRVALIDSEL);
  MsdcClearBits (SDC_FIFO_CFG, SDC_FIFO_CFG_RDVALIDSEL);
  MsdcClearBits (MSDC_PATCH_BIT1, MSDC_PB1_BUSYCHECKSEL);

  if (mHost.UseTop) {
    TopWrite (TOP_CTRL, 0);
    TopWrite (TOP_CMD, 0);
    TopSetBits (TOP_CTRL, SDC_RX_ENH_EN);
    TopSetBits (TOP_CTRL, PAD_DAT_RD_RXDLY_SEL);
    TopClearBits (TOP_CTRL, DATA_K_VALUE_SEL);
    TopSetBits (TOP_CMD, PAD_CMD_RD_RXDLY_SEL);
  } else {
    MsdcSetBits (SDC_ADV_CFG0, SDC_RX_ENHANCE_EN);
  }

  MsdcSetBits (MSDC_PATCH_BIT2, BIT2 | BIT3);
  MsdcClearBits (MSDC_PATCH_BIT2, MSDC_PB2_CFGRESP);
  MsdcSetBits (MSDC_PATCH_BIT2, MSDC_PB2_CFGCRCSTS);

  mHost.SclkHz = mHost.SourceClockHz;
  MsdcSetTimeout ();

  MmcTrace (
    L"MSDC regs after : CFG=%04x SDC_CFG=%04x PS=%04x INTEN=%04x\r\n",
    MmioRead16 (mHost.MsdcBase + MSDC_CFG),
    MmioRead16 (mHost.MsdcBase + SDC_CFG),
    MmioRead16 (mHost.MsdcBase + MSDC_PS),
    MmioRead16 (mHost.MsdcBase + MSDC_INTEN)
    );

  return MsdcSetBusWidth (1);
}

EFI_STATUS
EFIAPI
Mt6771MmcHostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_MMC_HOST_PROTOCOL  *Host;
  EFI_STATUS              Status;
  EFI_MMC_HOST_PROTOCOL   Template = {
    MMC_HOST_PROTOCOL_REVISION,
    Mt6771MmcIsCardPresent,
    Mt6771MmcIsReadOnly,
    Mt6771MmcBuildDevicePath,
    Mt6771MmcNotifyState,
    Mt6771MmcSendCommand,
    Mt6771MmcReceiveResponse,
    Mt6771MmcReadBlockData,
    Mt6771MmcWriteBlockData,
    Mt6771MmcSetIos,
    Mt6771MmcIsMultiBlock
  };

  Status = Mt6771MmcHostInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Mt6771Mmc: init failed: %r\n", Status));
    return Status;
  }

  Host = AllocateCopyPool (sizeof (EFI_MMC_HOST_PROTOCOL), &Template);
  if (Host == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Host->IsCardPresent  = Mt6771MmcIsCardPresent;
  Host->IsReadOnly     = Mt6771MmcIsReadOnly;
  Host->BuildDevicePath = Mt6771MmcBuildDevicePath;
  Host->NotifyState     = Mt6771MmcNotifyState;
  Host->SendCommand     = Mt6771MmcSendCommand;
  Host->ReceiveResponse = Mt6771MmcReceiveResponse;
  Host->ReadBlockData   = Mt6771MmcReadBlockData;
  Host->WriteBlockData  = Mt6771MmcWriteBlockData;
  Host->SetIos          = Mt6771MmcSetIos;
  Host->IsMultiBlock    = Mt6771MmcIsMultiBlock;

  mHostHandle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &mHostHandle,
                  &gEmbeddedMmcHostProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  Host
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Mt6771Mmc: install MmcHost failed: %r\n", Status));
    FreePool (Host);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "Mt6771Mmc: installed at 0x%x (top 0x%x), source=%uHz\n",
          mHost.MsdcBase, mHost.TopBase, mHost.SourceClockHz));
  return EFI_SUCCESS;
}
