#!/bin/sh
# Fetch the Debian arm64 installer kernel + initrd and build a ready ESP image
# containing GRUB (arm64), our board DTB, that kernel and its initrd.
#
# Why the installer kernel: Debian's generic arm64 kernel is the only widely
# available kernel that ships MediaTek support as modules
#   CONFIG_ARCH_MEDIATEK=y CONFIG_PINCTRL_MT6779=y
#   CONFIG_MMC_MTK=m CONFIG_COMMON_CLK_MT6779=m
# and CONFIG_FB_EFI=y, so it can draw its console on the framebuffer our
# firmware exposes over GOP.
#
# Usage:
#   Tools/fetch-debian-netboot.sh [-s suite] [-o esp.img]
#   Tools/fetch-debian-netboot.sh -I /path/debian-arm64-netinst.iso   # from ISO
set -eu

SUITE=trixie
OUT=esp-debian.img
ISO=""

usage() { echo "usage: $0 [-s suite] [-o esp.img] [-I installer.iso]" >&2; exit 1; }
while getopts "s:o:I:" opt; do
  case "$opt" in
    s) SUITE="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    I) ISO="$OPTARG" ;;
    *) usage ;;
  esac
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ -n "$ISO" ]; then
  [ -f "$ISO" ] || { echo "iso not found: $ISO" >&2; exit 1; }
  echo "== extracting installer kernel/initrd from $ISO =="
  command -v 7z >/dev/null 2>&1 || { echo "need 7z (p7zip-full) or use netboot mode" >&2; exit 1; }
  7z e -o"$WORK" "$ISO" install.a64/vmlinuz install.a64/initrd.gz >/dev/null
  mv "$WORK/vmlinuz" "$WORK/vmlinuz.iso"
  mv "$WORK/initrd.gz" "$WORK/initrd.iso.gz"
  KERNEL="$WORK/vmlinuz.iso"
  INITRD="$WORK/initrd.iso.gz"
else
  BASE="https://deb.debian.org/debian/dists/$SUITE/main/installer-arm64/current/images/netboot/debian-installer/arm64"
  echo "== downloading $BASE =="
  curl -fL --retry 3 -o "$WORK/vmlinuz" "$BASE/vmlinuz"
  curl -fL --retry 3 -o "$WORK/initrd.gz" "$BASE/initrd.gz"
  # netboot vmlinuz may be gzip-compressed; GRUB on arm64 expects a plain Image
  if [ "$(head -c2 "$WORK/vmlinuz" | od -An -tx1 | tr -d ' \n')" = "1f8b" ]; then
    echo "   netboot vmlinuz is gzip-compressed -> decompressing"
    gzip -dc "$WORK/vmlinuz" > "$WORK/Image"
  else
    cp "$WORK/vmlinuz" "$WORK/Image"
  fi
  KERNEL="$WORK/Image"
  INITRD="$WORK/initrd.gz"
fi

echo "== building ESP =="
sh "$(dirname "$0")/mkesp.sh" -o "$OUT" -k "$KERNEL" -i "$INITRD" -s 512

echo
echo "done: $OUT"
echo "write it to the ESP partition, e.g.:"
echo "  dd if=$OUT of=/dev/mmcblk0p43 bs=1M conv=fsync"
