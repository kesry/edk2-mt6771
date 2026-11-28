#!/bin/sh
# Boot the fnOS ARM64 kernel in a QEMU "virt" VM.
#
# Two modes are provided because they fail for different reasons:
#   kernel  - QEMU loads vmlinuz + initrd directly. No UEFI, no ISO, no El
#             Torito. This isolates "can the fnOS kernel run at all?".
#   iso     - AAVMF (QEMU_EFI.fd) boots the ISO from virtio-scsi. This is the
#             installer experience and needs the CD on SCSI, not virtio-blk
#             (AAVMF does not see El Torito over virtio-blk).
#
# Usage:
#   Tools/vm/boot-fnos-vm.sh kernel -b /mnt/boot -d /opt/fnos-rootfs.img
#   Tools/vm/boot-fnos-vm.sh iso    -i /path/fnos-arm.iso -d /opt/fnos-disk.img
set -eu

MODE=""
BOOTDIR=""
ROOTFS=""
ISO=""
DISK=""
MEM=4096
SMP=4
DISPLAY="nographic"
ACCEL="kvm"

usage() {
  cat >&2 <<'USAGE'
usage: boot-fnos-vm.sh <kernel|iso> [options]
  -b DIR   dir holding vmlinuz-*/initrd.img-*   (kernel mode)
  -d IMG   disk image (raw). In kernel mode this is the rootfs.
  -i ISO   installer ISO                        (iso mode)
  -m MB    memory, default 4096
  -s N     vcpus, default 4
  -v       use VNC (:0) instead of -nographic
  -t       force TCG (no KVM)
USAGE
  exit 1
}

[ $# -ge 1 ] || usage
MODE="$1"; shift
while getopts "b:d:i:m:s:vt" opt; do
  case "$opt" in
    b) BOOTDIR="$OPTARG" ;;
    d) DISK="$OPTARG" ;;
    i) ISO="$OPTARG" ;;
    m) MEM="$OPTARG" ;;
    s) SMP="$OPTARG" ;;
    v) DISPLAY="vnc :0" ;;
    t) ACCEL="tcg" ;;
    *) usage ;;
  esac
done

echo "== environment =="
qemu-system-aarch64 --version | head -1 || { echo "qemu-system-aarch64 missing"; exit 1; }
if [ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
  echo "/dev/kvm: present"
else
  echo "/dev/kvm: MISSING -> falling back to TCG (slow but functional)"
  ACCEL="tcg"
fi
FW=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd
[ -f "$FW" ] && echo "AAVMF: $FW" || echo "AAVMF: $FW NOT FOUND (iso mode needs it: apt install qemu-efi-aarch64)"

COMMON="-M virt,gic-version=3 -cpu max -smp $SMP -m $MEM -accel $ACCEL"

case "$MODE" in
kernel)
  [ -n "$BOOTDIR" ] || usage
  VMLINUZ=$(ls "$BOOTDIR"/vmlinuz-* 2>/dev/null | head -1)
  INITRD=$(ls "$BOOTDIR"/initrd.img-* 2>/dev/null | head -1)
  [ -n "$VMLINUZ" ] && [ -n "$INITRD" ] || { echo "no vmlinuz/initrd in $BOOTDIR"; exit 1; }
  echo "kernel: $VMLINUZ"
  echo "initrd: $INITRD"
  [ -n "$DISK" ] || { echo "kernel mode needs -d rootfs.img"; exit 1; }
  # shellcheck disable=SC2086
  exec qemu-system-aarch64 $COMMON \
    -kernel "$VMLINUZ" -initrd "$INITRD" \
    -append "console=ttyAMA0,115200 earlycon=pl011,0x9000000 root=/dev/vda rw loglevel=7" \
    -drive file="$DISK",if=none,id=d0,format=raw \
    -device virtio-blk-pci,drive=d0 \
    -device virtio-net-pci,netdev=n0 -netdev user,id=n0 \
    $DISPLAY
  ;;
iso)
  [ -n "$ISO" ] || usage
  [ -f "$FW" ] || { echo "install qemu-efi-aarch64 first"; exit 1; }
  [ -n "$DISK" ] || { echo "iso mode needs -d target-disk.img"; exit 1; }
  # shellcheck disable=SC2086
  exec qemu-system-aarch64 $COMMON \
    -bios "$FW" \
    -device virtio-scsi-pci,id=scsi0 \
    -drive file="$ISO",if=none,id=cd0,media=cdrom,readonly=on \
    -device scsi-cd,drive=cd0,bus=scsi0.0 \
    -drive file="$DISK",if=none,id=d0,format=raw \
    -device virtio-blk-pci,drive=d0 \
    -device virtio-net-pci,netdev=n0 -netdev user,id=n0 \
    -device virtio-gpu-pci \
    $DISPLAY
  ;;
*) usage ;;
esac
