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

## 9. USB 主机的阻塞点：VBUS 5V 由 MT6370 控制（2026-11-26）

### 现象

刷入含 `Mt6771UsbHostDxe` 的固件后，插入 2.4G 接收器与 USB 键盘均无反应，
**键盘 NUM LOCK 灯不亮** —— 说明端口没有输出 VBUS，设备根本未上电。

### 供电关系（重要，易混）

| 项目 | 电压 | 作用 |
|---|---|---|
| USB VBUS | **5V** | 给外接设备供电；host 模式下必须由手机升压输出 |
| `ldo_vusb` / VUSB33 | 3.3V | 只给手机内部 USB PHY 供电 |

3.3V 不能替代 VBUS：绝大多数 USB 设备要求 4.4–5.25V。

### 厂商源码证据

仓库：`https://github.com/oppo-source/A3-8.1-kernel-source`（branch `master`，
commit `da97cfa58a40dd13bb30472f2c02eab2ff0c85df`）。原机 DTB 中充电节点为
`mediatek,mt6370_pmu_charger`，即 **独立 MT6370 PMU 充电 IC**，不是 MT6358 内置充电器。

`drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_charger.c:1903` `mt6370_enable_otg()`：

```
enable hidden mode
set OTG_OC = 500 mA
write MT6370_PMU_REG_LG_CONTROL (0x33) = 0x7C      /* entering OTG */
set   MT6370_PMU_REG_CHGCTRL1  (0x11) bit0 OPA_MODE
msleep(20); verify bit
write MT6370_PMU_REG_CHGHIDDENCTRL6 (0x35) = 0x00
```

关闭时：清 OPA_MODE、`LG_CONTROL = 0x73`、`CHGHIDDENCTRL6 = 0x0F`。

### 仍缺的三块

1. **MTK I2C 主控制器驱动**（UEFI 无现成实现）。原机 DTB 给出 12 个
   `mediatek,i2c` 控制器基址（`0x11007000`/`0x11011000`/`0x11009000`…）。
   MT6370 的 I2C 地址与总线归属需进一步确认（DTB 中 `mt6370_pmu_dts` 节点未带 `reg`）。
2. **MT6370 "hidden mode" 访问**：OTG 序列依赖隐藏寄存器解锁流程。
3. **USB PHY 初始化**：厂商 `drivers/misc/mediatek/mu3phy/mt6771/mtk-phy-asic.c`
   （1462 行）含 MT6771 专用 PHY 寄存器表，需按 host 模式移植。

### 结论

USB host 能否工作不取决于 xHCI（那部分已实现），而取决于上述三块。
其中 VBUS 是硬前提：没有 5V，插任何设备都不会枚举。

诊断固件（含屏幕日志）已构建：run #29 `boot-uefi-29`，会在 Shell 画面打印
IPPC 寄存器与 xHCI `CAPLENGTH`，用于判断 MTU3 是否已被时钟起来。

## 10. USB host / VBUS 调试链（2026-11-26 记录）

真机逐步定位的过程，供后续参考。

### 已确认的事实

| 结论 | 证据 |
|---|---|
| LK 会跳到我们的 payload | 探针镜像（BootShim 0x40 直接写 TopRGU）→ 几秒重启 |
| BootShim 能把 `A3_UEFI.fd` 搬到 `0x50000000` | 拷贝后写 TopRGU → 反复重启 |
| `br UEFI_BASE` 能执行 | 拷贝 + 覆盖入口为 TopRGU 桩 + 跳转 → 重启 |
| EDK2 能启动到 Shell | run #26 起屏幕出现 UEFI Shell |
| framebuffer 地址是 `0x77440000`（非 `0x7d170000`） | 从可启动 DTB 解出 `framebuffer@77440000`，stride 4352 |
| **xHCI 控制器活着** | 诊断显示 `xHCI CAP0 caplen = 32` |
| **I2C6 起初无响应** | `I2C6 INTR_STAT = 0x0000`，MT6370 读 `Device Error` |
| USB 口无 5V | 键盘 NUM LOCK / 鼠标 LED 不亮 |

### 供电与充电 IC

- VBUS 必须 5V；`ldo_vusb` 3.3V 只喂手机自身 PHY。
- 充电 IC 是**外部 MT6370 PMU**（DTB `mediatek,mt6370_pmu_charger`），
  不是 MT6358 内置充电器。
- MT6370 挂在 **I2C6 `0x11005000`**，7 位地址 `0x34`。
- OTG 升压序列见 `mt6370_pmu_charger.c:1903`：
  hidden mode(`0x07..0x0A`=`96 69 C3 3C`) → `LG_CONTROL(0x33)=0x7C`
  → `CHGCTRL1(0x11) bit0` → 20 ms → `CHGHIDDENCTRL6(0x35)=0x00`。

### I2C6 时钟门控

```
INFRA_AO 基址        0x10001000
infra3 CG set        +0xC0   bit6 = I2C6
infra3 CG status     +0xC8
```
来自厂商 `drivers/clk/mediatek/clk-mt6771.c`：`GATE_INFRA3(INFRACFG_AO_I2C6_CG, ..., 6)`。

时序（26 MHz 父时钟、100 kHz，按 `mtk_i2c_calculate_speed` 推算）：
`CLOCK_DIV=5`、`TIMING=0x6A03`、`LTIMING=0x3203`。

### USB PHY（MT6771）

基址 `0x11f40000`（DTB `usb0phy@11f40000`），U2 PHY COM 块在 `+0x300`：

