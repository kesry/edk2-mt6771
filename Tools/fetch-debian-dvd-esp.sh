#!/bin/sh
# Build an ESP image from the official Debian arm64 DVD-1 ISO.
#
# The ISO is discovered from cdimage.debian.org (current release), verified
# against SHA256SUMS, then the installer kernel/initrd are extracted and packed
# into a FAT32 image together with GRUB arm64 and our board DTB.
#
# Usage:
#   Tools/fetch-debian-dvd-esp.sh [-o esp-dvd.img] [-c /path/iso-cache]
#
# The ISO is kept in the cache directory so repeated runs skip the download.
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
  echo "== downloading $NAME (large, cached afterwards) =="
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
  echo "   WARNING: $NAME not listed in SHA256SUMS, skipping verification"
fi

echo "== extracting installer kernel and initrd =="
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if command -v 7z >/dev/null 2>&1; then
  extract() { 7z e -y -o"$WORK" "$ISO" "$1" >/dev/null; }
  list() { 7z l "$ISO" | awk '{print $NF}'; }
elif command -v bsdtar >/dev/null 2>&1; then
  extract() { bsdtar -xf "$ISO" -C "$WORK" "$1"; }
  list() { bsdtar -tf "$ISO"; }
else
  echo "need p7zip-full or libarchive-tools to read the ISO" >&2
  exit 1
fi

VMLINUZ_PATH=$(list | grep -E '^install\.[a-z0-9]+/vmlinuz$' | head -1)
INITRD_PATH=$(list | grep -E '^install\.[a-z0-9]+/initrd\.gz$' | head -1)
[ -n "$VMLINUZ_PATH" ] || { echo "vmlinuz not found in ISO" >&2; exit 1; }
[ -n "$INITRD_PATH" ] || { echo "initrd.gz not found in ISO" >&2; exit 1; }
echo "   $VMLINUZ_PATH"
echo "   $INITRD_PATH"

extract "$VMLINUZ_PATH"
extract "$INITRD_PATH"
VMLINUZ="$WORK/$(basename "$VMLINUZ_PATH")"
INITRD="$WORK/$(basename "$INITRD_PATH")"
[ -f "$VMLINUZ" ] || { echo "extraction of vmlinuz failed" >&2; exit 1; }
[ -f "$INITRD" ]  || { echo "extraction of initrd failed" >&2; exit 1; }
ls -lh "$VMLINUZ" "$INITRD"

echo "== building ESP =="
sh "$(dirname "$0")/mkesp.sh" -o "$OUT" -k "$VMLINUZ" -i "$INITRD" -s "$SIZE_MB"

echo
echo "done: $OUT"
echo "The DVD ISO itself is at: $ISO"
echo "For a full install, put it on a partition and use the loopback entry in"
echo "the ESP's /boot/grub/grub.cfg."
