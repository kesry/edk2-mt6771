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

### 13. eMMC 现状与日志通道计划（2026-11-26 深夜）

#### fastboot 读到的分区事实（`fastboot getvar all`）

| 项 | 值 | 意义 |
|---|---|---|
| `boot` | `0x2000000` = 32 MiB | 我们的 `boot-uefi.img` 正好 32 MiB，不会溢出 |
| `logical-block-size` | `0x200` = 512 | 与 BlockIo/MMC 假设一致 |
| `unlocked` / `secure` | yes / no | 刷机不受 AVB 阻碍 |
| 分区类型 | 全部 `ext4` 或 `raw data` | **没有任何 FAT**，所以 Shell 无法挂载 eMMC（`BLK0` 而非 `FS0` 属正常） |
| `cache` | `0x1b000000` = 432 MiB，ext4 | 用户指定作为 UEFI 日志分区（待 eMMC 可写后启用） |

#### 当前问题

`map -r` 只有一个 `BLK0: VenHw(4BB9B3E8-…)`，且：

```
blk=512  last=0
LBA1 is not a GPT header
```

`last=0` 表示这是**零容量块设备**，`4BB9B3E8` 也不是本仓库 MSDC 设备路径使用的
`2F7F6A70-5F1D-4B6C-9AD1-321C7E6D4A20`。结论：**eMMC 没有被识别**，
`MmcDxe` 没有为 eMMC 建立可用的 BlockIo。

#### 已加的诊断（run #44 起）

`Mt6771MmcHostDxe`：

- 初始化前后各打印一次 `MSDC_CFG`/`SDC_CFG`/`MSDC_PS`/`MSDC_INTEN`
- 前 32 条 MMC 命令打印 `index / arg / raw / status / MSDC_INT`
- 由于驱动在 BDS 清屏前加载，跟踪行先存入缓冲区，再由 3 s 定时器**重放 4 次**

目的：看清卡初始化停在 CMD0 / CMD1 / CMD2 / CMD3 哪一步，以及 MSDC 是否真的发出命令。

#### 日志通道计划（等 eMMC 可写）

`cache` 分区 432 MiB、ext4、可牺牲。文本日志写到该分区**末尾 4 MiB**，
避免破坏 ext4 元数据；之后在 Android/Linux 侧 root 后：

```
dd if=/dev/block/by-name/cache bs=1M skip=428 count=4 of=/tmp/uefi.log
```

即可取回。风险：写坏 cache 一般只会导致 OTA 失败，系统可自行重格式化。

### 14. eMMC 供电与引脚（对照主线仓库）

来源：`kesry/mainline-linux-mt6771` `patch/0005-a3-OPPO-A3-MT6771-bring-up.patch:214-225`
与厂商 `arch/arm64/boot/dts/mediatek/cust_mt6771_msdc.dtsi:17-37`。

| 项目 | 事实 | 对 UEFI 的含义 |
|---|---|---|
| eMMC 供电 | `vmmc-supply = <&mt_pmic_vemc_ldo_reg>`（MT6358 LDO_VEMC，3.3V） | 固件无 LDO 控制，依赖 LK 保持上电；若卡不应答需通过 MT6358 PMIC wrapper 打开 |
| 时钟源 | `clk_msdcpll` 400 MHz 作 "source"，`clk26m` 作 "hclk" | `PcdMsdcSourceClockHz = 400000000` |
| 分频 | 主线明确警告：分频由**假定源频**推导，源频写错会 overclock 并产生 I/O 错误 | 我们的 `MsdcSetClock` 按 400 MHz 计算，若实际源频不同则分频错误 → 表现为 CMD1 CRC 错 |
| 时钟门控 | `INFRACFG_AO_MSDC0_CG`(infra1 bit2)、`INFRACFG_AO_MSDC0_SCK_CG`(infra1 bit6) | 已加入驱动：写 `0x10001000+0x88` 置位，读 `+0x94` 校验 |
| 引脚 | 主线用 `A3_MMC_NO_PINCTRL`，LK 已 mux eMMC 引脚 | 固件不改 pinmux（与 I2C6 同样教训） |
| 速率能力 | 主线移除 high-speed（HS400 需 200 MHz + tuning） | 固件只做 default speed |