| 寄存器 | 偏移 | 相关位 |
|---|---|---|
| `U3D_U2PHYDTM0` | `+0x68` | `FORCE_UART_EN` b26、`FORCE_SUSPENDM` b18 |
| `U3D_U2PHYDTM1` | `+0x6C` | `RG_VBUSVALID` b5 |
| `U3D_U2PHYACR4` | `+0x20` | `GPIO_CTL` b9、`GPIO_MODE` b8 |
| `U3D_USBPHYACR6` | `+0x18` | `OTG_VBUSCMP_EN` b20、`BC11_SW_EN` b23 |

当前实现只做"离开 UART 模式 / 释放 force suspend / 打开 OTG VBUS 比较器 /
置 VBUSVALID"，**完整模拟调参未移植**（厂商 `mtk-phy-asic.c` 有 1462 行）。

### 诊断界面

驱动 `Mt6771VbusDxe` 会把 LK framebuffer 整屏涂黑，从第 5 行开始打印，
并每 2 秒重绘 10 次（因为 Shell 启动会清屏）。显示内容：

```
INFRA3_CG_STA  before/after      I2C6 时钟门控状态
I2C6 CLOCK_DIV before/after
I2C6 TIMING    before/after
MT6370 read  : <status> val=0x..
MT6370 OTG   : <status>
   LG=0x.. CHGCTRL1=0x..          bit0=1 表示 boost 已开
PHY DTM0 / DTM1 / ACR6  before -> after
xHCI CAP0=0x........ caplen=..
```

### 11. 诊断版迭代记录（2026-11-26 晚）

| run | 改动 | 目的 |
|---|---|---|
| #31 | framebuffer 涂黑 + 第 5 行起打印 + 2 s 定时重绘 | Shell 清屏会吃掉 ReadyToBoot 的输出 |
| #32 | 解 I2C6 时钟门控 + 补时序 | `INTR_STAT=0x0000` 疑为无时钟 |
| #33 | USB PHY 诊断 + 最小 host 位 | 为 VBUS 通了之后做准备 |
| #34 | 内嵌 `startup.nsh` | 无输入设备时也能自动跑 `map -r` / `dh` |
| #35 | I2C 总线扫描（0x08–0x77，5 ms/地址） | 若 0x34 不通，看设备到底在哪 |
| #36 | **INFRA_AO 改为 32 位访问** + dump `0x11000600` pad 区 | 之前用 16 位写 CG 寄存器，几乎肯定没打开门控 |

关于 #36 的推断：MTK 的 `INFRA_AO` 时钟门控寄存器是 32 位；用 `MmioWrite16`
写 `+0xC0` 很可能无效，这解释了 #32 之后 `INFRA3_CG_STA` 仍未置位。

### startup.nsh 机制

`Platform/OPPO/A3Pkg/startup.nsh` 以 `FILE FREEFORM` + `SECTION UI="startup.nsh"`
嵌入 `[FV.FvMain]`，由 `FvSimpleFileSystemDxe` 暴露给 Shell 自动执行：

```
map -r            filesystem 列表（看 fs0: 是否出现 eMMC/U 盘）
dh -p BlockIo     块设备协议（eMMC 是否产出 BlockIo）
dh -p UsbHc       USB host controller
dh -p UsbIo       已枚举的 USB 设备
dh -p SerialIo    串口
```

注意：`A3_UEFI.fd` 里的 FVMAIN 被压缩在 FVMAIN_COMPACT 中，直接在 FD 二进制里
搜字符串是搜不到的；用构建日志中 FVMAIN 的字节数变化可以确认是否被打入
（加 startup.nsh 后从 5478784 增到 5479552）。

副作用：诊断驱动的黑屏重绘（6 次 × 2 s）会覆盖 Shell 自己打印的 startup.nsh 输出。
如果需要看 Shell 输出，把 `Mt6771VbusDxe` 的重绘次数改成 0 再构建。

### 12. I2C6 真正的失败原因（2026-11-26 深夜）

真机读数（run #37）：

```
INFRA3_CG_STA after = 0x00000040     ← 门控已打开（bit6）
CLOCK_DIV before = 0x0004 after = 0x0004
TIMING    before = 0x1303 after = 0x1303
I2C6 scan = 0 device(s)，MT6370 read = Device Error
```

`CLOCK_DIV`/`TIMING` **能读、能保持、且明显是 LK 配置的值**，说明：

- 控制器 MMIO 正常
- 时钟源已选通
- 引脚已 mux、上拉正常
- 总线硬件没有问题（LK 充电时就用这条总线跟 MT6370 通信）

**失败是自伤：`I2cPrepare()` 每次传输前都写 `I2C_SOFTRESET`。**
软复位会清掉 `TIMING`/`CLOCK_DIV`/`LTIMING`/`HS`/`IO_CONFIG`，
而重配逻辑只在"读回为 0"时触发（复位前读到非 0），因此复位后再无人恢复配置，
传输永远不完成 → 扫描全静默、所有地址都不 ACK。

修复（run #38）：

- 去掉每次传输的软复位，保留 LK 的配置
- 仅在读回确实为 0 时才写回退值，并在屏幕标注 `REPROGRAMMED` / `intact`
- `DELAY_LEN` 为 0 时补 2
- 完成判定同时接受"`START` 位自动清零"，避免某些配置不锁存中断状态

### 经验

遇到"寄存器读写正常但传输不完成"的总线问题，先看**自己有没有把别人配好的状态清掉**，
不要一上来怀疑引脚/上拉/时钟。本例中 LK 已经把 I2C6 配好，我们的复位把它的工作状态毁了。
