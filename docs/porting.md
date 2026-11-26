# MT6771 UEFI 移植记录

## 1. 基准分支选择

检查了 `linux-mediatek/edk2-mtk` 的全部远程分支（完整历史）：

| 分支 | HEAD commit | 相对 `next` | 结论 |
|---|---|---|---|
| `next` | `fdb0ef928ac70ff0f2725b4cb302db4d25a55c47` (2025-08-26) | 主线，最新 | **选用** |
| `next-cpufreq` | `05af7407` `[WIP] MT8183Pkg: Introduce CPUDxe` | 落后 19 个提交 | 未采用：MT8183 专用、依赖 MT6358 PMIC、WIP |
| `next-msdc` | `89573e43` `OH MY GODDD R U FR` | 落后 17 个提交 | 未采用：作者草稿，MT6789 专用 |
| `next-msdc-test` | `0115d361` `BlockIo testing` | 落后 17 个提交 | 未采用：测试代码 |
| `next-wdt` | `e7fcf5db` `WatchDogDxe: Workaround broken Timer` | 落后 25 个提交 | 未采用：`next` 已有正式 watchdog/timer 修复 |
| `old` | `ac4b3603` | 落后 48 个提交 | 未采用：重构前高通旧代码 |

**结论：使用固定 commit `fdb0ef928ac70ff0f2725b4cb302db4d25a55c47`（分支 `next`）。**
所有特性分支都从 `next` 的早期点分叉且未合回主线，落后 17~48 个修复；
它们针对 MT8183/MT6789 的 WIP 功能，不适合作为 MT6771 的地基。

## 2. 硬件事实来源（不臆造）

### 2.1 原机 `Android_boot.img`（本地解析，未提交）

用 Node.js 解析 boot header（v2），结果：

```
page_size=2048, header_version=2, os_version=0x14000154
kernel_size=11363263, kernel_addr=0x40080000
ramdisk_size=821923, ramdisk_addr=0x55000000
second_size=0, second_addr=0x40f00000, tags_addr=0x54000000
dtb_size=104530, dtb_addr=0x54000000
cmdline: bootopt=64S3,32N2,64N2 buildvariant=user ...
```

boot.img 的 dtb 段不是裸 FDT，而是 64 字节大端 MTK DTB 头（magic `0xD7B7AB1E`）+
104466 字节 FDT。FDT 关键节点：

```
model = "mt6771"; compatible = "mediatek,mt6771"
/memory: reg = <0x40000000 0x1e605000>            (4 GiB DRAM)
/chosen/bootargs: console=tty0 console=ttyS0,921600n1 ... androidboot.hardware=mt6771
/msdc@11230000: compatible="mediatek,msdc", bus-width=8, non-removable, status="okay"  (eMMC)
/msdc@11240000: compatible="mediatek,msdc", bus-width=4, SD
/pwrap@1000d000/mt6358-pmic: status="okay"        (MT6358 PMIC)
/toprgu@10007000: compatible="mediatek,toprgu"    (watchdog)
/serial@11002000, /serial@11003000, /serial@11004000: mediatek,mt6577-uart
/reserved-memory: sspm_share 0x510000, scp_share 0x1400000, consys 0x200000,
                  zmc-default size 0xffc00000
/mtkfb@0: compatible="mediatek,mtkfb"; /dsi0@14014000; /dsi1@1401f000
```

### 2.2 `kesry/mainline-linux-mt6771`（HEAD `60386617`）

