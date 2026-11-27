/** @file
  OPPO A3 USB keyboard driver using EP0 GET_REPORT polling.

  The MT6771 xHCI HID interrupt IN path does not deliver key reports under
  EDK2, but EP0 control transfers work (enumeration succeeds). This driver
  produces EFI_SIMPLE_TEXT_INPUT_PROTOCOL by polling the HID boot report with
  GET_REPORT over EP0 and converting HID usages to EFI keys.
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/TimerLib.h>
#include <Library/PrintLib.h>
#include <Protocol/UsbIo.h>
#include <Guid/ConsoleInDevice.h>
#include <IndustryStandard/Usb.h>

#define A3KB_POLL_PERIOD_US  10000u
#define A3KB_QUEUE_SIZE      32

typedef struct {
  UINTN                           Signature;   // for CR()
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  SimpleIn;
  EFI_EVENT                       Timer;
  EFI_EVENT                       WaitKeyEvent;
  EFI_HANDLE                      Handle;
  EFI_USB_IO_PROTOCOL            *UsbIo;
  EFI_INPUT_KEY                   Queue[A3KB_QUEUE_SIZE];
  UINTN                           Head;
  UINTN                           Tail;
  UINT8                           Last[8];
  BOOLEAN                         FirstReport;
} A3KB_DEV;

STATIC A3KB_DEV  *mKb = NULL;

/* HID usage (page 0x07) -> Unicode, for unshifted keys. 0 = no char. */
STATIC CHAR16
UsageToChar (
  IN UINT8   Usage,
  IN BOOLEAN Shift
  )
{
  CHAR16  c = 0;

  if (Usage >= 0x04 && Usage <= 0x1D) {          /* a..z */
    c = (CHAR16)('a' + (Usage - 0x04));
    if (Shift) c -= 32;
    return c;
  }
  if (Usage >= 0x1E && Usage <= 0x26) {          /* 1..9 */
    c = (CHAR16)('1' + (Usage - 0x1E));
    return c;
  }
  if (Usage == 0x27) return L'0';
  switch (Usage) {
  case 0x28: return 0x000D;   /* Enter */
  case 0x29: return 0x001B;   /* Esc */
  case 0x2A: return 0x0008;   /* Backspace */
  case 0x2B: return 0x0009;   /* Tab */
  case 0x2C: return L' ';
  case 0x2D: return Shift ? L'_' : L'-';
  case 0x2E: return Shift ? L'+' : L'=';
  case 0x2F: return Shift ? L'{' : L'[';
  case 0x30: return Shift ? L'}' : L']';
  case 0x31: return Shift ? L'|' : L'\\';
  case 0x33: return Shift ? L':' : L';';
  case 0x34: return Shift ? L'"' : L'\'';
  case 0x35: return Shift ? L'~' : L'`';
  case 0x36: return Shift ? L'<' : L',';
  case 0x37: return Shift ? L'>' : L'.';
  case 0x38: return Shift ? L'?' : L'/';
  default: return 0;
  }
}

STATIC
VOID
EnqueueKey (
  IN A3KB_DEV      *Kb,
  IN EFI_INPUT_KEY  Key
  )
{
  UINTN  Next;

  Next = (Kb->Tail + 1) % A3KB_QUEUE_SIZE;
  if (Next == Kb->Head) {
    return;   /* full */
  }
  Kb->Queue[Kb->Tail] = Key;
  Kb->Tail = Next;
  gBS->SignalEvent (Kb->WaitKeyEvent);
}

STATIC
VOID
ProcessReport (
  IN A3KB_DEV  *Kb,
  IN UINT8     *Report
  )
{
  UINTN     i;
  BOOLEAN   Shift;
  UINT8     OldUsage;
  EFI_INPUT_KEY  Key;

  if (!Kb->FirstReport) {
    /* first report is baseline; ignore so we don't replay held keys */
    CopyMem (Kb->Last, Report, 8);
    Kb->FirstReport = TRUE;
    return;
  }

  Shift = (Report[0] & (BIT1 | BIT2)) != 0;

  for (i = 2; i < 8; i++) {
    if (Report[i] == 0) continue;
    OldUsage = 0;
    {
      UINTN j;
      for (j = 2; j < 8; j++) {
        if (Kb->Last[j] == Report[i]) { OldUsage = Report[i]; break; }
      }
    }
    if (OldUsage != 0) continue;   /* key was already down */

    Key.ScanCode    = 0;
    Key.UnicodeChar = UsageToChar (Report[i], Shift);
    if (Key.UnicodeChar != 0) {
      EnqueueKey (Kb, Key);
    }
  }
  CopyMem (Kb->Last, Report, 8);
}

