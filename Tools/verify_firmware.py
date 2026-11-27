#!/usr/bin/env python3
"""Build-time verification for the OPPO A3 (MT6771) UEFI port.

Run from CI (or locally) after the EDK2 build:

    python3 Tools/verify_firmware.py Build/boot-uefi.img Build/A3/FV/A3_UEFI.fd

It checks the things that are easy to break silently and expensive to discover
on hardware:

  1. boot.img layout            (32 MiB, ANDROID! v2, page 2048, AVB present)
  2. the fake arm64 Image header (branch code0, stock text_offset/image_size)
  3. the MTK appended-DTB header (magic + sizes)
  4. platform PCD values against the documented hardware facts
  5. that every driver this port needs is present in the firmware volume

Exit code 0 = all checks passed.  Non-zero = at least one failed; the failing
check is printed with the expected and actual value.

Nothing here needs hardware, but a pass only means "the image is well formed",
not "it boots".
"""

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

FAILS = []
CHECKS = 0


def check(condition, label, detail=""):
    global CHECKS
    CHECKS += 1
    if condition:
        print(f"  OK   {label}")
    else:
        print(f"  FAIL {label}{(' :: ' + detail) if detail else ''}")
        FAILS.append(label)


def check_eq(actual, expected, label):
    check(actual == expected, label, f"expected {expected!r}, got {actual!r}")


# --------------------------------------------------------------------------
# boot.img
# --------------------------------------------------------------------------
PAGE_SIZE = 2048
IMAGE_SIZE = 32 * 1024 * 1024
MTK_DTB_MAGIC = 0xD7B7AB1E
ARM64_MAGIC = b"ARMd"

# Values measured from the stock Android 10 boot.img (see docs/porting.md).
STOCK_TEXT_OFFSET = 0x80000
STOCK_IMAGE_SIZE = 0x257F000


