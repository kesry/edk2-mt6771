# 在虚拟机里跑飞牛原版内核

## 为什么只能走虚拟机

飞牛 ARM 内核的配置是给 ACPI 平台（此芯 P1 / 苹果 M 系列虚拟化）编译的：

```
# CONFIG_ARCH_MEDIATEK is not set
# CONFIG_MMC_MTK is not set
```

MediaTek 的时钟、存储、USB-PHY、显示驱动**一行都没有**。真机上无论用 DTB 还是
ACPI，内核都没有驱动能绑定我们的硬件，因此唯一能"原样使用飞牛内核"的环境是
虚拟机（虚拟机由 hypervisor 提供 virtio 设备，不需要 SoC 平台代码）。

## 两种启动方式，用来看不同的失败

| 模式 | 绕开了什么 | 用来回答 |
|---|---|---|
| `kernel` | UEFI、ISO、El Torito、ACPI 找引导项 | **飞牛内核能不能跑？** |
| `iso`    | — | 安装镜像能不能引导（需要 AAVMF + SCSI 光驱）|

先跑 `kernel`。如果它能进系统，说明内核本身没问题，`iso` 模式失败纯粹是引导
参数问题；如果 `kernel` 也起不来，问题在内核/initrd 与 virt 硬件的匹配。

## 用法

```sh
# 1) 造 rootfs 磁盘（从已挂载的飞牛 rootfs 复制）
truncate -s 16G /opt/fnos-rootfs.img
mkfs.ext4 -F -L fnos-root /opt/fnos-rootfs.img
mkdir -p /opt/rfs && mount -o loop /opt/fnos-rootfs.img /opt/rfs
rsync -aHAX --numeric-ids \
  --exclude=/proc/* --exclude=/sys/* --exclude=/dev/* \
  --exclude=/run/* --exclude=/tmp/* --exclude=/mnt/* \
  /mnt/ /opt/rfs/
sync && umount /opt/rfs

# 2) 直接内核启动（先验证内核）
Tools/vm/boot-fnos-vm.sh kernel -b /mnt/boot -d /opt/fnos-rootfs.img

# 3) 安装镜像 + UEFI（需要 qemu-efi-aarch64）
Tools/vm/boot-fnos-vm.sh iso -i /path/fnos-arm.iso -d /opt/fnos-disk.img -v
```

退出 `-nographic`：先按 `Ctrl-A`，再按 `X`。

## 常见失败与含义

| 输出 | 含义 | 处理 |
|---|---|---|
| 完全无输出 | 内核不支持该 virt 机器或 CPU 型号 | 保留 `-cpu max -M virt,gic-version=3`；去掉 `-accel kvm` 试 TCG |
| `VFS: Unable to mount root fs` | 内核起来了，root 设备不对 | initrd 里缺 virtio_blk，或改用 `root=PARTUUID=` |
| 进 initramfs 紧急 shell | 同上，但 initrd 可交互 | 在 shell 里 `ls /dev/vd*` 确认磁盘是否存在 |
| UEFI 里看不到引导项 | CD 挂在 virtio-blk 上，AAVMF 认不到 El Torito | 必须用 `virtio-scsi` + `scsi-cd`（脚本已这样配）|
| QEMU 报 `Could not access KVM` | `/dev/kvm` 不可用 | 脚本已自动回落 TCG |

## 关于真机

在手机上直接用我们的 UEFI 引导飞牛 ISO 必然失败（内核没有 MTK 驱动），
这条不需要再试。真机上跑飞牛只能使用带 MTK 驱动的内核。
