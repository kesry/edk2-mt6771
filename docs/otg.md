# OPPO A3 (PADT00) UEFI OTG 主机模式

本文档记录 OPPO A3 2018（MT6771 / PADT00，4GB+128GB，Micro-USB）在 UEFI
中实现 USB OTG 主机模式的完整方案与移植依据。

**状态：已实机验证可用 —— USB 键盘可在 UEFI Shell 中输入。**

首次验证通过：2026-11-28，固件 run #122（commit `2ab1b44`）。

---

## 1. 目标与结果

目标：在没有 Linux 内核驱动模型的情况下，让 UEFI 直接使用 USB 设备
（键盘、最终是 U 盘/ISO）。

结果：

- ✅ USB 键盘在 Shell 中可输入
- ✅ VBUS 5V 由充电芯片升压输出
- ✅ xHCI 控制器初始化、端口上电、设备枚举正常
- ⚠️ xHCI 的 HID **中断 IN** 端点在 EDK2 下无法送达按键数据
- ✅ 通过 **EP0 控制传输 GET_REPORT 轮询** 绕开中断端点，实现可用的键盘

---

## 2. 硬件事实（全部来自实机/源码，非猜测）

### 2.1 设备与 USB 口

| 项 | 值 |
|---|---|
| 设备 | OPPO A3 2018 / PADT00 / 4GB+128GB |
| SoC | MediaTek MT6771（helio P60） |
| USB 口 | **Micro-USB**（不是 Type-C） |
| 角色判断 | Micro-USB ID 脚（OTG 线接地） |
| VBUS 5V | 外部充电芯片 **BQ24196**（不是 MT6370、不是 PMIC 直通）|

> 关键纠正：早期把 VBUS 猜成 MT6370/Type-C TCPC，走错了方向。
> Micro-USB 没有 CC 引脚，也不需要 Type-C 协商。

### 2.2 寄存器地址（来自 Android 10 原机 DTB）

| 块 | 地址 |
|---|---|
| xHCI | `0x11200000` (size `0x1000`) |
| MAC | `0x11201000` (size `0x3000`) |
| IPPC | `0x11203E00` (size `0x100`) |
| USB PHY | `0x11F40000` |

原机 DTB 关键节点：

```
usb3@11200000   compatible = "mediatek,mt6771-mtu3"   dr_mode = "otg"
usb3_xhci       compatible = "mediatek,mt67xx-xhci"
usb0phy@11f40000 compatible = "mediatek,mt6771-phy"
```

### 2.3 充电芯片与 I2C（来自 Android 8 厂商源码）

vendor `drivers/misc/mediatek/power/bq24196.c` / `oppo_bq24196.c`：

```
I2C 7-bit 地址 : 0x6B   (write 0xD6 / read 0xD7)
OTG enable     : REG00 &= ~BIT7  (SUSPEND_MODE_DISABLE)
                 REG01 |=  BIT5  (OTG_ENABLE)
```

vendor `drivers/misc/mediatek/usb20/mt6763/usb20_host.c`：

```
OPPO 的 VENDOR_EDIT 分支在 host 使能时调用 bq24196_otg_enable()
```

### 2.4 I2C 总线与时钟（来自 Android 10 DTB + vendor clk-mt6771.c）

MT6771 有 12 条 I2C 控制器，每条有独立的 INFRA_AO 时钟门控：

```
I2C0-3 → INFRA0 (+0x80) bits 11,12,13,14
I2C4   → INFRA2 (+0xA4) bit 7
I2C5   → INFRA2 (+0xA4) bit 18
I2C6   → INFRA3 (+0xC0) bit 6
I2C7-8 → INFRA3 (+0xC0) bits 22,23
```

USB 控制器时钟门控：

```
INFRA0 bit8  (ICUSB)
INFRA2 bit1  (USB)
APMIXED 0x1000C000+0x20 bit4  (SSUSB26M，反转逻辑：写 1 = 使能)
```

---

## 3. 完整 bring-up 链路（按启动顺序）

### 3.1 VBUS 阶段 —— `Mt6771VbusDxe`

1. 打开 I2C 时钟门控（逐总线）
2. 对每条总线补 timing（`CLOCK_DIV`/`TIMING` 为 0 时）
3. 在每条总线探测 BQ24196 @ 0x6B（读 REG00 有 ACK）
4. 执行厂商确切的 OTG 序列：
   ```
   REG00 &= ~BIT7    // SUSPEND_MODE_DISABLE
   REG01 |=  BIT5    // OTG_ENABLE → 输出 5V
   ```
5. 实机验证：插入 USB 风扇，风扇转动 ✅

### 3.2 USB 控制器阶段 —— `Mt6771UsbHostDxe`