def verify_boot_image(path):
    print(f"[boot.img] {path}")
    data = path.read_bytes()

    check_eq(len(data), IMAGE_SIZE, "image size is 32 MiB (matches boot partition)")
    check_eq(data[:8], b"ANDROID!", "boot header magic")

    page = struct.unpack_from("<I", data, 0x24)[0]
    header_version = struct.unpack_from("<I", data, 0x28)[0]
    kernel_size = struct.unpack_from("<I", data, 0x08)[0]
    kernel_addr = struct.unpack_from("<I", data, 0x0C)[0]
    ramdisk_size = struct.unpack_from("<I", data, 0x10)[0]
    ramdisk_addr = struct.unpack_from("<I", data, 0x14)[0]
    tags_addr = struct.unpack_from("<I", data, 0x20)[0]
    dtb_blob_size = struct.unpack_from("<I", data, 0x670)[0]

    check_eq(page, PAGE_SIZE, "page size")
    check_eq(header_version, 2, "header version")
    check_eq(kernel_addr, 0x40080000, "kernel address")
    check_eq(ramdisk_addr, 0x55000000, "ramdisk address")
    check_eq(tags_addr, 0x54000000, "tags address")

    def align(n):
        return (n + page - 1) // page * page

    kernel_off = page
    kernel = data[kernel_off:kernel_off + kernel_size]
    check(kernel[:2] == b"\x1f\x8b", "kernel segment is gzip",
          f"got {kernel[:2].hex()}")

    # MTK header is appended to the kernel region in this port.
    dtb_blob = kernel[-dtb_blob_size:] if dtb_blob_size else b""
    check(len(dtb_blob) == dtb_blob_size, "appended DTB blob present")

    # standalone DTB behind the ramdisk
    ramdisk_off = kernel_off + align(kernel_size)
    dtb_off = ramdisk_off + align(ramdisk_size)
    mtk_hdr = data[dtb_off:dtb_off + 64]
    magic = struct.unpack_from(">I", mtk_hdr, 0)[0] if len(mtk_hdr) >= 4 else 0
    check_eq(hex(magic), hex(MTK_DTB_MAGIC), "standalone MTK DTB header magic")

    dtb_len_field = struct.unpack_from(">I", mtk_hdr, 0x20)[0] if len(mtk_hdr) >= 0x24 else 0
    check(dtb_len_field > 0, "standalone MTK DTB header declares a DTB length",
          f"got {dtb_len_field}")
    fdt_magic = struct.unpack_from(">I", data, dtb_off + 64)[0] if len(data) > dtb_off + 68 else 0
    check_eq(hex(fdt_magic), hex(0xD00DFEED), "standalone DTB is an FDT")

    check(b"AVB0" in data, "AVB vbmeta block present")
    check(b"AVBf" in data, "AVB footer present")

    # ---- decompress the kernel segment and inspect the fake Image header ----
    raw = None
    payload = kernel[:len(kernel) - dtb_blob_size] if dtb_blob_size else kernel
    # strip a possibly named gzip header, then inflate
    if payload[:2] == b"\x1f\x8b":
        flg = payload[3]
        p = 10
        if flg & 0x04:
            p += 2 + struct.unpack_from("<H", payload, p)[0]
        if flg & 0x08:
            p = payload.index(0, p) + 1
        if flg & 0x10:
            p = payload.index(0, p) + 1
        if flg & 0x02:
            p += 2
        try:
            d = zlib.decompressobj(-15)
            raw = d.decompress(payload[p:])
        except zlib.error as exc:
            raw = None
            print(f"       (gzip inflate error: {exc})")

    check(raw is not None, "kernel segment inflates")
    if raw is None:
        return

    code0 = struct.unpack_from("<I", raw, 0x00)[0]
    text_offset = struct.unpack_from("<Q", raw, 0x08)[0]
    image_size = struct.unpack_from("<Q", raw, 0x10)[0]

    check((code0 & 0xFC000000) == 0x14000000,
          "code0 is a branch instruction (OPPO A3 LK requirement)",
          f"code0=0x{code0:08x}")
    check_eq(raw[0x38:0x3C], ARM64_MAGIC, "arm64 magic at offset 0x38")
    check_eq(hex(text_offset), hex(STOCK_TEXT_OFFSET), "text_offset matches stock")
    check_eq(hex(image_size), hex(STOCK_IMAGE_SIZE), "image_size matches stock")
    check(image_size >= len(raw), "declared image_size covers the payload",
          f"image_size=0x{image_size:x} payload=0x{len(raw):x}")


# --------------------------------------------------------------------------
# platform configuration
# --------------------------------------------------------------------------
# Values that must match the hardware facts recorded in docs/porting.md.
EXPECTED_PCDS = {
    "PcdMipiFrameBufferAddress": "0x77440000",
    "PcdMipiFrameBufferWidth": "1088",
    "PcdMipiFrameBufferHeight": "2280",
    "PcdMsdcMmioRegBase": "0x11230000",
    "PcdMsdcTopMmioRegBase": "0x11f50000",
}


PLATFORM_FILES = [
    "Platform/OPPO/A3Pkg/a3.dsc",
    "Platform/OPPO/A3Pkg/a3-acm.dsc",
    "Platform/OPPO/A3Pkg/a3.fdf",
    "Platform/OPPO/A3Pkg/a3-acm.fdf",
    "Silicon/MediaTek/MediaTekPkg/MediaTekPkg.dec",
    "Silicon/MediaTek/MediaTekPkg/MediaTekCommonDsc.inc",
    "Silicon/MediaTek/MT8183Pkg/MT8183.dsc",
]