| 事实 | 依据文件 |
|---|---|
| 目标设备 OPPO A3 (PADT00), MT6771, 4 GiB, eMMC, 1080×2280 NT36672A | `README.md` |
| framebuffer `0x7d170000`, 16 MiB, 1080×2280, `a8r8g8b8` | `patch/0001-arm64-dts-mediatek-add-minimal-MT6771-OPPO-A3-suppor.patch` |
| ramoops `0x54410000`, 896 KiB | 同上 |
| GICv3 `0x0c000000`(GICD)/`0x0c100000`(GICR), PSCI SMC | 同上 |
| Watchdog `0x10007000` 绑定 `mediatek,mt8183-wdt`；LK 启动前开了 WDT，必须尽早关 | 同上 + `patch/0004` |
| UART0 `0x11002000`, IRQ 91, 26 MHz；设备无 UART 测试点 | `patch/0002` |
| LK 会用 DTBO overlay 按符号名覆盖 DTB，缺 `/__symbols__` 会导致 `dtb_overlay fail` | `patch/0003` |
| boot.img 打包：page 2048, header v2, kernel+DTB 双放, MTK DTB 头, AVB 嫁接 | `script/mkbootimg-a3.py`, `docs/boot打包.md` |
| 已验证 DTB `a3-v186-good.dtb` md5 `21a2f20cc9602235102ae2c057c81e6e`，触摸版 `a3-v186-tgpio.dtb` | `docs/boot打包.md`, `script/a3-mkboot.sh` |
| 48-bit VA/PA（MT6771 是 ARMv8.0，无 LPA2） | `script/a3-mkboot.sh` |

## 3. 移植改动清单

1. 导出基线 `next@fdb0ef9` 的完整工作树（保留 EDK2 目录结构、子模块定义）。
2. 新增 `Platform/OPPO/A3Pkg/`（从 `Mashtab/T1Pkg` 模板派生）：
   - `a3.dsc`：PLATFORM_NAME=a3，framebuffer 1080×2280 @ `0x7d170000`，
     `PlatformMemoryMapLib` 指向 A3Pkg；仍 include `Silicon/MediaTek/MT8183Pkg/MT8183.dsc`。
   - `a3.fdf` / `a3.fdf.inc` / `Apriori.fdf.inc`：固件布局，沿用 MT8183 驱动。
   - `PlatformBuild.py`：产品名 a3；打包参数按原机 boot.img 实测值修正；
     调用 `Tools/mkbootimg-a3.py` 生成 boot.img。
   - `Library/PlatformMemoryMapLib/`：按原机 DTB 与主线上游事实重排内存映射，
     framebuffer 为 `0x7d170000` + 16 MiB，保留原厂保留区注释。
   - `Blob/a3-v186-tgpio.dtb`：来自 mainline 参考仓库的触摸可用 DTB（md5 79407986...）。
   - `Blob/ramdisk`：空 ramdisk（84 B）。
   - `Blob/stock-vbmeta.bin`：从原机 boot.img 提取的 AVB0 vbmeta（0xd40 字节）。
3. 新增 `Tools/mkbootimg-a3.py`：来自 mainline 参考仓库，支持 MTK DTB 头、
   DTB 双放、AVB 嫁接；补充支持裸 vbmeta blob 输入。
4. 新增 `.github/workflows/build.yml`：Ubuntu 24.04 上安装工具链，检出固定
   EDK2/EDK2-platforms commit，安装 edk2toolext，执行 `build.sh`，上传日志和产物。
5. 新增文档 `README.md`、`docs/build.md`、`docs/security.md`、本文件。

## 4. 验证状态

- ✅ 原机 boot.img 离线解析成功，参数和 DTB 硬件事实已记录（见 2.1）。
- ✅ mainline 参考仓库 DTB 与文档已核对，关键值可复现（md5 见 2.2）。
- ✅ 仓库初始化、平台包、文档、workflow 已创建。
- ⬜ GitHub Actions 首次编译：待推送后运行（结果写入 CI 日志）。
- ⬜ 真机启动：未验证（需要刷机与恢复手段）。
- ⬜ 存储/显示/输入：未验证。

## 5. 已知问题 / TODO

- `MT8183Pkg` 与 MT6771 并非完全相同；寄存器级差异需实机验证。
- UEFI 的 watchdog 关闭逻辑尚未单独验证（参考内核 `patch/0004` 的 `#reset-cells`）。
- DTBO 符号：本次使用 `a3-v186-tgpio.dtb`（已带 `/__symbols__`），若替换 DTB 必须保留符号。
- 若构建因 edk2toolext 依赖版本失败，按失败日志锁定版本后重试；不得伪造成功结果。

