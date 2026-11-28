# 为 fnOS ARM 内核开启 MediaTek（MTK）平台支持 — 配置清单

本文是一份可直接交给内核维护者使用的配置清单，用于让 fnOS ARM 版内核
支持联发科（MediaTek）平台设备。

## 1. 现状与依据

fnOS ARM 版内核 `6.18.18.c1193-trim` 的配置文件（`/boot/config-6.18.18.c1193-trim`）
中，已经开启多家 ARM SoC 平台：

```text
CONFIG_ARCH_CIX=y
CONFIG_ARCH_HISI=y
CONFIG_ARCH_QCOM=y
CONFIG_ARCH_ROCKCHIP=y
# CONFIG_ARCH_MEDIATEK is not set
# CONFIG_MMC_MTK is not set
```

说明这是一个多平台通用内核，联发科平台只是没有被勾选。下文所有选项均为
上游主线（upstream）驱动，**不需要新增任何代码**。

## 2. 请求 1：启动必需（建议 =y）

```text
CONFIG_ARCH_MEDIATEK=y
CONFIG_MTK_TIMER=y
CONFIG_MTK_CPUX_TIMER=y
CONFIG_COMMON_CLK_MT6779=y
CONFIG_PINCTRL_MT6779=y
CONFIG_MMC_MTK=y
CONFIG_SERIAL_8250_MT6577=y
```

| 选项 | 作用 | 缺失后果 |
| --- | --- | --- |
| ARCH_MEDIATEK | MTK 平台总开关 | 所有 MTK 驱动不可用 |
| MTK_TIMER / MTK_CPUX_TIMER | 系统定时器 | 无法启动 |
| COMMON_CLK_MT6779 | 时钟控制器 | 无时钟，eMMC/USB 全部不可用 |
| PINCTRL_MT6779 | 引脚复用 | 外设无法出信号 |
| MMC_MTK | eMMC/SD 控制器 | 找不到存储，无法挂载 root |
| SERIAL_8250_MT6577 | 串口控制台 | 启动日志不可见，无法调试 |

## 3. 请求 2：供电与 PMIC（MT6771 常见搭配 MT6358）

```text
CONFIG_MTK_PMIC_WRAP=m
CONFIG_MTK_SCPSYS=m
CONFIG_REGULATOR_MT6358=m
CONFIG_COMMON_CLK_MT6358=y
```

说明：eMMC 需要 `vmmc` / `vqmmc` 供电，缺少 PMIC regulator 时会卡在 eMMC 枚举阶段。
若目标机型使用其他 PMIC（如 MT6357 / MT6360），替换为对应型号即可。

## 4. 请求 3：外设完整支持（建议 =m，按需加载，不影响常驻内存）

```text
CONFIG_MTK_IOMMU=m
CONFIG_MTK_CMDQ_MBOX=m
CONFIG_MTK_SMI=m
CONFIG_PHY_MTK_TPHY=m
CONFIG_USB_MTU3=m
CONFIG_DRM_MEDIATEK=m
CONFIG_I2C_MT65XX=m
CONFIG_SPI_MT65XX=m
```

| 选项 | 作用 |
| --- | --- |
| MTK_IOMMU / MTK_SMI / MTK_CMDQ_MBOX | 多媒体与 DMA 基础设施 |
| PHY_MTK_TPHY / USB_MTU3 | USB（OTG 键鼠、U 盘、网络共享） |
| DRM_MEDIATEK | 显示（DSI 面板） |
| I2C_MT65XX | 触摸、传感器、充电芯片 |
| SPI_MT65XX | SPI 外设 |

## 5. 参考：Debian arm64 官方配置

Debian 的 arm64 内核这些项本来就是开启的，可作为可行性的旁证：

```text
CONFIG_ARCH_MEDIATEK=y
CONFIG_PINCTRL_MT6779=y
CONFIG_MMC_MTK=m
CONFIG_COMMON_CLK_MT6779=m
```

