@echo -off
#
# Auto-run diagnostics for OPPO A3 (MT6771) UEFI.
# Embedded in the firmware volume so the shell executes it without any
# keyboard input (the board has no working input device yet).
#
echo ==============================================
echo  A3 UEFI auto diagnostic
echo ==============================================
echo -- filesystems --
map -r
echo
echo -- block io handles --
dh -p BlockIo
echo
echo -- usb host controller --
dh -p UsbHc
echo
echo -- usb io (enumerated devices) --
dh -p UsbIo
echo
echo -- serial io --
dh -p SerialIo
echo
echo ==============================================
echo  end of diagnostic
echo ==============================================