INFRA_AO infra1 CG：`set 0x88 / clr 0x8C / sta 0x94`（`clk-mt6771.c:1065`）。

### 15. 编译期自动验证

新增 `Tools/verify_firmware.py`，在 CI 中紧跟构建执行：

```
python3 Tools/verify_firmware.py Build/boot-uefi.img <A3_UEFI.fd>
```

检查项：

1. boot.img：32 MiB、`ANDROID!`、header v2、page 2048、各地址
2. kernel 段 gzip、MTK 追加 DTB 头 magic `0xD7B7AB1E`、独立段 FDT magic
3. `AVB0` / `AVBf` 存在
4. 解压后伪 Image 头：`code0` 是分支、`ARMd`@0x38、`text_offset=0x80000`、
   `image_size=0x257f000`（LK 硬性要求）
5. 平台 PCD 与文档事实一致（framebuffer `0x77440000`/1088/2280、
   MSDC `0x11230000`、TOP `0x11f50000`）
6. DSC/FDF 中必须包含的驱动（MmcHost/Vbus/UsbHost/MmcDiag/Xhci/…）
7. `startup.nsh` 已内嵌

注意：脚本只证明"镜像结构正确"，不证明"能启动"。

### 16. 下一步（记录自 2026-11-27 讨论）

1. **eMMC 验证**：刷 run #54 起看 `INFRA1 CG sta` 是否含 bit2（`MSDC0_CG`）与
   bit6（`MSDC0_SCK_CG`），以及 `CMD1 resp0` 是否非 0。
2. **若仍失败**：按主线警告"分频由假定源频推导"，改为**读回 LK 设的 `MSDC_CFG`
   分频值并沿用**，而不是自己按 400 MHz 计算。
3. **vmmc**：若怀疑 LDO_VEMC 未供电，通过 `MT6358PmicDxe` 的 wrapper 打开
   `LDO_VEMC`（需补 LDO 接口，当前 wrapper 只有 BUCK/LDO 部分能力）。
4. **USB 模拟串口（CDC-ACM）**：设计、事实与分阶段计划见
   [`docs/usb-acm.md`](usb-acm.md)。当前仓库只有骨架
   `Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771UsbAcmDxe/`：
   - 仅参与编译（在 DSC 中），**未放入 FDF**，因此不影响现有屏幕 Shell
   - 只实现了 IPPC 切设备角色 + 描述符表（VID/PID 沿用主线 `1d6b/0105`，
     串口 115200 8N1）
   - EP0 状态机、端点配置、PHY 设备模式初始化、数据通路均未实现，不得声称可用

### 17. OTG 自检（2026-11-27，刷机前）

用户提醒"很多设备默认 device，不开 host 就不会输出 5V"，逐项核对结论：

#### 17.1 角色设置：正确（host）

反编译原机 DTB（`Android_boot.img` 里的 `stock-dtbo-0.dtb`）：

```
usb3@11200000 | compatible = "mediatek,mt6771-mtu3"
usb3@11200000 | dr_mode = "otg"        ← 原厂 OTG 双角色，默认可能是 device
```

我们的 `Mt6771UsbHostDxe` 按厂商源码切 host：
```
IP_PW_CTRL1 &= ~SSUSB_IP_HOST_PDN        host IP 上电
IP_PW_CTRL2 |=  SSUSB_IP_DEV_PDN         device IP 下电
U2/U3 CTRL_0P &= ~(DIS|PDN); |= HOST_SEL(bit2)
U3D_U2PHYDTM1: FORCE_IDDIG=1, RG_IDDIG=0   ← 厂商 ssusb_otg_plug_in() 的语义 = host
U2_CTRL |= OTG_SEL(bit7)
```
厂商依据：`drivers/misc/mediatek/ssusb/mu3d/xhci-mtk.c`
`ssusb_otg_plug_in()` 清 `RG_IDDIG` 的注释是 "tell mac switch to host mode"。

#### 17.2 地址：全部对得上

