# USB 模拟串口（CDC-ACM）把 UEFI Shell 接到电脑

目标：手机在 UEFI 下把自己枚举成 USB CDC-ACM 串口设备（`/dev/ttyACM0` 或 COMx），
在电脑上用串口终端直接操作 UEFI Shell。

本文只记录**有出处的事实**；未实现的部分明确标注 TODO，不含猜测值。

## 1. 为什么选设备模式而不是主机模式

| 方案 | VBUS 5V 需求 | 结论 |
|---|---|---|
| USB 主机（xHCI + 键鼠/U 盘） | 需要 MT6370 OTG 升压 | 卡在 I2C，未打通 |
| **USB 设备（CDC-ACM）** | **由电脑供电，手机不需要升压** | 绕开 VBUS 问题，可行 |

## 2. 事实来源

### 2.1 控制器

| 事实 | 出处 |
|---|---|
| UDC 名 = `11201000.usb3`，控制器基址 `0x11201000` | `mainline-linux-mt6771/docs/a3-usbnet.md:260`；原机 DTB `usb3@11200000` 的 `mac` reg = `<0x11201000 0x3000>` |
| IPPC 基址 `0x11203E00` | 原机 DTB `usb3@11200000` `reg-names="mac","ippc"` |
| xHCI（主机侧）`0x11200000` | 原机 DTB `usb3_xhci@11200000` |
| USB PHY `0x11F40000` | 原机 DTB `usb0phy@11f40000` |

### 2.2 设备模式寄存器（厂商 `drivers/misc/mediatek/mu3d/hal/ssusb_dev_c_header.h`）

偏移均相对 `SSUSB_DEV_BASE`（= MAC 基址 `0x11201000`）：

| 寄存器 | 偏移 |
|---|---|
| `U3D_LV1ISR` / `LV1IER` / `LV1IESR` / `LV1IECR` | `0x00 / 0x04 / 0x08 / 0x0C` |
| `U3D_EPISR` / `EPIER` / `EPIESR` / `EPIECR` | `0x80 / 0x84 / 0x88 / 0x8C` |
| `U3D_EP0CSR` | `0x100` |
| `U3D_RXCOUNT0` | `0x108` |
| `U3D_TX1CSR0` / `TX1CSR1` / `TX1CSR2` | `0x110 / 0x114 / 0x118` |

`U3D_EP0CSR` 位（同头文件）：

| 位 | 名称 |
|---|---|
| 25 | `EP0_SENDSTALL` |
| 20 | `EP0_DPHTX` |
| 19 | `EP0_DATAEND` |
| 18 | `EP0_TXPKTRDY` |
| 17 | `EP0_SETUPPKTRDY` |
| 16 | `EP0_RXPKTRDY` |
| 9:0 | `EP0_MAXPKTSZ0` |

`U3D_RXCOUNT0` 9:0 = 本包接收字节数。

### 2.3 IPPC（设备电源/角色）位

来自 Linux v6.12 `drivers/usb/mtu3/mtu3_hw_regs.h`（IPPC 基址 + 偏移）：

| 寄存器 | 偏移 | 位 |
|---|---|---|
| `U3D_SSUSB_IP_PW_CTRL0` | `+0x00` | `SSUSB_IP_SW_RST` bit0 |
| `U3D_SSUSB_IP_PW_CTRL1` | `+0x04` | `SSUSB_IP_HOST_PDN` bit0 |
| `U3D_SSUSB_IP_PW_CTRL2` | `+0x08` | `SSUSB_IP_DEV_PDN` bit0 |
| `U3D_SSUSB_U2_CTRL_0P` | `+0x50` | `FORCE_IDDIG` bit11、`RG_IDDIG` bit12、`PORT_PDN` bit1 |

设备模式 = 关 host IP、开 device IP、U2 口 `IDDIG` 强制为 device（`FORCE_IDDIG|RG_IDDIG` 同时置位，见 `mtu3_dr.c:ssusb_set_force_mode()`）。