## 6. 补充：LK 启动链约束（来自 mainline 仓库 git history / script）

以下条目来自 `kesry/mainline-linux-mt6771` 的提交历史与脚本，是 UEFI 替换内核入口时必须满足的条件：

| 约束 | 依据 | 对本项目的意义 |
|---|---|---|
| LK 套用 odmdtbo 覆盖，缺 `/__symbols__` → `dtb_overlay fail` | `patch/0003` | 保留 tgpio DTB 的符号；替换 DTB 时须重新生成 `-@` 符号 |
| LK 需要 AVB footer，否则 `bootreason=lk_crash` | `script/mkbootimg-a3.py` graft_avb 注释 | `Tools/mkbootimg-a3.py` 已嫁接原机 vbmeta+footer |
| kernel Image 头：`image_size=0x257f000`、`text_offset=0x80000`、`code0` 必须是分支 | `script/a3-prep-kernel.py` | BootShim 入口已满足 ARM64 `b` 指令；image_size 需实机确认 |
| TopRGU 看门狗由 LK 预置，20~30 秒重启 | `patch/0001/0004` | UEFI 必须尽早禁用 WDT（TODO：实现/验证） |
| 内核侧 `CONFIG_FB_SIMPLE` 曾导致花屏（stride 4352 vs 4320） | commit `7d54087`,`31a3b17` | UEFI `PcdMipiFrameBuffer...` 使用 1080×2280/stride 4320；仍需实机验证 |
| MT6771 与 MT8183 在 MMSYS/DRM/I2C/WDT 上复用 IP，但寄存器需逐项核对 | `script/a3-mmsys-patch.py`, `patch/0005` | 继续以 `MT8183Pkg` 为基础，但不可假定全部寄存器一致 |
| 物理 UART 未引出；USB ACM 115200 可用 | `docs/partition-userdata-split.md`, `script/build-rootfs.sh` | UEFI 控制台优先 USB；串口调试 TODO |

## 7. eMMC BlockIo 支持（2026-11-26）

为了让 UEFI Shell 能看到 eMMC 分区（`fs0:`），新增了完整的 eMMC host 栈：

```
Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771MmcHostDxe/
  → 安装 gEmbeddedMmcHostProtocolGuid

EmbeddedPkg/Universal/MmcDxe/MmcDxe.inf
  → 消费 gEmbeddedMmcHostProtocolGuid，产生 BlockIo
```

### 事实依据

- `mainline-linux-mt6771/patch/0005-...:513-521`：
  `msdc@11230000`, reg `<0 0x11230000 0 0x10000>`, bus-width=8, non-removable, bootable。
- 同 patch `:662-665`：`msdc0_top@11f50000`, reg `<0 0x11f50000 0 0x1000>`。
- 同 patch `:216-219`：source clock 来自 `clk_msdcpll`，约 400 MHz；注意这是 "ASSUMED"，
  需要真机确认（文档中已作为 TODO）。
- 同 patch `:8179-8187`、`:8222-8252`：MT6771 MSDC 复用 MT6779 IP；LK 已完成 pinmux，
  主线内核用 `A3_MMC_NO_PINCTRL` 跳过 pinctrl。
- `linux-mediatek/edk2-mtk origin/next-msdc` 提供了寄存器定义、PIO 模型和 quirks 参考；
  但该驱动是 WIP，不安装任何 BlockIo 协议，不能直接用于读 ISO。
- Linux v6.12 `drivers/mmc/host/mtk-sd.c` 中 `mt6779_compat` 交叉验证了
  async_fifo/data_tune/busy_check/stop_clk_fix/enhance_rx 等 quirks 位。

### 实现状态