| 项目 | 原机 DTB | UEFI |
|---|---|---|
| MAC | `0x11201000 + 0x3000` | `0x11201000` ✅ |
| IPPC | `0x11203e00 + 0x100` | `0x11203E00` ✅ |
| xHCI | `0x11200000 + 0x1000` | `0x11200000` ✅ |
| PHY | `0x11f40000 + 0x10000` | `0x11F40000` ✅ |

#### 17.3 真正的阻塞：VBUS 5V 没开

DTB 里的相关节点：
```
usb_boost_manager | compatible = "mediatek,usb_boost"   boost_period = 0x1e (30s)
otg_iddig         | compatible = "mediatek,usb_iddig_bi_eint"
extcon_usb        | compatible = "mediatek,extcon-usb"
charger           | pd_vbus_low_bound = 0x4c4b40 (5.0V)
```
注意：`usb_boost` 经查证是 **CPU/内存性能 boost**（插 USB 提频），不是 5V 升压，
名字误导。5V 升压在 MT6370 充电 IC 的 `mt6370_enable_otg()`
（`drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_charger.c:1903`），通过 I2C 写
`CHGCTRL1` 的 `OPA_MODE` 位实现。

我们的 `Mt6771VbusDxe` 试图走这条路，但 I2C6 传输未打通（scan 0），
所以 **5V 从未输出**。这也是"角色是 host 但设备不上电"的完整解释。

#### 17.4 低成本验证办法

刷机前可用万用表量 USB-C 口 VBUS 脚（A4/A9/B4/B9）：
- 刷 ACM 镜像（device 模式）：电脑供电时应为 5V（从外部进来）
- 刷 OTG 镜像（host 模式）：如果仍为 0V，证明升压未开，与上述结论一致
- 或者使用**自带供电的 OTG HUB**：绕过手机升压，只验证 D+/D- 数据通路

### 18. 设备与分区事实（2026-11-27）

设备：**OPPO A3 2018 / PADT00 / 4GB RAM + 128GB eMMC / Micro-USB**（不是 Type-C）。

`parted_info.txt`（实机 `parted -s /dev/mmcblk0 print` + `blkid`）给出的权威事实：

| 项 | 值 |
|---|---|
| eMMC 型号 | `MMC DV6DMB` |
| 容量 | 125 GB，GPT，512B logical/physical |
| boot 分区 | 分区 35，`mmcblk0p35`，33.6 MB，PARTUUID `c2635e15-61aa-454e-9c40-ebe1bdf19b9b` |
| cache 分区 | 分区 43，`mmcblk0p43`，453 MB，ext4，PARTUUID `85a5b02f-3773-18b3-1049-718cde95107e` |
| userdata 分区 | 分区 44，`mmcblk0p44`，8590 MB，ext4，PARTUUID `6fce83a6-5273-4748-1145-c205ebb4b8ad` |
| data 分区 | 分区 45，`mmcblk0p45`，110 GB，Btrfs/RAID 成员，PARTLABEL=data |
| lk | 分区 33，`mmcblk0p33` |
| odmdtbo | 分区 37，`mmcblk0p37` |

这些 PARTUUID 与 `Tools/mkbootimg-a3.py` 的 `BOOT_PARTUUID` 一致
（`c2635e15-...`），说明 boot 镜像 cmdline 是按这台机器的实测分区生成的。

日志分区计划（用户指定）：写 **cache 分区**（分区 43，453 MB，ext4）。
为了不破坏 ext4 元数据，建议只写分区尾部 4 MiB，之后在系统里取回。

### 19. eMMC 时钟修正

按主线警告"分频由假定源频推导，写错会 overclock"，驱动改为**采用 LK 已编程的
MSDC_CFG 分频**（`clk_div_bits=12`：CKDIV bits 8..19，CKMOD bits 20..21），
推算出实际 SCLK 并写入跟踪输出；不再按假定 400 MHz 自行重设时钟。

### 20. OTG 主机模式实测状态（2026-11-28，run #97）

经过多轮源码修正，屏幕诊断确认（照片为证）：