### 2.4 描述符与串口参数（沿用主线已跑通的配置）

| 项 | 值 | 出处 |
|---|---|---|
| `idVendor` | `0x1d6b`（Linux Foundation） | `mainline-linux-mt6771/script/a3-gadget-ecm.sh:117` |
| `idProduct` | `0x0105` | 同上 `:118` |
| `bcdUSB` | `0x0200` | 同上 `:120` |
| 串口参数 | **115200 8N1，无流控** | `docs/a3-usbnet.md:86`、`docs/partition-userdata-split.md:147` |
| Linux 侧设备名 | `/dev/ttyGS0`（手机侧）、主机侧 COMx | `docs/a3-usbnet.md:244` |

### 2.5 CDC-ACM 拓扑（标准 USB 类定义）

```
配置 1
├── 接口 0: CDC Communication (0x02/0x02/0x01)  + 类请求 SET/GET_LINE_CODING 等
│   └── EP1 IN  中断 (0x83)，8 字节，用于串口状态通知
└── 接口 1: CDC Data (0x0A/0x00/0x00)
    ├── EP2 OUT 批量 (0x02)，承载 主机→设备 数据
    └── EP2 IN  批量 (0x82)，承载 设备→主机 数据
```

端点号/方向是 CDC-ACM 的常规布局，具体端点最大包长与 MTU3 的 FIFO 分配需在实现时按控制器能力确定（TODO）。

## 3. 实现分阶段

### 阶段 1：设备模式起来并能被识别（未完成）

- [x] IPPC 切到 device 角色（寄存器与位已确定，见 2.3）
- [ ] MTU3 device IP 软复位与上电时序（需要厂商 `mu3d` 初始化序列的完整列表）
- [ ] USB PHY 设备模式初始化（`mu3phy/mt6771/mtk-phy-asic.c`）
- [ ] 端点配置：EP0 + 3 个数据端点、FIFO 大小与映射

### 阶段 2：枚举（未完成）

- [ ] EP0 控制传输状态机：`SETUP → DATA → STATUS`，用 `U3D_EP0CSR` 的
      `SETUPPKTRDY/RXPKTRDY/TXPKTRDY/DATAEND/DPHTX` 位驱动
- [ ] 描述符：Device / Config(CDC×2 接口) / String（VID/PID 见 2.4）
- [ ] 处理 `SET_ADDRESS`、`SET_CONFIGURATION`、CDC `SET_LINE_CODING`（115200 8N1）

### 阶段 3：数据通路（未完成）

- [ ] EP2 OUT：主机→设备，喂给 Shell 输入
- [ ] EP2 IN：设备→主机，输出 Shell 文本
- [ ] 实现 `EFI_SERIAL_IO_PROTOCOL`，让 `TerminalDxe` 绑上去 → Shell 控制台

## 4. 现状与风险

- 目前仓库中 `Silicon/MediaTek/MediaTekPkg/Drivers/Mt6771UsbAcmDxe/` 只有骨架：
  寄存器定义、描述符表、设备角色切换。**未安装任何协议，未进入固件卷**，
  因此不会影响现有 Shell 显示。
- 之所以不直接写完整驱动：MTU3 设备侧的数据通路使用 TRB/Ring 结构
  （厂商 `mu3d` 驱动数千行），没有完整序列就实现等于猜参数，风险高于收益。
- 下一步最省事的验证方式：先把阶段 1、2 做完（能被 PC 识别为 ttyACM0），
  再决定数据通路的实现方式。

## 5. 备选方案（若 CDC-ACM 成本过高）

1. **USB host + USB-TTL 适配器**：需要先打通 VBUS（当前卡在 MT6370 I2C）。
2. **eMMC 日志文件**：把 Shell 输出写到 `cache` 分区尾部，再在系统里 `dd` 取出
   （方案已记录在 `docs/porting.md` 第 13 节，依赖 eMMC BlockIo 可用）。
3. **屏幕拍照 + startup.nsh 自动跑命令**：当前已在用，零成本但信息带宽最低。
