/** @file

  OPPO A3 (MT6771) eMMC diagnostics.

  The MSDC/eMMC stack produces a BlockIo handle (the shell shows it as BLK0 with
  the driver's vendor device path). There is no FAT partition on the phone's
  eMMC, so the shell cannot mount it. This driver instead reads the GPT header
  and partition entries straight from the block device and prints them, which
  proves reads work and reveals the partition layout.

  Output is printed on a timer a few times because the shell clears the screen
  when it starts.

  Copyright (c) 2026, kesry. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define DIAG_PERIOD   50000000u   // 5 s, after Shell clears the screen


//
// Screen output is disabled on purpose: only USB diagnostics should occupy the
// screen while the USB/OTG bring-up is being debugged. Set to TRUE to re-enable
// the eMMC/GPT report.
//
#define MMC_DIAG_SCREEN_TRACE  TRUE

STATIC
VOID
PrintText (IN CONST CHAR16 *Text)
{
  if (!MMC_DIAG_SCREEN_TRACE) {
    return;
  }
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
  }
}

STATIC
EFI_STATUS
ReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlkIo,
  IN  UINT64                Lba,
  IN  UINTN                 Bytes,
  OUT VOID                  *Buffer
  )
{
  return BlkIo->ReadBlocks (
                  BlkIo,
                  BlkIo->Media->MediaId,
                  Lba,
                  Bytes,
                  Buffer
                  );
}

STATIC
VOID
DumpGpt (
  IN EFI_BLOCK_IO_PROTOCOL  *BlkIo,
  IN UINT8                  *Buf,          // at least one block
  IN UINTN                  BlockSize
  )
{
  CHAR16  Line[192];
  UINT64  EntryLba;
  UINT32  NumEntries;
  UINT32  EntrySize;
  UINTN   i;
  UINT8   *Entries;
  UINTN   EntriesBytes;
  UINTN   Printed;
  EFI_STATUS  Status;

  Status = ReadBlocks (BlkIo, 1, BlockSize, Buf);
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"eMMC: read LBA1 failed %r\r\n", Status);
    PrintText (Line);
    return;
  }

  if (CompareMem (Buf, "EFI PART", 8) != 0) {
    PrintText (L"eMMC: LBA1 is not a GPT header\r\n");
    return;
  }

  EntryLba   = *(UINT64 *)(Buf + 0x48);
  NumEntries = *(UINT32 *)(Buf + 0x50);
  EntrySize  = *(UINT32 *)(Buf + 0x54);

  UnicodeSPrint (
    Line, sizeof (Line),
    L"eMMC GPT: entries@LBA%Lu count=%u size=%u\r\n",
    EntryLba, NumEntries, EntrySize
    );
  PrintText (Line);

  if ((EntrySize < 128) || (NumEntries == 0) || (NumEntries > 128)) {
    PrintText (L"eMMC: unexpected GPT entry geometry\r\n");
    return;
  }

  EntriesBytes = (UINTN)NumEntries * EntrySize;
  Entries = AllocatePool (EntriesBytes);
  if (Entries == NULL) {
    return;
  }

  Status = ReadBlocks (
             BlkIo,
             EntryLba,
             (EntriesBytes + BlockSize - 1) / BlockSize * BlockSize,
             Entries
             );
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"eMMC: read entries failed %r\r\n", Status);
    PrintText (Line);
    FreePool (Entries);
    return;
  }

  Printed = 0;
  for (i = 0; i < NumEntries && Printed < 48; i++) {
    UINT8   *E = Entries + i * EntrySize;
    UINT64  First;
    UINT64  Last;
    BOOLEAN Used = FALSE;
    UINTN   k;

    for (k = 0; k < 16; k++) {
      if (E[k] != 0) {
        Used = TRUE;
        break;
      }
    }
    if (!Used) {
      continue;
    }

    First = *(UINT64 *)(E + 32);
    Last  = *(UINT64 *)(E + 40);

    {
      CHAR8  Name[17];
      UINTN  n;
      for (n = 0; n < 16; n++) {
        CHAR16 c = *(CHAR16 *)(E + 56 + n * 2);
        Name[n] = (c == 0) ? 0 : ((c < 0x80) ? (CHAR8)c : (CHAR8)'?');
        if (c == 0) {
          break;
        }
      }
      Name[16] = 0;

      UnicodeSPrint (
        Line, sizeof (Line),
        L"  #%u %a %08x-%04x-%04x first=%Lu last=%Lu (%Lu MB)\r\n",
        i,
        Name,
        *(UINT32 *)E,
        *(UINT16 *)(E + 4),
        *(UINT16 *)(E + 6),
        First,
        Last,
        (Last - First + 1) * BlockSize / (1024 * 1024)
        );
      PrintText (Line);
    }
    Printed++;
  }

  FreePool (Entries);
}

STATIC
VOID
Diagnose (
  VOID
  );

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->SetTimer (Event, TimerCancel, 0);
  Diagnose ();
}

STATIC
VOID
Diagnose (
  VOID
  )
{
  CHAR16               Line[192];
  EFI_STATUS           Status;
  EFI_HANDLE           *Handles;
  UINTN                Count;
  UINTN                i;
  EFI_BLOCK_IO_PROTOCOL *BlkIo;
  UINT8                *Buf;

  PrintText (L"\r\n==== A3 eMMC diagnostics ====\r\n");

  Handles = NULL;
  Count   = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"BlockIo lookup failed: %r\r\n", Status);
    PrintText (Line);
    return;
  }

  UnicodeSPrint (Line, sizeof (Line), L"BlockIo handles: %u\r\n", Count);
  PrintText (Line);

  for (i = 0; i < Count; i++) {
    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlkIo);
    if (EFI_ERROR (Status) || (BlkIo->Media == NULL)) {
      continue;
    }

    UnicodeSPrint (
      Line, sizeof (Line),
      L"  [%u] blk=%u last=%Lu present=%u ro=%u rem=%u id=%u\r\n",
      i,
      BlkIo->Media->BlockSize,
      BlkIo->Media->LastBlock,
      BlkIo->Media->MediaPresent,
      BlkIo->Media->ReadOnly,
      BlkIo->Media->RemovableMedia,
      BlkIo->Media->MediaId
      );
    PrintText (Line);

    //
    // Force one block read. MmcDxe installs BlockIo first and initialises the
    // card on first access, so LastBlock is still 0 at report time; this read
    // triggers the initialisation and lets us re-check the media and look at
    // the real sector content (a GPT disk has a protective MBR: zeros up to
    // 445, a partition entry at 446..461 and 55 AA at 510..511).
    //
    if ((BlkIo->Media->BlockSize >= 512) && (BlkIo->Media->BlockSize <= 4096)) {
      UINT8       *Sec;
      EFI_STATUS   RdStatus;
      Sec = AllocatePool (BlkIo->Media->BlockSize);
      if (Sec != NULL) {
        RdStatus = BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId, 0,
                                      BlkIo->Media->BlockSize, Sec);
        UnicodeSPrint (
          Line, sizeof (Line),
          L"      read LBA0 %r  last=%Lu\r\n",
          RdStatus, BlkIo->Media->LastBlock
          );
        PrintText (Line);
        if (!EFI_ERROR (RdStatus)) {
          UINTN k;
          UnicodeSPrint (Line, sizeof (Line), L"      head:");
          PrintText (Line);
          for (k = 0; k < 16; k++) {
            UnicodeSPrint (Line, sizeof (Line), L" %02x", Sec[k]);
            PrintText (Line);
          }
          PrintText (L"\r\n      mbr :");
          for (k = 446; k < 462; k++) {
            UnicodeSPrint (Line, sizeof (Line), L" %02x", Sec[k]);
            PrintText (Line);
          }
          UnicodeSPrint (Line, sizeof (Line), L"  sig=%02x%02x\r\n",
                         Sec[510], Sec[511]);
          PrintText (Line);
        }
        FreePool (Sec);
      }
    }

    {
      EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&Dp)) && (Dp != NULL)) {
        EFI_DEVICE_PATH_PROTOCOL  *Node = Dp;
        UINTN                      Nodes = 0;
        while (!IsDevicePathEnd (Node) && Nodes < 6) {
          if ((DevicePathType (Node) == HARDWARE_DEVICE_PATH) &&
              (DevicePathSubType (Node) == HW_VENDOR_DP)) {
            VENDOR_DEVICE_PATH  *V = (VENDOR_DEVICE_PATH *)Node;
            UnicodeSPrint (
              Line, sizeof (Line),
              L"      venhw %08x-%04x-%04x\r\n",
              V->Guid.Data1, V->Guid.Data2, V->Guid.Data3
              );
            PrintText (Line);
          } else {
            UnicodeSPrint (
              Line, sizeof (Line),
              L"      node type=%u sub=%u\r\n",
              DevicePathType (Node), DevicePathSubType (Node)
              );
            PrintText (Line);
          }
          Node = NextDevicePathNode (Node);
          Nodes++;
        }
      }
    }

    if ((BlkIo->Media->BlockSize == 0) || (BlkIo->Media->LastBlock < 2)) {
      continue;
    }

    Buf = AllocatePool (BlkIo->Media->BlockSize);
    if (Buf == NULL) {
      continue;
    }
    DumpGpt (BlkIo, Buf, BlkIo->Media->BlockSize);
    FreePool (Buf);
  }

  FreePool (Handles);
  PrintText (L"==== end eMMC diagnostics ====\r\n");
}


EFI_STATUS
EFIAPI
Mt6771MmcDiagDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  TimerStatus;
  EFI_EVENT   TimerEvent;

  //
  // ONE-SHOT at 5 s so the report survives the Shell screen clear. A periodic
  // timer rebooted the board earlier; this must stay single-shot.
  //
  TimerEvent = NULL;
  TimerStatus = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnTimer,
                  NULL,
                  &TimerEvent
                  );
  if (!EFI_ERROR (TimerStatus)) {
    gBS->SetTimer (TimerEvent, TimerRelative, DIAG_PERIOD);
  }

  return EFI_SUCCESS;
}