```
USB: DEVCTL 00000000 -> 00000001          SESSION 写入生效
USB: IPPC1=00000000 U2=00000280 U3=0000020C STS1=01000D0F STS2=00000001
USB: sys125=1 u3mac=0 u2mac=1
USB: xHCI after init CAP0=01100020 caplen=32
USB: U2CTRL 00000280 -> 00000284 host_sel=1
USB: register xHCI -> Success
XHCI CAPLEN=32 VER=0110 SLOTS=15 PORTS=1
XHCI DBOFF=00800 RTSOFF=00600 HCCAP=01400F99 HCSP2=00000004
XHCI PPC=1
PORTSC1=00000003 CCS=1 PED=1 PLS=0 PP=1 CSC=0     ← 端口完全正常(U0)
USBCMD=00000001 RS=1 USBSTS=00000010 HCH=0 CNR=0   ← 控制器运行中
MAC PWR=000060E1 DEVCTL=0000003D TIMING=0005497D
IPPC U2=00000284 U3=0000020C PWCTRL1=00000000
PHY DTM0=02000000 DTM1=00000220 ACR6=001004B2
Usb2Hc handles = 1 (bound)
dh -p UsbIo -> 6B: ConsoleIn SimpleTextInEx SimpleTextIn UsbIO DevicePath(USB)
```

DEVCTL=0x3D 解码：`SESSION=1, HOSTMODE=1, VBUS=11(有效), LS_DEV=1, FS_DEV=0`（**低速设备**）。
低速键盘正是 Linux xhci-mtk-sch.c 使用 `EP_BPKTS(1)` 的典型场景，与 MTK patch 匹配。
PHY DTM0 bit25=`RG_SIFSLV_USB20_PLL_STABLE`=1，PLL 已锁定。
TIMING=0x0005497D：`TIME_VALUE_1US=0x7D(125), SOFTCON_DELAY=0x49, CHOPPER=0x05`。

#### 已解决（全部有源码依据）
- VBUS 5V：风扇转。BQ24196 @ I2C 0x6B，REG00&=~BIT7, REG01|=BIT5
  （vendor oppo_bq24196.c:825）；I2C 需逐总线开 clock gate + 补 timing
- USB 时钟：INFRA0 bit8 / INFRA2 bit1 / APMIXED+0x20 bit4（clk-mt6771.c）
- 端口电源：**EDK2 XhciDxe 从不设置 PORTSC.PP**（Xhci.c 无 XHC_PORTSC_PP
  引用），本机 PPC=1 需软件置位，故我们自己在定时器里置 PP（bit9）
- PORTSC 正确位定义来自 XhciReg.h：PLS=bits5-8, PP=bit9
  （早期误读为 PLS=bits4-7/PP=bit8，导致漏掉端口未上电的根因）

#### 唯一剩余障碍
键盘枚举成功（ConsoleIn/UsbIO 句柄存在），但 Shell 敲键无反应，NUM LOCK 不亮。
注意：EDK2 UsbKbDxe 不发 LED SET_REPORT，所以灯不能作为判据，Shell 无输入才是真失败。
EP0 控制传输通（否则无法枚举），问题在 HID 中断端点（EP1 IN）数据读取。

待查方向（子智能体分析中）：
- XHCI_MTK_HOST quirk（Linux xhci-mem.c 保留 ep_ctx->reserved[]）
- XHCI_SPURIOUS_SUCCESS（xhci-ring.c:2856 忽略 short transfer 后的 spurious 成功）
- xhci_mtk_set_frame_interval / rxfifo_depth_set（HSCH_CFG1 @0x960）
- EDK2 XhciDxe 的 PollTimer 事件环轮询是否正常

### 21. HID 中断传输诊断判读指南（round 4）

run #105+ 的一次性回调在 3 秒时打印（全部是标准 xHCI MMIO 读，安全）：

```
USB: RTSOFF=00600 MFINDEX xxxx->xxxx ticking=N ERSTSZ=M ERDP=xxxxxxxx
```

判读：