1. 打开 SoC 侧 USB 时钟（INFRA0/INFRA2/APMIXED）
2. IP 软复位、host IP 上电、**保持 device IP 供电**
   - 关键：上游 mtu3 为省电会关 device IP，但 MT6771 厂商 host 路径**从不**
     设置 `SSUSB_IP_DEV_PDN`；USB2 CSR 块在 device IP 里，关掉后时序寄存器
     读回 0，写入无效
3. 端口切换：U2/U3 清 `PDN|DIS`，置 `HOST_SEL(bit2)`；PHY IDDIG 强制 host
4. 补 USB2 MAC 时序基准：`U3D_USB20_TIMING_PARAMETER` bits7:0 = 63
5. PHY host 模式（厂商 `usb_phy_recover()` 完整序列）
6. 最后再次强制 `HOST_SEL|OTG_SEL`（防止中途被清）
7. 注册 xHCI 为 NonDiscoverablePciDevice（DMA: NonCoherent）

### 3.3 端口上电 —— 一次性定时器

EDK2 的 `XhciDxe` 从不设置 `PORTSC.PP`（`Xhci.c` 无 `XHC_PORTSC_PP` 引用），
但本机 `HCCPARAMS1.PPC=1`（端口电源必须软件控制）。

`Mt6771UsbHostDxe` 的一次性定时器在 XhciDxe 绑定后：

```
PORTSC1.PP = 1   (bit9)
```

上电后实机读回：

```
PORTSC1 = 0x00000003   CCS=1 PED=1 PLS=0(U0) PP=1
USBCMD  = 0x00000001   RS=1
USBSTS  = 0x00000010   HCH=0 CNR=0
```

### 3.4 MTK 周期端点调度 —— 构建时 patch

Linux 的 `XHCI_MTK_HOST` quirk 对直连 FS/LS 中断端点写：

```
ep_ctx->reserved[0] = EP_BPKTS(1)
```

EDK2 把同样字段叫 `RsvdZ5/RsvdZ6` 但**从不写入**。

`Patches/apply_edk2_mtk_patches.py` 在构建时幂等注入：
```
InputContext->EP[Dci-1].RsvdZ5 = 1;   // MTK_EP_BPKTS(1)
```
覆盖 32/64 字节两个端点上下文初始化路径。

> 厂商 Android 8 `xhci-mtk-sch.c:need_bw_sch()` 证实：LS/FS 且无 TT 时只设
> `reserved[0]=1`，`reserved[1]` 保持 0。我们的 patch 逐位一致。

### 3.5 键盘输入 —— EP0 兜底驱动（关键的最终方案）

**问题**：尽管键盘枚举成功（有 `ConsoleIn/SimpleTextIn/UsbIO` 句柄），
xHCI 的 HID 中断 IN 端点在 EDK2 下始终读不到按键数据。

**方案**：`A3UsbKbDxe` 完全绕开中断端点，用 EP0 控制传输轮询：

```
每 10ms:
  找 HID/boot/keyboard 接口 (3/1/1) —— 在回调中动态查找，适配晚到的键盘
  GET_REPORT over EP0:
    bmRequestType = 0xA1 (class/interface/IN)
    bRequest      = 0x01 (GET_REPORT)
    wValue        = 0x0100 (Input report)
    wIndex        = 键盘真实的 InterfaceNumber
    length        = 8
  → HID usage 转 EFI_INPUT_KEY (含 Shift)
  → 入队 + SignalEvent
  → 安装 gEfiConsoleInDeviceGuid，由 ConSplitter 聚合进 gST->ConIn
  → Shell 可读
```

---

## 4. 文件清单

| 文件 | 作用 |
|---|---|
| `Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771VbusDxe/` | BQ24196 VBUS 升压 + I2C 全总线探测 |
| `Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771UsbHostDxe/` | USB 时钟/IPPC/端口上电/MTK 诊断 |
| `Silicon/MediaTek/MediaTekPkg/Drivers/A3UsbKbDxe/` | **EP0 GET_REPORT 键盘（最终可用方案）** |
| `Silicon/MediaTek/MediaTekPkg/Drivers/A3UsbProbeDxe/` | EP0 GET_REPORT 诊断探针（15 秒） |
| `Patches/apply_edk2_mtk_patches.py` | 构建时给 EDK2 XhciDxe 打 MTK 调度 patch |
| `Tools/verify_firmware.py` | 60+ 项编译期不变量检查 |
| `Platform/OPPO/A3Pkg/a3.fdf` / `a3.dsc` | OTG 镜像构建配置 |

---

## 5. 验证方法

### 5.1 编译

```
./build.sh -t RELEASE OPPO/A3Pkg
```

CI 每次 push 自动构建 OTG + ACM 双镜像，验证 60+ 项不变量。

### 5.2 实机验证

1. 刷 `boot-uefi-otg.img` 到 boot 分区
2. 用 **Micro-USB OTG 线**（ID 脚接地）连接 USB 键盘
3. 屏幕应出现：
   ```
   USB: register xHCI -> Success
   A3Kb: EP0 keyboard active
   USB: port power done, PORTSC1=... CCS=1 PED=1 PP=1
   ```