- ✅ 新增 `Mt6771MmcHostDxe`（GPL-2.0-only，引用上游 next-msdc 的寄存器定义）；
- ✅ 新增 PCD：`PcdMsdcMmioRegBase=0x11230000`、`PcdMsdcTopMmioRegBase=0x11f50000`、
  `PcdMsdcUseTop=TRUE`、`PcdMsdcSourceClockHz=400000000`；
- ✅ 仅支持 MSDC0/eMMC，PIO 模式；不支持 SD 卡、不支持 DMA/HS200/HS400；
- ✅ CI run #18 编译通过，产物中包含 `Mt6771MmcHostDxe` 和 `MmcDxe` 模块；
- ⬜ 真机验证：未做。需在 Shell 中确认 `fs0:`/`fs1:` 是否出现、能否读取 eMMC FAT 分区；
- ⬜ ISO9660 支持：未添加。下一步在 BlockIo 验证后再做；
- ⬜ 时钟/quirks：如果真机枚举失败，先调整 `PcdMsdcSourceClockHz` 并逐项关闭 quirks。

## 8. USB 主机支持（2026-11-26）

UEFI Shell 已能在屏幕上工作（run #26 起）。为获得输入与 U 盘支持，新增 USB host 栈。

### 硬件事实（原机 DTB 解码）

| 节点 | compatible | reg |
|---|---|---|
| `usb3@11200000` | `mediatek,mt6771-mtu3` | `<0x11201000 0x3000>`, `<0x11203e00 0x100>` ("mac","ippc") |
| `usb3_xhci@11200000` | `mediatek,mt677x-xhci` | `<0x11200000 0x1000>` |
| `usb0phy@11f40000` | `mediatek,mt6771-phy` | `<0x11f40000 0x10000>`, `<0x11203e00 0x100>` |

IPPC 寄存器偏移/位来自 Linux v6.12 `drivers/usb/mtu3/mtu3_hw_regs.h`：
`U3D_SSUSB_IP_PW_CTRL0 +0x00`（`SSUSB_IP_SW_RST BIT0`）、
`PW_CTRL1 +0x04`（`SSUSB_IP_HOST_PDN BIT0`）、
`PW_CTRL2 +0x08`（`SSUSB_IP_DEV_PDN BIT0`）、
`U2_CTRL_0P +0x50`（`FORCE_IDDIG BIT11`、`RG_IDDIG BIT12`、`PORT_PDN BIT1`）。
host 序列参照 `mtu3_plat.c:ssusb_ip_sw_reset()` 与 `mtu3_dr.c` 的 force/switch 逻辑。

### 新增

- `Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771UsbHostDxe/`
  - IPPC 切 host：整 IP 复位 → 关 gadget IP → 开 host IP → U2 口强制 IDDIG=host 且解除 PD
  - 通过 `RegisterNonDiscoverableMmioDevice(NonDiscoverableDeviceTypeXhci, ...)`
    把 `0x11200000/0x1000` 注册为 xHCI 设备
- DSC/FDF 加入 `NonDiscoverablePciDeviceDxe`、`XhciDxe`、`UsbKbDxe`
  （`UsbBusDxe`、`UsbMassStorageDxe` 原本已在）
- 新增 DEC 库映射 `NonDiscoverableDeviceRegistrationLib`

### 状态

- ✅ CI run #28 编译通过；产物 FV 中已确认包含
  `Mt6771UsbHostDxe`、`NonDiscoverablePciDeviceDxe`、`XhciDxe`、`UsbKbDxe`
- ⬜ 真机未验证：USB 键盘、U 盘、OTG 供电（VBUS）
- ⚠️ 未实现：MTU3 MAC 时钟、USB PHY（`0x11f40000`）上电、VBUS 开关。
  若 LK 没有把 MTU3 时钟/PHY 留在可用状态，控制器会读不到（CAPLENGTH=0），
  需要补充 PHY/时钟初始化。
- ⚠️ EDK2 pinned commit **没有 USB 串口类驱动**（CDC-ACM/CH340 均无）。
  要用 USB-TTL 适配器做串口控制台，需要额外移植 USB-serial 驱动。
