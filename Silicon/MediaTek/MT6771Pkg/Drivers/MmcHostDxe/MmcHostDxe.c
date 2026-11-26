/** @file

  MT6771 MSDC/eMMC DXE host implementation.

  See MmcHostDxe.h for provenance and hardware facts.

  Copyright (c) 2026, kesry. All rights reserved.
  SPDX-License-Identifier: GPL-2.0-only

**/

#include "MmcHostDxe.h"

STATIC MT6771_MMC_HOST  mMmcHost;

//
// Stable vendor DP GUID for the single soldered-down eMMC device.
//
STATIC CONST EFI_GUID  gMt6771MmcDevicePathGuid = {
  0x2f7f6a70, 0x5f1d, 0x4b6c, { 0x9a, 0xd1, 0x32, 0x1c, 0x7e, 0x6d, 0x4a, 0x20 }
};

STATIC VOID
MsdcWrite (IN UINT32 Offset, IN UINT32 Value)
{
  MmioWrite32 (mMmcHost.Base + Offset, Value);
}

STATIC UINT32
MsdcRead (IN UINT32 Offset)
{
  return MmioRead32 (mMmcHost.Base + Offset);
}

STATIC VOID
MsdcSetBits (IN UINT32 Offset, IN UINT32 Mask)
{
  MsdcWrite (Offset, MsdcRead (Offset) | Mask);
}

STATIC VOID
MsdcClrBits (IN UINT32 Offset, IN UINT32 Mask)
{
  MsdcWrite (Offset, MsdcRead (Offset) & ~Mask);
}

STATIC VOID
TopWrite (IN UINT32 Offset, IN UINT32 Value)
{
  MmioWrite32 (mMmcHost.TopBase + Offset, Value);
}

STATIC VOID
TopSetBits (IN UINT32 Offset, IN UINT32 Mask)
{
  MmioWrite32 (mMmcHost.TopBase + Offset, MmioRead32 (mMmcHost.TopBase + Offset) | Mask);
}

STATIC VOID
TopClrBits (IN UINT32 Offset, IN UINT32 Mask)
{
  MmioWrite32 (mMmcHost.TopBase + Offset, MmioRead32 (mMmcHost.TopBase + Offset) & ~Mask);
}