4. 在 Shell 敲字母 —— 屏幕应回显

### 5.3 诊断行（安全 MMIO 读，一次性）

```
USB: RTSOFF=00600 MFINDEX xxxx->xxxx ticking=? ERSTSZ=? ERDP xxx->xxx
USB: PHY DMON1=xxxxxxxx AVALID=? HST_DISCON=? LINE=?
```

判读：

| 现象 | 结论 |
|---|---|
| `AVALID=0` / `HST_DISCON=1` | 模拟层没看到设备（接线/VBUS/PHY）|
| `ticking=0` | 周期调度时钟停 |
| `ERSTSZ=0` | 事件环没配置 |
| `ERDP` 不变 | 无 transfer event |

---

## 6. 已知限制与后续

- **中断端点仍未修复**：xHCI HID 中断 IN 无数据，根因未最终确定
  （候选：EDK2 缺 MTK quirk 完整实现 / 事件环 / 周期调度）。键盘已通过 EP0 兜底。
- **U 盘/ISO**：`UsbMassStorageDxe` 已打包，但走的是批量端点（Bulk），
  与中断端点不同路径。下一步可实测 U 盘，若可用则 ISO 启动路径打通。
- **ACM 设备模式**：双镜像中的 `boot-uefi-acm.img` 是独立分支，见 `usb-acm.md`。
- **eMMC**：MSDC 驱动已改为采用 LK 分频，见 `porting.md` §19。

---

## 7. 关键教训（避免重复踩坑）

1. **不猜参数**：所有地址/位/序列必须有源码或实机 DTB 依据
2. **LK 已配置的不要重置**：I2C soft-reset、MSDC 分频都踩过坑
3. **MTK xHCI 与上游不同**：`HOST_SEL` 位定义、`DEV_PDN` 处理都不同
4. **RELEASE 的 DEBUG 走未接线 UART**：诊断必须走 ConOut 或安全 MMIO 读
5. **TPL 安全**：不要在 TPL_NOTIFY 回调里调 ConOut / 分配内存
6. **晚到设备**：VBUS 后期上电，驱动入口只查一次会导致漏设备
7. **LCD 与 DEBUG 争抢 framebuffer**：FrameBufferSerialPortLib 与 ConOut 会互相覆盖

---

## 8. 从 eMMC 引导 Linux（下一步，2026-11-28）

用户没有 USB-A 扩展坞（只有 Type-C），所以 U 盘启动不可行。改走 eMMC。

### 8.1 关键事实（来自实机 parted_info.txt）

```
分区 41 vendor  : 981MB  ~ 2374MB  TYPE="swap"  (无文件系统)
分区 42 system  : 2374MB ~ 5931MB  TYPE="swap"  (无文件系统)
```

这两个分区被标记为 **swap（原始分区，无 ext4 文件系统）**。而我们的 UEFI
固件只包含 FAT 文件系统驱动（`FatPkg/EnhancedFatDxe`），**没有 EXT4 驱动**。

因此正确的加载方式是：**不依赖文件系统，按 LBA 直接读取原始字节**：

```
内核 Image  → dd 到 system 分区起始扇区（LBA 0，即分区偏移 0）
dtb         → dd 到 vendor 分区起始扇区
ramdisk     → dd 到 vendor 分区后续扇区（可选）
UEFI       → 用 BlockIo 按 LBA 读入内存 → 校验 ARM64 Image 头 → 跳转
```

### 8.2 当前阻塞点

eMMC 驱动本身尚未在实机确认为可用。早期日志显示 "CMD1 CRC error / resp0=0"，
但那是在 R3 响应修复（`7c5af9e`）和 LK 分频方案（`b693837`）之前。

本轮（run #123）已重新打开 eMMC 屏幕 trace（`MMC_SCREEN_TRACE=TRUE`），
下次刷机屏幕上会显示 CMD0/CMD1/CMD2/CMD3 的真实响应，据此判断：
- 若 CMD1 返回有效 OCR 且后续识别成功 → 可以直接做 BlockIo 读取
- 若仍失败 → 先修 MSDC，再谈引导

### 8.3 引导驱动设计（待 eMMC 可用后实施）

新增 `A3EmmcBootDxe`：
1. 等待 BlockIo 句柄（eMMC GPT 出现）
2. 按分区 PARTUUID 找到 system / vendor
3. 从分区起始 LBA 读取 Linux Image 到固定内存地址
4. 校验 ARM64 Image header（code0 分支 + `text_offset`）
5. 加载 dtb/ramdisk，构造 boot params，跳转内核

注意：BootShim 的头（`text_offset=0x80000`, `image_size=0x257f000`）是给 LK 用的；
UEFI 直接跳转时需要使用内核真实头，不能照搬 BootShim 的伪装值。