| 观察 | 结论 | 下一步 |
|---|---|---|
| `ticking=0` | MFINDEX 微帧计数器不走 → 周期调度时钟关闭 → 中断端点永远不会被服务，这是根因 | 查 USB 125MHz/48MHz 参考时钟与 `USBCMD`/`CONFIG` 寄存器 |
| `ticking=1, ERSTSZ=0` | 帧计数器在走但 XhciDxe 没建事件环 | 查 NonDiscoverablePciDevice 的 `AllocateBuffer`/`Map` 是否失败 |
| `ticking=1, ERSTSZ>=1`，键盘仍无输入 | 事件环起来了、MTK 调度 patch 已注入；问题在 transfer event 或 DMA 数据可见性 | 读 ERDP 前后对比；验证 NonCoherent DMA flush；确认 `UsbKbDxe` callback 是否收到非零 DataLength |

补充事实（源码确认）：
- EDK2 控制传输走同步路径，不依赖 PollTimer；中断传输靠 1ms PollTimer
  (`XHC_ASYNC_TIMER_INTERVAL = EFI_TIMER_PERIOD_MILLISECONDS(1)`) 轮询事件环。
  因此"枚举成功"不能证明 PollTimer 在跑——这是本次诊断的核心目的。
- LS 键盘 `DEVCTL=0x3D`（LS_DEV=1），`bInterval` 通常 10ms，走
  `HighBitSet32(10)+3 = 6`，合法。2025 年的 bInterval clamp 修复只针对 HS/SS，
  与本机无关。
- `XhcInitializeEndpointContext/64` 是所有非控制端点上下文的唯一汇聚点
  （SetConfigCmd 3234/3325、SetInterface 3701/3907），MTK patch 覆盖全部路径。

### 22. Round 7 静态排除项

本轮通过源码核对排除了以下可能，避免后续重复排查：

1. **DMA coherence 正常**：EDK2 XhciDxe 的 `UsbHcMem.c` 用
   `PciIo->AllocateBuffer(EfiBootServicesData, ...)` 分配事件环与传输环
   （第 61/666/701 行），在 NonCoherent 模式下会正确做 cache 维护。
   NonDiscoverablePciDevice 声明了 `EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE`，
   64 位 DMA 也正常。

2. **patch 不会被 stuart_update 覆盖**：CI 日志确认顺序是
   setup/update（1849 行）→ apply MTK patch（1891 行）→ build，
   补丁在正确的时机注入。

3. **UsbKbDxe 数据长度检查对 LS 键盘正确**：`KeyboardHandler`
   (KeyBoard.c:1039-1052) 要求 `DataLength >= 8`，LS 键盘 HID boot report
   正好 8 字节。若 `Result != EFI_USB_NOERROR` 会走错误分支并不处理按键 —
   这仍是最可能的症状描述（见 §21 ERDP 诊断）。

4. **ConIn 硬编码 bug 已修**（run #107）：BeforeConsole 里
   `handles[1]` 越界读已删除，改用 USB HID class device path 热插拔匹配。

5. **MTK patch 覆盖所有端点初始化路径**：`XhcInitializeEndpointContext/64`
   是全部非控制端点的唯一汇聚点，patch 用全局 `text.replace` 覆盖两处。

结论：源码层面能静态确认的都已确认/修复。剩余必须靠 #109 的 `ticking` /
`ERSTSZ` / `ERDP` 三项运行时数据定位（见 §21）。

### 23. Round 10 — 实机照片复核与 MTK patch 精确性确认

重新逐行读取 00:03 实机照片，确认：

```
6B: ConsoleIn SimpleTextInEx SimpleTextIn UsbIO DevicePath(...)/USB(0x0,0x0)
PORTSC1=00000003 CCS=1 PED=1 PLS=0 PP=1 CSC=0
USBCMD=00000001 RS=1 USBSTS=00000010 HCH=0 CNR=0
MAC PWR=000060E1 DEVCTL=0000003D TIMING=0005497D
IPPC U2=00000284 U3=0000020C PWCTRL1=00000000
PHY DTM0=02000000 DTM1=00000220 ACR6=001004B2
```

- 键盘句柄**确实有 ConsoleIn/SimpleTextInEx/SimpleTextIn**，ConIn 绑定正常。
  因此 PcdConInConnectOnDemand 不是根因，已恢复 FALSE（避免 BDS 跳过初始
  ConIn 连接的风险）。