STATIC EFI_STATUS
MsdcWaitBitClear (IN UINT32 Offset, IN UINT32 Mask, IN UINTN Retries)
{
  while (Retries-- > 0) {
    if ((MsdcRead (Offset) & Mask) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (100);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
MsdcWaitBitSet (IN UINT32 Offset, IN UINT32 Mask, IN UINTN Retries)
{
  while (Retries-- > 0) {
    if ((MsdcRead (Offset) & Mask) != 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (100);
  }
  return EFI_TIMEOUT;
}

STATIC VOID
MsdcResetController (VOID)
{
  MsdcSetBits (MSDC_CFG, MSDC_CFG_RST);
  MsdcWaitBitClear (MSDC_CFG, MSDC_CFG_RST, 1000);
}

STATIC VOID
MsdcClearFifo (VOID)
{
  MsdcSetBits (MSDC_FIFOCS, MSDC_FIFOCS_CLR);
  MsdcWaitBitClear (MSDC_FIFOCS, MSDC_FIFOCS_CLR, 1000);
}

STATIC VOID
MsdcSetBusWidth (IN UINT32 Width)
{
  UINT32 Reg;

  Reg = MsdcRead (SDC_CFG);
  Reg &= ~(0x3u << SDC_CFG_BUS_WIDTH_SHIFT);
  Reg |= Width << SDC_CFG_BUS_WIDTH_SHIFT;
  MsdcWrite (SDC_CFG, Reg);
}

STATIC VOID
MsdcInitController (VOID)
{
  if (mMmcHost.InitializationDone) {
    return;
  }

  DEBUG ((DEBUG_INFO, "MT6771 MmcHost: initializing MSDC0 @ 0x%x\n", mMmcHost.Base));

  // LK has already enabled clocks, power and MSDC pin mux. We only configure
  // the controller into PIO mode and keep the existing clock divider.
  MsdcSetBits (MSDC_CFG, MSDC_CFG_MODE);
  MsdcSetBits (MSDC_CFG, MSDC_CFG_CKPDN);
  MsdcSetBits (MSDC_CFG, MSDC_CFG_PIO);
  MsdcResetController ();
  MsdcClearFifo ();
  MsdcWrite (MSDC_INTEN, 0);

  MsdcSetBits (SDC_CFG, SDC_CFG_SDIO);
  MsdcClrBits (SDC_CFG, SDC_CFG_SDIOIDE);

  // MT6779 compatible quirks, cross-checked with Linux mtk-sd.c.
  MsdcWrite (MSDC_PATCH_BIT0, 0x403c0046);
  MsdcWrite (MSDC_PATCH_BIT1, 0xffff4089);
  MsdcSetBits (MSDC_PATCH_BIT0, BIT10);
  MsdcSetBits (EMMC50_CFG0, BIT4);
  MsdcSetBits (MSDC_PATCH_BIT1, BIT8 | BIT9);
  MsdcClrBits (SDC_FIFO_CFG, SDC_FIFO_CFG_WRVALIDSEL | SDC_FIFO_CFG_RDVALIDSEL);
  MsdcClrBits (MSDC_PATCH_BIT1, MSDC_PB1_BUSYCHECKSEL);
  MsdcSetBits (MSDC_PATCH_BIT2, BIT2 | BIT3);
  TopSetBits (TOP_CTRL, TOP_CTRL_SDC_RX_ENH_EN);
  TopSetBits (TOP_CTRL, BIT13);
  TopClrBits (TOP_CTRL, BIT14);
  TopSetBits (TOP_CMD, BIT11);
  MsdcClrBits (MSDC_PATCH_BIT2, MSDC_PB2_CFGRESP);
  MsdcSetBits (MSDC_PATCH_BIT2, MSDC_PB2_CFGCRCSTS);

  MsdcWrite (MSDC_IOCON, 0);
  MsdcSetBusWidth (SDC_CFG_BUS_WIDTH_8BIT);

  // Use a conservative timeout: 0xff in the high byte of SDC_CFG.
  MsdcWrite (SDC_CFG, (MsdcRead (SDC_CFG) & ~(0xffu << 24)) | (0xffu << 24));

  mMmcHost.Sclk = MT6771_MSDC_SCLK_HZ;
  mMmcHost.InitializationDone = TRUE;
  DEBUG ((DEBUG_INFO, "MT6771 MmcHost: init done\n"));
}

STATIC EFI_STATUS
MsdcWaitCommandDone (VOID)
{
  UINT32 Int;

  for (;;) {
    Int = MsdcRead (MSDC_INT);
    if (Int & (MSDC_INT_CMDTMO | MSDC_INT_CMDCRCERR)) {
      MsdcWrite (MSDC_INT, Int);
      return EFI_DEVICE_ERROR;
    }
    if (Int & MSDC_INT_CMDRDY) {
      MsdcWrite (MSDC_INT, Int);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (100);
  }
}

STATIC EFI_STATUS
MsdcFifoReadBytes (OUT UINT8 *Buffer, IN UINTN Length)
{
  while (Length >= 4) {
    *(UINT32 *)Buffer = MmioRead32 (mMmcHost.Base + MSDC_RXDATA);
    Buffer += 4;
    Length -= 4;
  }
  while (Length > 0) {
    *Buffer = (UINT8)MmioRead32 (mMmcHost.Base + MSDC_RXDATA);
    Buffer++;
    Length--;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MsdcFifoWriteBytes (IN UINT8 *Buffer, IN UINTN Length)
{
  while (Length >= 4) {
    MmioWrite32 (mMmcHost.Base + MSDC_TXDATA, *(UINT32 *)Buffer);
    Buffer += 4;
    Length -= 4;
  }
  while (Length > 0) {
    MmioWrite32 (mMmcHost.Base + MSDC_TXDATA, *Buffer);
    Buffer++;
    Length--;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MsdcReadData (OUT UINT8 *Buffer, IN UINTN Length)
{
  UINT32 Int;
  UINT32 RxBytes;
  UINTN  Chunk;

  while (Length > 0) {
    Int = MsdcRead (MSDC_INT);
    if (Int & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)) {
      MsdcWrite (MSDC_INT, Int);
      return EFI_DEVICE_ERROR;
    }

    RxBytes = (MsdcRead (MSDC_FIFOCS) & MSDC_FIFOCS_RXCNT);
    Chunk = RxBytes < Length ? RxBytes : Length;
    if (Chunk > 0) {
      MsdcFifoReadBytes (Buffer, Chunk);
      Buffer += Chunk;
      Length -= Chunk;
    }
    MicroSecondDelay (10);
  }

  // Wait for transfer complete and acknowledge it.
  for (;;) {
    Int = MsdcRead (MSDC_INT);
    if (Int & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)) {
      MsdcWrite (MSDC_INT, Int);
      return EFI_DEVICE_ERROR;
    }
    if (Int & MSDC_INT_XFER_COMPL) {
      MsdcWrite (MSDC_INT, Int & MSDC_INT_XFER_COMPL);
      break;
    }
    MicroSecondDelay (10);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MsdcWriteData (IN UINT8 *Buffer, IN UINTN Length)
{
  UINT32 Int;
  UINT32 TxBytes;
  UINTN  Chunk;

  while (Length > 0) {
    Int = MsdcRead (MSDC_INT);
    if (Int & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)) {
      MsdcWrite (MSDC_INT, Int);
      return EFI_DEVICE_ERROR;
    }

    TxBytes = (MsdcRead (MSDC_FIFOCS) & MSDC_FIFOCS_TXCNT) >> 16;
    Chunk = (MT6771_MSDC_FIFO_SIZE - TxBytes) < Length
            ? (MT6771_MSDC_FIFO_SIZE - TxBytes) : Length;
    if (Chunk > 0) {
      MsdcFifoWriteBytes (Buffer, Chunk);
      Buffer += Chunk;
      Length -= Chunk;
    }
    MicroSecondDelay (10);
  }

  for (;;) {
    Int = MsdcRead (MSDC_INT);
    if (Int & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)) {
      MsdcWrite (MSDC_INT, Int);
      return EFI_DEVICE_ERROR;
    }
    if (Int & MSDC_INT_XFER_COMPL) {
      MsdcWrite (MSDC_INT, Int & MSDC_INT_XFER_COMPL);
      break;
    }
    MicroSecondDelay (10);
  }
  return EFI_SUCCESS;
}

STATIC UINT32
MmcCmdToMsdcRaw (
  IN MMC_CMD Cmd,
  IN BOOLEAN HasData,
  IN BOOLEAN IsWrite,
  IN BOOLEAN MultiBlock
  )
{
  UINT32 Index = MMC_GET_INDX (Cmd);
  UINT32 Raw = Index;

  if (Cmd & MMC_CMD_WAIT_RESPONSE) {
    UINT32 RspType = (Cmd & MMC_CMD_LONG_RESPONSE) ? MMC_RSP_R2 : MMC_RSP_R1;
    Raw |= RspType << SDC_CMD_RSP_TYPE_SHIFT;
  }

  if (HasData) {
    Raw |= (512u << SDC_CMD_BLK_SIZE_SHIFT);
    if (IsWrite) {
      Raw |= SDC_CMD_RW;
    }
    Raw |= MultiBlock ? SDC_CMD_MULTIPLE_BLK : SDC_CMD_SINGLE_BLK;
  }

  if (Index == 12) {
    Raw |= SDC_CMD_STOP_CMD;
  }

  return Raw;
}

STATIC EFI_STATUS
MmcHostSendCommand (
  IN EFI_MMC_HOST_PROTOCOL *This,
  IN MMC_CMD               MmcCmd,
  IN UINT32                Argument
  )
{
  UINT32 Raw;
  UINT32 Index;

  MsdcInitController ();
  Index = MMC_GET_INDX (MmcCmd);

  Raw = MmcCmdToMsdcRaw (
          MmcCmd,
          (Index == 17) || (Index == 18) || (Index == 24) || (Index == 25) ||
          (Index == 6) || (Index == 8) || (Index == 51),
          (Index == 24) || (Index == 25) || (Index == 6),
          (Index == 18) || (Index == 25)
          );

  MsdcClrBits (MSDC_INTEN, MSDC_INT_CMDRDY);
  MsdcWrite (SDC_ARG, Argument);
  MsdcWrite (SDC_CMD, Raw);

  return MsdcWaitCommandDone ();
}

STATIC EFI_STATUS
MmcHostReceiveResponse (
  IN  EFI_MMC_HOST_PROTOCOL *This,
  IN  MMC_RESPONSE_TYPE     Type,
  OUT UINT32                *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Type == MMC_RESPONSE_TYPE_R2 || Type == MMC_RESPONSE_TYPE_CID ||
      Type == MMC_RESPONSE_TYPE_CSD) {
    Buffer[0] = MsdcRead (SDC_RESP0);
    Buffer[1] = MsdcRead (SDC_RESP1);
    Buffer[2] = MsdcRead (SDC_RESP2);
    Buffer[3] = MsdcRead (SDC_RESP3);
  } else {
    Buffer[0] = MsdcRead (SDC_RESP0);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MmcHostReadBlockData (
  IN  EFI_MMC_HOST_PROTOCOL *This,
  IN  EFI_LBA               Lba,
  IN  UINTN                 Length,
  OUT UINT32                *Buffer
  )
{
  UINT32 Blocks;
  UINT32 Arg;
  EFI_STATUS Status;

  if ((Buffer == NULL) || (Length == 0) || ((Length % 512) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Blocks = (UINT32)(Length / 512);
  Arg = (UINT32)Lba;   // sector addressing: OPPO A3 eMMC > 2 GiB

  if (Blocks == 1) {
    Status = MmcHostSendCommand (This, MMC_CMD17, Arg);
  } else {
    Status = MmcHostSendCommand (This, MMC_CMD18, Arg);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MsdcReadData ((UINT8 *)Buffer, Length);
  if ((Blocks > 1) && !EFI_ERROR (Status)) {
    Status = MmcHostSendCommand (This, MMC_CMD12, 0);
  }
  return Status;
}

STATIC EFI_STATUS
MmcHostWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL *This,
  IN EFI_LBA               Lba,
  IN UINTN                 Length,
  IN UINT32                *Buffer
  )
{
  UINT32 Blocks;
  UINT32 Arg;
  EFI_STATUS Status;

  if ((Buffer == NULL) || (Length == 0) || ((Length % 512) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Blocks = (UINT32)(Length / 512);
  Arg = (UINT32)Lba;

  if (Blocks == 1) {
    Status = MmcHostSendCommand (This, MMC_CMD24, Arg);
  } else {
    Status = MmcHostSendCommand (This, MMC_CMD25, Arg);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MsdcWriteData ((UINT8 *)Buffer, Length);
  if ((Blocks > 1) && !EFI_ERROR (Status)) {
    Status = MmcHostSendCommand (This, MMC_CMD12, 0);
  }
  return Status;
}

STATIC EFI_STATUS
MmcHostBuildDevicePath (
  IN  EFI_MMC_HOST_PROTOCOL    *This,
  OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath
  )
{
  VENDOR_DEVICE_PATH *Node;

  if (DevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Node = CreateDeviceNode (HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof (VENDOR_DEVICE_PATH));
  if (Node == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyGuid (&Node->Guid, &gMt6771MmcDevicePathGuid);
  *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Node;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MmcHostNotifyState (IN EFI_MMC_HOST_PROTOCOL *This, IN MMC_STATE State)
{
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MmcHostSetIos (
  IN EFI_MMC_HOST_PROTOCOL *This,
  IN UINT32                BusClockFreq,
  IN UINT32                BusWidth,
  IN UINT32                TimingMode
  )
{
  if (BusWidth == 8) {
    MsdcSetBusWidth (SDC_CFG_BUS_WIDTH_8BIT);
  } else if (BusWidth == 4) {
    MsdcSetBusWidth (SDC_CFG_BUS_WIDTH_4BIT);
  } else {
    MsdcSetBusWidth (SDC_CFG_BUS_WIDTH_1BIT);
  }
  // Clock and timing are left as configured by LK for first bring-up.
  return EFI_SUCCESS;
}

STATIC BOOLEAN
MmcHostIsCardPresent (IN EFI_MMC_HOST_PROTOCOL *This)
{
  return TRUE;  // eMMC is soldered down
}

STATIC BOOLEAN
MmcHostIsReadOnly (IN EFI_MMC_HOST_PROTOCOL *This)
{
  return FALSE;
}

STATIC BOOLEAN
MmcHostIsMultiBlock (IN EFI_MMC_HOST_PROTOCOL *This)
{
  return TRUE;
}

EFI_MMC_HOST_PROTOCOL gMt6771MmcHost = {
  MMC_HOST_PROTOCOL_REVISION,
  MmcHostIsCardPresent,
  MmcHostIsReadOnly,
  MmcHostBuildDevicePath,
  MmcHostNotifyState,
  MmcHostSendCommand,
  MmcHostReceiveResponse,
  MmcHostReadBlockData,
  MmcHostWriteBlockData,
  MmcHostSetIos,
  MmcHostIsMultiBlock
};

EFI_STATUS
EFIAPI
MmcHostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  mMmcHost.Base = MT6771_MSDC0_BASE;
  mMmcHost.TopBase = MT6771_MSDC0_TOP_BASE;
  mMmcHost.Sclk = MT6771_MSDC_SCLK_HZ;
  mMmcHost.InitializationDone = FALSE;

  MsdcInitController ();

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEmbeddedMmcHostProtocolGuid,
                  &gMt6771MmcHost,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MT6771 MmcHost: install protocol failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "MT6771 MmcHost: eMMC host protocol installed\n"));
  return EFI_SUCCESS;
}
