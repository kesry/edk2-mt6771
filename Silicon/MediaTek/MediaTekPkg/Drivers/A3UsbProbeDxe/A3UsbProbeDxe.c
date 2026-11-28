/** @file
  OPPO A3 USB keyboard probe: read HID boot reports over EP0 (GET_REPORT).

  This is a bring-up diagnostic and fallback input path. It prints the 8-byte
  boot keyboard report obtained with a control transfer, which lets us tell
  whether the keyboard responds on EP0 even if the HID interrupt IN endpoint
  never produces data.
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
#include <IndustryStandard/Usb.h>

//
// Keep probing for 15 seconds so the keyboard has time to appear after the
// charger-boost VBUS path comes up. A periodic callback is used instead of a
// blocking loop in the driver entry point.
//
#define A3_PROBE_TICKS     8
#define A3_PROBE_PERIOD_US 250000u   // 250 ms

STATIC EFI_EVENT  mProbeEvent;
STATIC UINTN      mProbeTicks;
STATIC BOOLEAN    mDescPrinted;
STATIC UINT8      mLastReport[8];
STATIC BOOLEAN    mHaveLast;

STATIC
VOID
PrintLine (
  IN CONST CHAR16  *Text
  )
{
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
  }
}

/**
  Issue HID GET_REPORT (Input report, report ID 0) and print the 8-byte boot
  report. bmRequestType 0xA1 = class/interface/device-to-host.
**/
STATIC
VOID
ProbeOneUsbIo (
  IN EFI_USB_IO_PROTOCOL  *UsbIo
  )
{
  EFI_USB_DEVICE_REQUEST  Request;
  UINT8                   Report[8];
  UINT32                  UsbStatus;
  EFI_STATUS              Status;
  EFI_USB_DEVICE_DESCRIPTOR  DevDesc;
  CHAR16                  Buf[160];

  ZeroMem (&DevDesc, sizeof (DevDesc));
  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc);
  if (EFI_ERROR (Status)) {
    PrintLine (L"A3Probe: UsbGetDeviceDescriptor failed\r\n");
    return;
  }

  if (!mDescPrinted) {
    mDescPrinted = TRUE;
    UnicodeSPrint (Buf, sizeof (Buf),
      L"A3Probe: VID=%04x PID=%04x Class=%02x Sub=%02x Proto=%02x\r\n",
      DevDesc.IdVendor, DevDesc.IdProduct,
      DevDesc.DeviceClass, DevDesc.DeviceSubClass, DevDesc.DeviceProtocol);
    PrintLine (Buf);
  }

  //
  // UsbKbDxe already sent SET_PROTOCOL(BOOT) during its Start(), so a plain
  // GET_REPORT here should return the 8-byte boot report.
  //
  ZeroMem (&Request, sizeof (Request));
  Request.RequestType = 0xA1;   // class, interface, IN
  Request.Request     = 0x01;   // HID GET_REPORT
  Request.Value       = 0x0100; // Input report, ReportID 0
  Request.Index       = 0;      // interface 0
  Request.Length      = sizeof (Report);

  ZeroMem (Report, sizeof (Report));
  Status = UsbIo->UsbControlTransfer (
                    UsbIo,
                    &Request,
                    EfiUsbDataIn,
                    1000,
                    Report,
                    sizeof (Report),
                    &UsbStatus
                    );
  if (mHaveLast && (CompareMem (mLastReport, Report, sizeof (Report)) == 0)) {
    return;
  }
  CopyMem (mLastReport, Report, sizeof (Report));
  mHaveLast = TRUE;
  UnicodeSPrint (Buf, sizeof (Buf),
    L"A3Probe: GET_REPORT %r %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
    Status,
    Report[0], Report[1], Report[2], Report[3],
    Report[4], Report[5], Report[6], Report[7]);
  PrintLine (Buf);
}

STATIC
VOID
EFIAPI
A3UsbProbeTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN        Count = 0;
  UINTN        i;

  mProbeTicks++;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiUsbIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (!EFI_ERROR (Status) && (Count > 0)) {
    for (i = 0; i < Count; i++) {
      EFI_USB_IO_PROTOCOL  *UsbIo = NULL;
      Status = gBS->HandleProtocol (
                      Handles[i],
                      &gEfiUsbIoProtocolGuid,
                      (VOID **)&UsbIo
                      );
      if (!EFI_ERROR (Status) && (UsbIo != NULL)) {
        ProbeOneUsbIo (UsbIo);
      }
    }
  }

  if (Handles != NULL) {
    gBS->FreePool (Handles);
  }

  if (mProbeTicks >= A3_PROBE_TICKS) {
    gBS->SetTimer (mProbeEvent, TimerCancel, 0);
    PrintLine (L"A3Probe: done\r\n");
  }
}

EFI_STATUS
EFIAPI
A3UsbProbeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  PrintLine (L"A3Probe: start (15s polling)\r\n");

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A3UsbProbeTimer,
                  NULL,
                  &mProbeEvent
                  );
  if (EFI_ERROR (Status)) {
    PrintLine (L"A3Probe: CreateEvent failed\r\n");
    return EFI_DEVICE_ERROR;
  }

  Status = gBS->SetTimer (
                  mProbeEvent,
                  TimerPeriodic,
                  A3_PROBE_PERIOD_US
                  );
  if (EFI_ERROR (Status)) {
    PrintLine (L"A3Probe: SetTimer failed\r\n");
    gBS->CloseEvent (mProbeEvent);
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}
