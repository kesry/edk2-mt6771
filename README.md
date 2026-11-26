# edk2-mt6771

EDK2 (UEFI) port for MediaTek MT6771, currently targeting **OPPO A3 / PADT00**.

## Status (2026-11-26)

- [x] Repository initialized from `linux-mediatek/edk2-mtk` `next` @ `fdb0ef928ac70ff0f2725b4cb302db4d25a55c47`
- [x] Platform package `Platform/OPPO/A3Pkg` added (AARCH64, DEBUG/RELEASE)
- [x] Boot/DT/PMIC facts extracted from the original `Android_boot.img` and reference repos
- [x] GitHub Actions build workflow added
- [x] UEFI firmware builds successfully in CI (run #9, 2026-11-26; see docs/ci)
- [x] boot.img packaging passes in CI (run #11, 2026-11-26; 32 MiB, MTK header + AVB verified)
- [ ] Runtime boot on device verified (TODO: needs physical device + backup)
- [x] eMMC/MSDC0 host + MmcDxe added and compiled into firmware (run #18)
- [ ] eMMC BlockIo / fs0: verified on real device (TODO)
- [ ] Storage / display / input verified under UEFI (TODO)

This is a **work in progress**. Do not flash to a device unless you have a
verified recovery path and a backup of the boot partition.

## Sources

- Base: <https://github.com/linux-mediatek/edk2-mtk>, branch `next`, commit `fdb0ef928ac70ff0f2725b4cb302db4d25a55c47`
- Device/mainline Linux reference: <https://github.com/kesry/mainline-linux-mt6771>
- Original boot image facts: local `Android_boot.img` (not committed; parsed offline)

See [docs/porting.md](docs/porting.md) for the branch decision and all hardware facts.

## Layout

```
BootShim/                    assembly shim prepended to the UEFI FD
Common/edk2/                 upstream EDK2 submodule (pinned by base repo)
Common/edk2-platforms/       upstream EDK2 platforms submodule (pinned by base repo)
Platform/*/                  board packages from the base repo (reference)
Platform/OPPO/A3Pkg/         MT6771 / OPPO A3 UEFI platform package (this port)
Silicon/MediaTek/            MediaTek SoC packages from the base repo
Tools/                       helper scripts
docs/                        porting, build and security notes
.github/workflows/build.yml  CI build
```

The base EDK2 directory layout is kept intact; MT6771 changes are confined to
`Platform/OPPO/A3Pkg/`, `Tools/`, `docs/`, and the workflow.

## Build

See [docs/build.md](docs/build.md). In short:

```sh
./build.sh -s OPPO/A3Pkg
./build.sh -t RELEASE OPPO/A3Pkg
```

Output (when successful): `Build/A3/FV/A3_UEFI.fd` and the flashable
`Build/boot-uefi.img`. In GitHub Actions, `boot-uefi.img` is uploaded as its own
artifact named `boot-uefi-<run_number>`.

## Known limitations

- MT6771 is not upstream in the base EDK2 tree. This port reuses the MT8183
  silicon package, following the reference projects which run MT8183 UEFI code
  with MT6771 device trees; register-level differences may remain.
- `mvn` (modem) reserved memory is intentionally not represented; the mainline
  Linux project returned it to the kernel.
- No serial output has been confirmed on this device. The original DTB exposes
  UART0/1/2 but the mainline project notes there is no UART test point wired up.
- The LK bootloader applies a vendor DTBO overlay by symbol name. Any DTB used
  with this firmware must keep its `/__symbols__` entries, or boot will fail.
- UEFI runtime validation on hardware is not done yet. Treat every build as
  experimental.

## License

BSD-2-Clause, inherited from the base repository. See `LICENSE`.
