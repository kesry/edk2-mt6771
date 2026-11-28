#!/bin/sh
# Build a FAT32 ESP image for booting an arm64 kernel through this firmware.
#
# Layout produced:
#   /EFI/BOOT/BOOTAA64.EFI   GRUB arm64 standalone
#   /boot/grub/grub.cfg      embedded, also copied as a file for easy edits
#   /a3.dtb                  our board device tree
#   /Image                   kernel, if provided via $KERNEL (optional)
#
# Usage: Tools/mkesp.sh [-o esp.img] [-k /path/to/Image] [-d a3.dtb]
set -eu

OUT=esp.img
KERNEL=""
DTB=""
SIZE_MB=512

usage() { echo "usage: $0 [-o esp.img] [-k Image] [-d dtb] [-s sizeMB]" >&2; exit 1; }
while getopts "o:k:d:s:" opt; do
  case "$opt" in
    o) OUT="$OPTARG" ;;
    k) KERNEL="$OPTARG" ;;
    d) DTB="$OPTARG" ;;
    s) SIZE_MB="$OPTARG" ;;
    *) usage ;;
  esac
done

command -v grub-mkstandalone >/dev/null 2>&1 || { echo "grub-mkstandalone missing" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1            || { echo "mtools missing (apt install mtools)" >&2; exit 1; }
command -v mkfs.vfat >/dev/null 2>&1        || { echo "dosfstools missing" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ -n "$DTB" ] || DTB=Platform/OPPO/A3Pkg/Blob/a3-v186-tgpio.dtb
[ -f "$DTB" ] || { echo "dtb not found: $DTB" >&2; exit 1; }

cat > "$WORK/grub.cfg" <<'GRUBCFG'
set timeout=5
set default=0

# Text terminal: works with the simple-framebuffer GOP we expose.
terminal_output console

menuentry "fnOS (OPPO A3 / MT6771, our kernel)" {
    # Our own device tree: the phone's hardware is described here, not by ACPI
    # and not by any vendor Image's built-in DTB.
    devicetree /a3.dtb
    linux /Image root=PARTUUID=3edee4d4-eba3-4bfc-9e93-29bad7809196 rw \
          console=tty0 loglevel=7
}

menuentry "UEFI Shell" {
    exit
}
GRUBCFG

echo "== building GRUB arm64 standalone =="
grub-mkstandalone \
  -O arm64-efi \
  -o "$WORK/BOOTAA64.EFI" \
  --modules="normal linux devicetree part_gpt part_msdos fat ext2 search search_label configfile echo test" \
  --locales="" --themes="" --fonts="" \
  "boot/grub/grub.cfg=$WORK/grub.cfg"

echo "== creating ${SIZE_MB}MB FAT32 image: $OUT =="
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
mkfs.vfat -F32 -n A3ESP "$OUT" >/dev/null

mmd -i "$OUT" ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub
mcopy -i "$OUT" "$WORK/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$OUT" "$WORK/grub.cfg"     ::/boot/grub/grub.cfg
mcopy -i "$OUT" "$DTB"               ::/a3.dtb

if [ -n "$KERNEL" ]; then
  [ -f "$KERNEL" ] || { echo "kernel not found: $KERNEL" >&2; exit 1; }
  mcopy -i "$OUT" "$KERNEL" ::/Image
  echo "   added kernel: $KERNEL"
else
  echo "   NOTE: no kernel added; copy your Image to /Image on the ESP"
fi

echo "== done: $OUT =="
mdir -i "$OUT" ::/ ::/EFI/BOOT ::/boot/grub