def verify_platform(root, usb_mode=None):
    print("[platform] a3.dsc / a3.fdf / MediaTekPkg.dec")

    # PCDs may be declared in the DEC (with a default) and only overridden in
    # the DSC, so search the DSC first and fall back to the DEC.
    dsc_text = (root / "Platform/OPPO/A3Pkg/a3.dsc").read_text(errors="replace")
    acm_dsc = (root / "Platform/OPPO/A3Pkg/a3-acm.dsc")
    dsc_all_text = dsc_text + (acm_dsc.read_text(errors="replace") if acm_dsc.is_file() else "")
    dec_text = (root / "Silicon/MediaTek/MediaTekPkg/MediaTekPkg.dec").read_text(errors="replace")

    def lookup_pcd(name):
        for text in (dsc_all_text, dec_text):
            m = re.search(rf"{re.escape(name)}\|([^\s|]+)", text)
            if m:
                return m.group(1)
        return None

    for name, expected in EXPECTED_PCDS.items():
        actual = lookup_pcd(name)
        if actual is None:
            check(False, f"{name} is declared", "not found in a3.dsc or MediaTekPkg.dec")
            continue
        try:
            same = int(actual, 0) == int(expected, 0)
        except ValueError:
            same = actual == expected
        check(same, f"{name} == {expected}", f"got {actual}")

    use_top = lookup_pcd("PcdMsdcUseTop")
    check(use_top is not None and use_top.upper() == "TRUE",
          "PcdMsdcUseTop is TRUE (msdc0 top block exists)", f"got {use_top}")

    # Everything the platform needs must be reachable from the DSC (including
    # the shared includes) and actually placed in the FV.
    dsc_all = "\n".join(
        (root / f).read_text(errors="replace")
        for f in PLATFORM_FILES[::2] + PLATFORM_FILES[1::2]
    )
    fdf_name = "a3-acm.fdf" if usb_mode == "ACM" else "a3.fdf"
    fdf = (root / "Platform/OPPO/A3Pkg" / fdf_name).read_text(errors="replace")
    common = ["Mt6771MmcHostDxe", "Mt6771VbusDxe", "Mt6771MmcDiagDxe"]
    if usb_mode == "ACM":
        # device mode: the host stack must not be present
        usb_specific = ["Mt6771UsbAcmDxe"]
    else:
        usb_specific = ["Mt6771UsbHostDxe", "XhciDxe",
                        "NonDiscoverablePciDeviceDxe", "UsbKbDxe"]

    for driver in common + usb_specific:
        check(driver in dsc_all, f"{driver} listed in the platform DSC set")
        check(driver in fdf, f"{driver} placed in a3.fdf")

    # startup.nsh must be embedded or the board cannot run diagnostics
    check("startup.nsh" in fdf, "startup.nsh embedded in the firmware volume (" + fdf_name + ")")
    check((root / "Platform/OPPO/A3Pkg/startup.nsh").is_file(),
          "Platform/OPPO/A3Pkg/startup.nsh exists")


def verify_last_commit_helper(root):
    """The platfom build stamps git describe; make sure the stamp code exists."""
    print("[repo] build inputs")
    py = root / "Platform/OPPO/A3Pkg/PlatformBuild.py"
    text = py.read_text(errors="replace")
    check("boot-uefi-" in text, "build produces Build/boot-uefi-<mode>.img")
    check("mkbootimg-a3.py" in text, "boot image uses Tools/mkbootimg-a3.py")
    check((root / "Tools/mkbootimg-a3.py").is_file(), "Tools/mkbootimg-a3.py exists")


# --------------------------------------------------------------------------
# porting invariants: values that must come from the vendor/upstream sources
# --------------------------------------------------------------------------
# (name, value, why) -- each must still be defined with that value so a future
# edit cannot silently replace a sourced value with a guess.
USB_HOST_INVARIANTS = [
    ("MT6771_IPPC_BASE", "0x11203E00", "IPPC base from the stock DTB usb3 reg-names"),
    ("U3D_SSUSB_U3_CTRL_0P", "0x0030", "U3 port0 control offset (vendor ippc header)"),
    ("U3D_SSUSB_U2_CTRL_0P", "0x0050", "U2 port0 control offset (vendor ippc header)"),
    ("U3D_SSUSB_IP_PW_STS1", "0x0010", "IP clock status 1 offset (vendor ippc header)"),
    ("U3D_SSUSB_IP_PW_STS2", "0x0014", "IP clock status 2 offset (vendor ippc header)"),
    ("SSUSB_U2_PORT_OTG_SEL", "BIT7", "vendor OTG_SEL is bit7 (upstream differs)"),
    ("SSUSB_U2_PORT_HOST_SEL", "BIT2", "vendor U2 HOST_SEL is bit2 (upstream uses VBUSVALID|HOST)"),
    ("SSUSB_SYS125_RST_B_STS", "BIT10", "vendor sys125 clock status bit"),
    ("SSUSB_U3_MAC_RST_B_STS", "BIT16", "vendor U3 MAC clock status bit"),
    ("SSUSB_U2_MAC_SYS_RST_B_STS", "BIT0", "vendor U2 MAC clock status bit"),
    ("PHY_FORCE_IDDIG", "BIT9", "vendor forces IDDIG in PHY DTM1 bit9"),
    ("PHY_RG_IDDIG", "BIT1", "vendor RG_IDDIG in PHY DTM1 bit1"),
    ("MT6771_UPHY_DTM1", "0x036C", "PHY DTM1 offset (usb0phy + 0x300 + 0x6C)"),
]