## 6. 可直接应用的配置片段

在内核源码目录执行（`$O` 为构建输出目录）：

```sh
K=.            # 内核源码根目录
O=$K          # 或独立的输出目录

./scripts/config --file "$O/.config" \
  -e ARCH_MEDIATEK -e MTK_TIMER -e MTK_CPUX_TIMER \
  -e COMMON_CLK_MT6779 -e PINCTRL_MT6779 -e MMC_MTK -e SERIAL_8250_MT6577 \
  -m MTK_PMIC_WRAP -m MTK_SCPSYS -m REGULATOR_MT6358 -e COMMON_CLK_MT6358 \
  -m MTK_IOMMU -m MTK_CMDQ_MBOX -m MTK_SMI \
  -m PHY_MTK_TPHY -m USB_MTU3 -m DRM_MEDIATEK -m I2C_MT65XX -m SPI_MT65XX

make O="$O" ARCH=arm64 olddefconfig
```

对应的 defconfig 片段：

```text
# --- MediaTek platform (boot essentials) ---
CONFIG_ARCH_MEDIATEK=y
CONFIG_MTK_TIMER=y
CONFIG_MTK_CPUX_TIMER=y
CONFIG_COMMON_CLK_MT6779=y
CONFIG_PINCTRL_MT6779=y
CONFIG_MMC_MTK=y
CONFIG_SERIAL_8250_MT6577=y

# --- PMIC / regulators ---
CONFIG_MTK_PMIC_WRAP=m
CONFIG_MTK_SCPSYS=m
CONFIG_REGULATOR_MT6358=m
CONFIG_COMMON_CLK_MT6358=y

# --- peripherals ---
CONFIG_MTK_IOMMU=m
CONFIG_MTK_CMDQ_MBOX=m
CONFIG_MTK_SMI=m
CONFIG_PHY_MTK_TPHY=m
CONFIG_USB_MTU3=m
CONFIG_DRM_MEDIATEK=m
CONFIG_I2C_MT65XX=m
CONFIG_SPI_MT65XX=m
```

## 7. 为什么值得做

- 联发科设备在闲置手机、DIY NAS、电视盒子领域保有量很大
  （MT6771 / MT6765 / MT6768 / MT6833 等）
- 开启后这些设备可使用**官方内核**，无需第三方自行编译
- 从而保留官方内核的私有补丁（例如文件系统权限相关）——这是目前社区适配
  最大的痛点：自行编译的内核会丢失这些能力

## 8. 测试与验证

我们可提供一台 **OPPO A3（PADT00，MT6771 / Helio P60，4GB + 128GB eMMC）**
用于验证，当前状态：

- 主线 Linux 内核可启动，eMMC / 显示 / 触摸 / WiFi 已工作
- 自研 UEFI 固件（EDK2）：eMMC BlockIo、GPT（47 分区）、GOP、USB 键鼠
- 可提供该机型 DTB 与 UEFI 启动链（ESP + GRUB）
- 愿意承担测试并反馈完整启动日志

验证顺序：

| 阶段 | 观察点 | 说明 |
| --- | --- | --- |
| 1 | 串口输出 `Booting Linux ...` | 平台、时钟、DTB 生效 |
| 2 | `mmc0: new HS400 MMC card` | eMMC 驱动与供电正常 |
| 3 | systemd 启动或进入 initramfs | 分区与 initrd 匹配 |
| 4 | 进入系统 | 全部就绪 |

## 9. 备注

- 该机型使用 **DTB**（Device Tree）描述硬件，不使用 ACPI。内核已为其他 SoC
  开启 `CONFIG_OF`，新增 `ARCH_MEDIATEK` 后可复用同一 DT 启动路径。
- 无需开放任何私有补丁；MTK 平台与上述驱动就位后，剩余硬件适配通过 DTB 完成。
- 若担心影响现有平台的镜像体积，可先只加第 2 节的 7 项，其余按 `=m` 按需加载。
