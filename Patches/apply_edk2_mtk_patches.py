#!/usr/bin/env python3
"""
Idempotently patch the pinned EDK2 submodule for MT6771 quirks.

The submodule is a pinned gitlink (e7aac7fc...), so we cannot commit changes to
it. build.sh runs this script before each build; each patch is guarded so it
applies exactly once.

Patch: XhciSched.c -- write the MTK periodic-endpoint scheduling hint
(endpoint context reserved[0] = EP_BPKTS(1)) for INT endpoints in both the
32-byte and 64-byte context paths. Linux's xhci-mtk-sch.c does this for MTK
SSUSB controllers; EDK2 leaves the field zero, so interrupt IN (HID keyboard)
never gets a periodic service window while EP0/enumeration still work.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
SCHED = ROOT / "Common/edk2/MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c"
MARK = "MTK_EP_BPKTS"
SCHED_DIAG_MARK = "A3_INT_DIAG"

BLOCK = """        //
        // MT6771 (MediaTek SSUSB) xHCI needs periodic-endpoint scheduling
        // hints in the endpoint context reserved DW. Linux xhci-mtk-sch.c
        // writes ep_ctx->reserved[0] = EP_BPKTS(1) for a directly attached
        // FS/LS interrupt endpoint; EDK2 leaves RsvdZ5 zero. Without it the
        // controller has no periodic service window and HID interrupt IN
        // never returns data (control/EP0 and enumeration still work).
        //
        InputContext->EP[Dci-1].RsvdZ5 = 1;   // MTK_EP_BPKTS(1)

"""




def patch_int_diag(text: str, nl: str) -> tuple[str, bool]:
    """Inject a once-per-report interrupt-transfer diagnostic."""
    if SCHED_DIAG_MARK in text:
        return text, False
    anchor = (
        "    if (Urb->Callback != NULL) {" + nl +
        "      //" + nl +
        "      // Restore the old TPL, USB bus maybe connect device in" + nl
    )
    if anchor not in text:
        print("WARNING: interrupt diagnostic anchor not found", file=sys.stderr)
        return text, False
    lines = [
        "    // A3_INT_DIAG: report any non-trivial HID interrupt IN report",
        "    if ((Urb->Completed > 4) && (Urb->Data != NULL)) {",
        "      UINT8 *A3Data = (UINT8 *)Urb->Data;",
        "      DEBUG ((DEBUG_ERROR,",
        "        \"A3_INT_DIAG: len=%u res=0x%x d0=%02x d1=%02x d2=%02x\\n\",",
        "        Urb->Completed, Urb->Result, A3Data[0], A3Data[1], A3Data[2]));",
        "    }",
    ]
    block = nl.join(lines) + nl + anchor
    return text.replace(anchor, block, 1), True


def main() -> int:
    if not SCHED.is_file():
        print("SKIP: XhciSched.c not present (submodule not checked out?)", file=sys.stderr)
        return 0

    raw = SCHED.read_bytes()
    text = raw.decode("utf-8")

    nl = "\r\n" if "\r\n" in text else "\n"
    block = BLOCK.replace("\n", nl)
    # Insert immediately BEFORE the `break;` that follows the INT-ring creation
    # block. Using the `case USB_ENDPOINT_CONTROL:` label makes the anchor unique
    # to the two endpoint-context initializers and keeps it clear of the DEBUG((
    # macro above, whose closing `));` has the same indentation.
    anchor = ("        break;" + nl + nl +
              "      case USB_ENDPOINT_CONTROL:" + nl)

    count = text.count(anchor)
    if count == 0 and MARK not in text:
        print("ERROR: XhciSched.c anchor not found; patch NOT applied", file=sys.stderr)
        return 1

    if MARK not in text:
        updated = text.replace(anchor, block + anchor)
        SCHED.write_bytes(updated.encode("utf-8"))
        print(f"Applied MTK periodic-schedule patch to {count} endpoint init site(s)")
    else:
        updated = text
        print("MTK periodic-schedule patch already applied")

    updated, diag_applied = patch_int_diag(updated, nl)
    if diag_applied:
        SCHED.write_bytes(updated.encode("utf-8"))
        print("Applied A3 interrupt-transfer diagnostic patch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
