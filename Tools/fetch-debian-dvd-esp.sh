#!/bin/sh
# Build an ESP image from the official Debian arm64 DVD-1 ISO.
#
# The ISO is discovered on cdimage.debian.org (current release) and verified
# against SHA256SUMS, then reused from the cache on later runs.
#
# From the ISO we take:
#   install.a64/vmlinuz        installer kernel
#   install.a64/initrd.gz      installer initrd
#   efi/boot/bootaa64.efi      Debian's own GRUB (version matched, no extra deps)
#
# They are packed into a FAT32 image together with our board DTB.
#
# Usage:
#   Tools/fetch-debian-dvd-esp.sh [-o esp-dvd.img] [-c iso-cache] [-s sizeMB]
set -eu

OUT=esp-dvd.img
CACHE=${ISO_CACHE:-./iso-cache}
SIZE_MB=512

while getopts "o:c:s:" opt; do
  case "$opt" in
    o) OUT="$OPTARG" ;;
    c) CACHE="$OPTARG" ;;
    s) SIZE_MB="$OPTARG" ;;
    *) echo "usage: $0 [-o esp.img] [-c cache-dir] [-s sizeMB]" >&2; exit 1 ;;
  esac
done

BASE="https://cdimage.debian.org/debian-cd/current/arm64/iso-dvd"
mkdir -p "$CACHE"

echo "== discovering current arm64 DVD-1 ISO =="
NAME=$(curl -fsSL "$BASE/" | grep -oE 'debian-[0-9.]+-arm64-DVD-1\.iso' | head -1)
[ -n "$NAME" ] || { echo "cannot discover ISO name at $BASE/" >&2; exit 1; }
echo "   $NAME"

ISO="$CACHE/$NAME"
if [ ! -f "$ISO" ]; then
  echo "== downloading $NAME (large; cached afterwards) =="
  curl -fL --retry 3 -o "$ISO.part" "$BASE/$NAME"
  mv "$ISO.part" "$ISO"
fi
ls -lh "$ISO"

echo "== verifying checksum =="
curl -fsSL "$BASE/SHA256SUMS" -o "$CACHE/SHA256SUMS"
WANT=$(grep " $NAME\$" "$CACHE/SHA256SUMS" | awk '{print $1}')
if [ -n "$WANT" ]; then
  GOT=$(sha256sum "$ISO" | awk '{print $1}')
  if [ "$WANT" != "$GOT" ]; then
    echo "checksum mismatch: want $WANT got $GOT" >&2
    rm -f "$ISO"
    exit 1
  fi
  echo "   ok $GOT"
else
  echo "   WARNING: $NAME not in SHA256SUMS, skipping verification"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if command -v 7z >/dev/null 2>&1; then
  list()    { 7z l "$ISO" | awk '{print $NF}'; }
  extract() { 7z e -y -o"$WORK" "$ISO" "$1" >/dev/null; }
elif command -v bsdtar >/dev/null 2>&1; then
  list()    { bsdtar -tf "$ISO"; }
  extract() { bsdtar -xf "$ISO" -C "$WORK" "$1"; }
else
  echo "need p7zip-full or libarchive-tools to read the ISO" >&2
  exit 1
fi

echo "== locating files inside the ISO =="
GRUB_PATH=$(list | grep -iE '^(efi)/boot/bootaa64\.efi$' | head -1)
KERN_PATH=$(list | grep -E '^install\.[a-z0-9]+/vmlinuz$' | head -1)
INIT_PATH=$(list | grep -E '^install\.[a-z0-9]+/initrd\.gz$' | head -1)
[ -n "$KERN_PATH" ] || { echo "installer vmlinuz not found in ISO" >&2; exit 1; }
[ -n "$INIT_PATH" ] || { echo "installer initrd.gz not found in ISO" >&2; exit 1; }
echo "   kernel : $KERN_PATH"
echo "   initrd : $INIT_PATH"
if [ -n "$GRUB_PATH" ]; then
  echo "   grub   : $GRUB_PATH"
else
  echo "   grub   : NOT FOUND (will fall back to grub-mkstandalone)"
fi

extract "$KERN_PATH"
extract "$INIT_PATH"
[ -n "$GRUB_PATH" ] && extract "$GRUB_PATH"

VMLINUZ="$WORK/$(basename "$KERN_PATH")"
INITRD="$WORK/$(basename "$INIT_PATH")"
GRUBEFI=""
[ -n "$GRUB_PATH" ] && GRUBEFI="$WORK/$(basename "$GRUB_PATH")"

[ -f "$VMLINUZ" ] || { echo "kernel extraction failed" >&2; exit 1; }
[ -f "$INITRD" ]  || { echo "initrd extraction failed" >&2; exit 1; }
ls -lh "$VMLINUZ" "$INITRD"

echo "== building ESP =="
if [ -n "$GRUBEFI" ] && [ -f "$GRUBEFI" ]; then
  sh "$(dirname "$0")/mkesp.sh" -o "$OUT" -g "$GRUBEFI" -k "$VMLINUZ" -i "$INITRD" -s "$SIZE_MB"
else
  sh "$(dirname "$0")/mkesp.sh" -o "$OUT" -k "$VMLINUZ" -i "$INITRD" -s "$SIZE_MB"
fi

echo
echo "done: $OUT"
echo "ISO cached at: $ISO"
echo "For a full install, copy that ISO to a partition and use the loopback"
echo "menu entry in the ESP's /boot/grub/grub.cfg."