- DEVCTL=0x3D = LS_DEV(bit5)，低速键盘直连 root hub。

MTK patch 精确性再确认（厂商 drivers/usb/host/xhci-mtk-sch.c）：
```
need_bw_sch(): LS/FS 且 !has_tt → return false（root hub 直接调度）
xhci_mtk_check_bandwidth(): 此时只需 ep_ctx->reserved[0] |= EP_BPKTS(1)
```
我们的 patch 写 `InputContext->EP[Dci-1].RsvdZ5 = 1`（RsvdZ5 即
reserved[0]），与厂商逻辑**完全一致**；reserved[1] 无需设置。

RELEASE DEBUG 现在路由到 FrameBufferSerialPortLib（PcdDebugPrintErrorLevel
=0x80000000 即 DEBUG_ERROR）。XhciDxe 的
"Transfer Default Error Occur! Completecode = 0x%x" 是 DEBUG_ERROR，
若中断传输报错，下一版屏幕会直接显示 completion code。

**当前候选镜像：run #111**。所有源码层面能做的已做完，剩余依赖实测：
- Shell 敲键有无反应
- ERDP 双采样是否前进
- 屏幕是否出现 DEBUG_ERROR completion code

### 24. Round 14 — 实测清单与 SET_PROTOCOL 排除

检查了 UsbKbDxe 的初始化路径（KeyBoard.c:825-871）：
- `UsbGetConfiguration` → `UsbSetConfiguration` → `UsbSetProtocolRequest(BOOT_PROTOCOL)`
- `UsbSetProtocolRequest` 的返回值未被检查（EDK2 上游写法，广泛使用，暂不改）
- 枚举成功已证明 EP0 控制传输可用，因此 SET_PROTOCOL 失败的可能性低

**当前 build（run #114）屏幕上应能看到的完整诊断集：**
```
USB: host init (upstream mtu3_host sequence)
USB: DEVCTL xxxxxxxx -> xxxxxxxx
USB: IPPC1=... U2=... U3=... STS1=... STS2=... sys125=... u2mac=...
USB: U2CTRL xxxxxxxx -> xxxxxxxx host_sel=1
USB: register xHCI -> Success
XHCI CAPLEN=32 VER=0110 SLOTS=15 PORTS=1 / PPC=1
PORTSC1=... CCS=? PED=? PLS=? PP=1 CSC=?
USBCMD=... RS=1 USBSTS=... HCH=0 CNR=0
MAC PWR=... DEVCTL=... TIMING=...
Usb2Hc handles = 1 (bound)
USB: port power done, PORTSC1=... CCS=1 PED=1 PP=1
USB: RTSOFF=00600 MFINDEX xxxx->xxxx ticking=? ERSTSZ=? ERDP xxx->xxx
USB: PHY DMON1=xxxxxxxx AVALID=? HST_DISCON=? LINE=?
```

**下次刷机必须采集的三行（一次性、安全 MMIO 读）：**
1. `USB: RTSOFF=... MFINDEX ... ticking=? ERSTSZ=? ERDP xxx->xxx`
2. `USB: PHY DMON1=... AVALID=? HST_DISCON=? LINE=?`
3. `PORTSC1=... CCS=? PED=? PLS=? PP=?`

判读矩阵：
| AVALID | HST_DISCON | ticking | ERDP | 结论 |
|---|---|---|---|---|
| 0 或 DISCON=1 | - | - | - | 模拟层没看到键盘 → 接线/VBUS/PHY |
| 1 | 0 | 0 | - | 周期时钟停 → 查参考时钟 |
| 1 | 0 | 1 | 不变 | 无 transfer event → MTK 调度/事件环 |
| 1 | 0 | 1 | 前进 | event 有了 → 查 DMA/URB 匹配 |

### 25. Round 16 — 最终静态结论

本轮核对 UsbKbDxe 的 URB 提交与 HID 请求：
- `KeyBoard.c:382` 的 `UsbAsyncInterruptTransfer(..., DataLength=PacketSize=8, ...)`
  参数正确，LS 键盘 boot report 为 8 字节。
