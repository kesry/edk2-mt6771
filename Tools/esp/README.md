# 用 UEFI 引导 arm64 发行版（fnOS / Debian 等）

本固件已经具备导出启动所需的一切：eMMC BlockIo、GPT、FAT 驱动、GOP
（`SimpleFbDxe` 基于 LK 注入的 simple-framebuffer），并且 BDS 会调用
`EfiBootManagerRefreshAllBootOption()` 自动发现 ESP 上的
`\EFI\BOOT\BOOTAA64.EFI`。因此**不需要改固件**就能通过引导器启动系统。

## 1. 准备一个 FAT32 的 ESP

需要一个 FAT32 分区（ESP）。可以从 `data`（p45，110GB）划出一块，例如 1GB：

```sh
# 前提：已在 Linux 下，且 data 分区已缩容
mkfs.vfat -F32 -n ESP /dev/mmcblk0pXX
```

> 注意：FAT32 要求分区至少约 33MB；GRUB + 内核 + initrd 建议 1GB 以上。

## 2. 放入文件

```
/EFI/BOOT/BOOTAA64.EFI    GRUB arm64（grubaa64.efi 改名）
/boot/grub/grub.cfg       见本目录 grub.cfg
/a3.dtb                   我们的设备树（mainline 仓库 a3-v186-tgpio.dtb）
/vmlinuz                  fnOS ARM64 内核
/initrd.img               fnOS ARM64 initrd
```

## 3. 为什么必须换 dtb

官方 ARM ISO 的 dtb 是给鲲鹏一类服务器准备的，描述的硬件与本机完全不同。
GRUB 的 `devicetree` 命令会把我们的 dtb 交给内核，**优先级高于内核自带/内置
的那份**，这是解决硬件不匹配的关键一步。

## 4. 启动顺序

```
LK → BootShim(伪内核) → EDK2 → BDS 发现 ESP 的 BOOTAA64.EFI
   → GRUB 读 grub.cfg → 加载我们的 dtb + 内核 + initrd → Linux
```

Shell 仍保留为启动项，万一 GRUB 配置有问题还能回到 Shell 里修。

## 5. 已知风险

- GRUB 的 `gfxterm` 需要 GOP；我们有 GOP，但若显示异常可改用
  `terminal_output console`（grub.cfg 里已经是这样）。
- 内核需要支持本机：内存布局、simple-framebuffer、eMMC（我们的驱动在 UEFI
  里够用，Linux 里用主线 `mtk-sd`）。
