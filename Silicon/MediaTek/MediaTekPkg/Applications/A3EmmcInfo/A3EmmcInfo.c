/** @file
  UEFI Shell application: query eMMC/BlockIo devices on demand.

  Usage from the Shell:  A3EmmcInfo
  It never runs automatically: the user decides when to print, so there is no
  timer/ConOut storm at TPL_CALLBACK and no reboot loop.
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>

STATIC
VOID
DumpBlockIo (
  IN EFI_HANDLE        Handle,
  IN EFI_BLOCK_IO_PROTOCOL  *Blk
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  CHAR16                    *Text;
  UINT64                    Size;

  Dp = DevicePathFromHandle (Handle);
  Text = (Dp != NULL) ? ConvertDevicePathToText (Dp, TRUE, TRUE) : NULL;

  if (Blk->Media == NULL) {
    Print (L"  BlockIo: NO MEDIA  DP=%s\n", Text != NULL ? Text : L"<none>");
    return;
  }
  Size = Blk->Media->LastBlock > 0
           ? MultU64x32 (Blk->Media->LastBlock + 1, Blk->Media->BlockSize)
           : 0;
  Print (L"  BlockIo: BlockSize=%u LastBlock=%llu Size=%lluMB RO=%a DP=%s\n",
         Blk->Media->BlockSize,
         (UINT64)Blk->Media->LastBlock,
         DivU64x32 (Size, 1024 * 1024),
         Blk->Media->ReadOnly ? "YES" : "NO",
         Text != NULL ? Text : L"<none>");
  if (Text != NULL) FreePool (Text);
}

STATIC
EFI_STATUS
TryGpt (
  IN EFI_BLOCK_IO_PROTOCOL  *Blk
  )
{
  UINT8     *Buf;
  EFI_STATUS Status;
  UINTN     BlockSize;
  UINTN     i;
  CHAR16    Name[9];

  if ((Blk == NULL) || (Blk->Media == NULL)) return EFI_INVALID_PARAMETER;
  BlockSize = Blk->Media->BlockSize;
  if ((BlockSize < 512) || (BlockSize > 4096)) return EFI_UNSUPPORTED;

  Buf = AllocatePool (BlockSize);
  if (Buf == NULL) return EFI_OUT_OF_RESOURCES;

  // GPT header is LBA 1. Check signature "EFI PART".
  Status = Blk->ReadBlocks (Blk, Blk->Media->MediaId, 1, BlockSize, Buf);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return Status;
  }
  if (CompareMem (Buf, "EFI PART", 8) != 0) {
    Print (L"  GPT: no 'EFI PART' signature at LBA1\n");
    FreePool (Buf);
    return EFI_NOT_FOUND;
  }

  // Partition entries start at LBA 2, 128 bytes each; print first 8 names.
  Status = Blk->ReadBlocks (Blk, Blk->Media->MediaId, 2, BlockSize, Buf);
  if (!EFI_ERROR (Status)) {
    Print (L"  GPT: header OK, first partition entries:\n");
    for (i = 0; i + 128 <= BlockSize && i < 8 * 128; i += 128) {
      UINTN k;
      BOOLEAN Any = FALSE;
      for (k = 0; k < 72; k++) {
        if (Buf[i + 56 + k] != 0) { Any = TRUE; break; }
      }
      if (!Any) continue;
      for (k = 0; k < 8; k++) Name[k] = (CHAR16)Buf[i + 56 + k * 2];
      Name[8] = 0;
      Print (L"    %s\n", Name);
    }
  }
  FreePool (Buf);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
A3EmmcInfoMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  *Handles = NULL;
  UINTN        Count = 0;
  UINTN        i;
  EFI_STATUS   Status;

  Print (L"A3EmmcInfo: enumerate BlockIo devices\n");
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Count == 0) {
    Print (L"A3EmmcInfo: no BlockIo devices (%r)\n", Status);
    return EFI_SUCCESS;
  }
  Print (L"A3EmmcInfo: %u BlockIo handle(s)\n", Count);

  for (i = 0; i < Count; i++) {
    EFI_BLOCK_IO_PROTOCOL *Blk = NULL;
    Status = gBS->HandleProtocol (
                    Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&Blk);
    if (EFI_ERROR (Status) || Blk == NULL) continue;
    Print (L"[%u]\n", i);
    DumpBlockIo (Handles[i], Blk);
    TryGpt (Blk);
  }

  if (Handles != NULL) FreePool (Handles);
  return EFI_SUCCESS;
}