- `UsbSetProtocolRequest(BOOT_PROTOCOL)` 未检查返回值是 EDK2 上游写法；
  枚举成功说明 EP0 控制传输正常，SET_PROTOCOL 失败可能性低。
- 在 XhciDxe 回调里用 `gST->ConOut` 输出不安全：虽然 callback 调用点在
  `RestoreTPL()` 之后，但 UsbBus 层可能仍有 TPL 约束，且之前尝试这类诊断
  与重启现象时间相关，已撤销。
- MTK EP_BPKTS patch 只设 `reserved[0]=1`、不设 `reserved[1]`，与厂商
  `xhci-mtk-sch.c` 的直连 LS/FS 分支完全一致；设置 reserved[1] 反而错误。

**结论：源码层面所有能确认的修复都已完成。** 下一步必须依赖 #114/#115 的
PHY DMON1 / MFINDEX / ERSTSZ / ERDP 实测数据（判读矩阵见 §24）。

## 26. eMMC 打通（2026-11-28，build #152）

`map -r` 已能列出全部 47 个 GPT 分区。完整根因链，全部有源码或实机数据依据：

| # | 问题 | 依据 | 修复 |
|---|---|---|---|
| 1 | CMD1 报 CRC、OCR 无效 | EDK2 `MmcIdentification.c` 在 CMD0/CMD1 **之前**不调 `SetIos`，卡跑在 LK 留下的 50MHz | init 里主动编程 260kHz（厂商 `sd.c:4373`）|
| 2 | 时钟被门控 | 厂商 `sd.c:4373-4375` 结束时 CKPDN **清零** | `MsdcSetClock` 末尾改为清 CKPDN |
| 3 | 每条命令前软复位 | `mtk-sd.c:1308` 只在 FIFO 非空时复位 | 改为按需复位 |
| 4 | CMD16/CMD12 超时且 INT=0 | `mtk-sd.c:1262 msdc_cmd_is_ready()` 要求先等 CMDBUSY/SDCBUSY | 加同样等待 |
| 5 | 数据读 CRC 错 | EDK2 写 `BUS_WIDTH=6`（8-bit **DDR**），控制器只有 SDR | 强制 EXT_CSD `DEVICE_TYPE=0`（`MmcIdentification.c:271` 提前返回）|
| 6 | 数据读短 | 单次排空 FIFO 模式不可靠 | 按 `mtk-sd.c:1306` 只用 RXCNT 当"非空"标志，逐字读到齐 |
| 7 | trace 里 resp0 恒为 0 | trace 在读取响应寄存器**之前**打印 | 先读响应再打印 |
| 8 | trace 里 CFG/SDC_CFG 被截断 | 用 `MmioRead16` 读 32 位寄存器 | 改 `MmioRead32` |
| 9 | 响应缓冲区越界写 12 字节 | `ReceiveResponse` 无条件拷 16 字节，OCR 调用方只有 4 字节 | 只有 R2（CID/CSD）拷 16 字节 |
| 10 | BlockIo 容量为 0 | EDK2 MmcDxe 媒体字段不可靠 | 驱动自己识别 + 用 EXT_CSD `SEC_COUNT` 发布自己的 BlockIo |
| 11 | 分区扫描卡住 | BlockIo 句柄缺少 DevicePath | 一并安装 VenHw 设备路径节点 |
| 12 | 日志刷屏/重启 | TPL_CALLBACK 大量 ConOut | `MMC_VERBOSE_TRACE=0` / `MMC_DIAG_VERBOSE=0` 默认静音 |

关键实测数据：

```
CMD1  resp0=40FF8080            OCR 有效
CMD13 resp0=00000900            Tran 状态、READY_FOR_DATA
EXT_CSD SEC_COUNT=0x0EBF6000    247,431,168 扇区 ≈ 126 GB
MBR   sig=55AA，mbr 项 EE FF FF FF 01 00 00 00 FF 5F 8F 0E
```

驱动现在自己发布 `EFI_BLOCK_IO_PROTOCOL`（容量 244,277,248 扇区），
`MmcDxe` 已从 FV 移除，避免同一控制器出现两个 BlockIo。