STATIC
VOID
EFIAPI
A3KbPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A3KB_DEV               *Kb = (A3KB_DEV *)Context;
  EFI_USB_DEVICE_REQUEST  Request;
  UINT8                   Report[8];
  UINT32                  UsbStatus;

  if ((Kb == NULL) || (Kb->UsbIo == NULL)) return;

  ZeroMem (&Request, sizeof (Request));
  Request.RequestType = 0xA1;
  Request.Request     = 0x01;
  Request.Value       = 0x0100;
  Request.Index       = 0;
  Request.Length      = 8;

  ZeroMem (Report, sizeof (Report));
  if (!EFI_ERROR (Kb->UsbIo->UsbControlTransfer (
                    Kb->UsbIo, &Request, EfiUsbDataIn, 100,
                    Report, 8, &UsbStatus))) {
    ProcessReport (Kb, Report);
  }
}

STATIC
EFI_STATUS
EFIAPI
A3KbReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  A3KB_DEV *Kb = CR (This, A3KB_DEV, SimpleIn, 0);
  Kb->Head = Kb->Tail = 0;
  Kb->FirstReport = FALSE;
  ZeroMem (Kb->Last, 8);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
A3KbReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  A3KB_DEV *Kb = CR (This, A3KB_DEV, SimpleIn, 0);

  if (Key == NULL) return EFI_INVALID_PARAMETER;
  if (Kb->Head == Kb->Tail) return EFI_NOT_READY;
  *Key = Kb->Queue[Kb->Head];
  Kb->Head = (Kb->Head + 1) % A3KB_QUEUE_SIZE;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
A3KbWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  /* ConSplitter owns the WaitForKey event; nothing to do here. */
}

EFI_STATUS
EFIAPI
A3UsbKbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN        Count = 0;
  UINTN        i;
  CHAR16       Buf[128];

  mKb = AllocateZeroPool (sizeof (A3KB_DEV));
  if (mKb == NULL) return EFI_OUT_OF_RESOURCES;
  mKb->Signature = 0x41334B42u;   // "A3KB"

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Count == 0) {
    UnicodeSPrint (Buf, sizeof (Buf), L"A3Kb: no UsbIo (%r)\r\n", Status);
    gST->ConOut->OutputString (gST->ConOut, Buf);
    return EFI_SUCCESS;
  }

  /* find the HID keyboard (interface class 3/1/1) */
  for (i = 0; i < Count; i++) {
    EFI_USB_IO_PROTOCOL            *UsbIo = NULL;
    EFI_USB_INTERFACE_DESCRIPTOR    IfDesc;
    if (EFI_ERROR (gBS->HandleProtocol (
                          Handles[i], &gEfiUsbIoProtocolGuid, (VOID **)&UsbIo))) {
      continue;
    }
    if (EFI_ERROR (UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc))) {
      continue;
    }
    if ((IfDesc.InterfaceClass == 3) &&
        (IfDesc.InterfaceSubClass == 1) &&
        (IfDesc.InterfaceProtocol == 1)) {
      mKb->UsbIo = UsbIo;
      break;
    }
  }

  if (mKb->UsbIo == NULL) {
    gST->ConOut->OutputString (gST->ConOut, L"A3Kb: no HID keyboard found\r\n");
    if (Handles != NULL) gBS->FreePool (Handles);
    return EFI_SUCCESS;
  }
  if (Handles != NULL) gBS->FreePool (Handles);

  mKb->SimpleIn.Reset         = A3KbReset;
  mKb->SimpleIn.ReadKeyStroke = A3KbReadKeyStroke;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                  A3KbWaitForKey, mKb, &mKb->WaitKeyEvent);
  if (EFI_ERROR (Status)) return EFI_DEVICE_ERROR;
  mKb->SimpleIn.WaitForKey = mKb->WaitKeyEvent;

  //
  // Install ConsoleInDeviceGuid too: ConSplitter binds that GUID and merges
  // the device into gST->ConIn, the same way it does for UsbKbDxe's handle.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mKb->Handle,
                  &gEfiSimpleTextInProtocolGuid,  &mKb->SimpleIn,
                  &gEfiConsoleInDeviceGuid,       NULL,
                  NULL);
  if (EFI_ERROR (Status)) {
    gST->ConOut->OutputString (gST->ConOut, L"A3Kb: install SimpleTextIn failed\r\n");
    return EFI_DEVICE_ERROR;
  }

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                  A3KbPoll, mKb, &mKb->Timer);
  if (EFI_ERROR (Status)) return EFI_DEVICE_ERROR;
  gBS->SetTimer (mKb->Timer, TimerPeriodic, A3KB_POLL_PERIOD_US);

  gST->ConOut->OutputString (gST->ConOut, L"A3Kb: EP0 keyboard active\r\n");
  return EFI_SUCCESS;
}