def verify_edk2_patches(root):
    print("[invariants] EDK2 build-time patches")
    patch = root / "Patches/apply_edk2_mtk_patches.py"
    check(patch.is_file(), "MTK EDK2 patch script exists", str(patch))
    if patch.is_file():
        text = patch.read_text(errors="replace")
        check("RsvdZ5 = 1" in text,
              "patch writes MTK EP_BPKTS(1) to RsvdZ5")
        check("USB_ENDPOINT_CONTROL" in text,
              "patch anchor targets the endpoint-context initializers")
        check(text.count("count(anchor)") >= 1,
              "patch checks anchor count before applying")
        check("updated = text.replace(anchor, block + anchor)" in text,
              "patch replaces ALL anchor matches (both 32- and 64-byte paths)")
        check("A3_INT_DIAG" in text,
              "patch injects interrupt-transfer completion diagnostic")
        check("Urb->Result" in text and "Urb->Completed" in text,
              "diagnostic reports URB completion and result")
    build = root / "build.sh"
    check(build.is_file() and "apply_edk2_mtk_patches.py" in build.read_text(),
          "build.sh runs the EDK2 patch script")


def verify_porting_invariants(root):
    print("[invariants] OTG / USB source values")
    src = (root / "Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771UsbHostDxe"
                  "/Mt6771UsbHostDxe.c")
    if not src.is_file():
        check(False, "Mt6771UsbHostDxe.c exists", str(src))
        return
    text = src.read_text(errors="replace")
    for name, value, why in USB_HOST_INVARIANTS:
        pattern = r"#define\s+" + re.escape(name) + r"\s+.*" + re.escape(value)
        check(re.search(pattern, text) is not None,
              name + " uses " + value + " (" + why + ")",
              "definition missing or value changed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("boot_image", nargs="?")
    ap.add_argument("fd", nargs="?")
    ap.add_argument("--root", default=None,
                    help="repository root (default: parent of Tools/)")
    ap.add_argument("--usb-mode", default=None, choices=["OTG", "ACM"],
                    help="which USB driver the image is expected to contain")
    args = ap.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent

    verify_platform(root, args.usb_mode)
    verify_porting_invariants(root)
    verify_edk2_patches(root)
    verify_last_commit_helper(root)

    if args.boot_image:
        p = Path(args.boot_image)
        if not p.is_file():
            check(False, "boot image exists", str(p))
        else:
            verify_boot_image(p)
    else:
        print("[boot.img] skipped (no path given)")

    if args.fd:
        fd = Path(args.fd)
        if not fd.is_file():
            check(False, "firmware volume exists", str(fd))
        else:
            print(f"[firmware] {fd}")
            data = fd.read_bytes()
            check(len(data) > 0, "firmware volume is non-empty")
            # FFS files are GUID-keyed; the UI names are UTF-16 and only exist
            # for files that declare one, so just assert sensible size.
            check(len(data) <= 8 * 1024 * 1024,
                  "firmware volume fits the 8 MiB FD region limit",
                  f"size={len(data)}")

    print()
    if FAILS:
        print(f"{len(FAILS)} of {CHECKS} checks FAILED:")
        for f in FAILS:
            print(f"  - {f}")
        return 1

    print(f"all {CHECKS} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
