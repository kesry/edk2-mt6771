# Issue 草稿：请求 fnOS ARM 内核开启 MediaTek（MTK）平台支持

> 用法：直接复制下面 `---` 之间的内容提交到 https://github.com/ophub/fnnas/issues
> 或在 #21 里以评论形式发出（末尾附了精简版）。

---

## 标题

请求内核开启 MediaTek 平台支持（`ARCH_MEDIATEK` / `MMC_MTK` / `COMMON_CLK_MT6779`）

## 正文

### 背景

fnOS ARM 版当前内核 `6.18.18.c1193-trim` 的配置里，已经开启了多家 ARM SoC 平台：

```
CONFIG_ARCH_CIX=y          # 此芯
CONFIG_ARCH_HISI=y         # 海思
CONFIG_ARCH_QCOM=y         # 高通
CONFIG_ARCH_ROCKCHIP=y     # 瑞芯微
# CONFIG_ARCH_MEDIATEK is not set    ← 唯独联发科没有开
# CONFIG_MMC_MTK is not set
```

也就是说这是一个**多平台通用内核**，MediaTek 只是没有被勾选，并不是存在技术障碍。

### 请求

希望在后续内核构建中开启以下选项（均为上游主线驱动，**无需新增代码**）：

```
CONFIG_ARCH_MEDIATEK=y
CONFIG_MTK_TIMER=y
CONFIG_MTK_CPUX_TIMER=y
CONFIG_COMMON_CLK_MT6779=y
CONFIG_PINCTRL_MT6779=y
CONFIG_MMC_MTK=y
```

如需 USB / 显示 / I2C 支持，可再加：

```
CONFIG_MTK_IOMMU=m
CONFIG_MTK_CMDQ_MBOX=m
CONFIG_MTK_SMI=m
CONFIG_PHY_MTK_TPHY=m
CONFIG_USB_MTU3=m
CONFIG_DRM_MEDIATEK=m
CONFIG_I2C_MT65XX=m
```

作为参考，**Debian arm64 官方配置这些本来就是开的**：

```
CONFIG_ARCH_MEDIATEK=y
CONFIG_PINCTRL_MT6779=y
CONFIG_MMC_MTK=m
CONFIG_COMMON_CLK_MT6779=m
```

### 这样做能带来什么

MediaTek 设备在闲置手机 / DIY NAS 领域保有量极大（例如 MT6771/MT6765/MT6768 等）。
开启后，这类设备可以直接使用官方内核，而不需要第三方自行编译内核，也就能保留
官方的文件系统权限补丁等私有能力。

### 我们可以提供

我们正在为一台 **OPPO A3（PADT00，MT6771 / Helio P60，4GB+128GB eMMC）** 做完整的
ARM64 移植，目前状态：

- 主线 Linux 内核可启动，eMMC / 显示 / 触摸 / WiFi 均已工作
- 自研 UEFI 固件（EDK2）已完成：eMMC BlockIo、GPT 47 分区、GOP、USB 键盘鼠标
- 可提供该机型的 **DTB**（`mt6771-oppo-a3`，已在主线仓库维护）
- 可提供 **UEFI 启动链**（ESP + GRUB），用于验证新内核
- **愿意承担测试工作**，并按需反馈启动日志

### 说明

- 该机型的硬件描述使用 **DTB**（不是 ACPI）。由于内核已经为其他 SoC 开启了
  `CONFIG_OF`，新增 `ARCH_MEDIATEK` 后 DT 启动路径可直接复用，不需要额外适配框架。
- 我们不需要飞牛开放任何私有补丁；只要内核里带上 MTK 平台与 eMMC/时钟驱动，
  剩下的硬件适配由我们通过 DTB 完成。

感谢！

---

## 精简版（可直接作为 #21 的评论）

> 想申请一件事：官方内核 `6.18.18.c1193-trim` 已经开了 `ARCH_CIX/HISI/QCOM/ROCKCHIP`，
> 但 `CONFIG_ARCH_MEDIATEK is not set`、`CONFIG_MMC_MTK is not set`。
>
> 如果后续构建能把 MTK 打开（都是上游现成驱动，不用改代码）：
> ```
> CONFIG_ARCH_MEDIATEK=y
> CONFIG_MTK_TIMER=y
> CONFIG_COMMON_CLK_MT6779=y
> CONFIG_PINCTRL_MT6779=y
> CONFIG_MMC_MTK=y
> ```
> 那么 MT6771（OPPO A3 等）一类设备就能直接用官方内核，不用第三方自行编译。
> Debian arm64 官方配置里这几项本来就是开的，可作为参考。
>
> 我们在做 MT6771 的完整移植（已能启动主线内核 + 自研 UEFI，eMMC/显示/WiFi 可用），
> 可以提供 DTB 和 UEFI 启动链，并愿意负责测试与反馈日志。
