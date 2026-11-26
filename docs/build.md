# 构建说明

## 本地构建

前置条件（Ubuntu/Debian）：

```sh
sudo apt-get update
sudo apt-get install -y build-essential gcc-aarch64-linux-gnu python3 python3-pip \
  make git curl ca-certificates device-tree-compiler uuid-dev libssl-dev nasm
python3 -m pip install edk2toolext edk2toollib pyyaml
```

本仓库使用上游 EDK2 子模块的**固定 commit**（见 `.gitmodules`）。首次构建：

```sh
git clone https://github.com/kesry/edk2-mt6771.git
cd edk2-mt6771
git submodule update --init --recursive   # 若网络不通，见 CI 的 tarball/fetch 方案
./build.sh -s -t RELEASE OPPO/A3Pkg
./build.sh -t RELEASE OPPO/A3Pkg
```

产物：

- `Build/A3/FV/A3_UEFI.fd`（EDK2 固件）
- `Build/A3/FV/A3_UEFI.fd.shimmed`（加 BootShim）
- `Build/A3/FV/A3_UEFI.fd.shimmed.gz`（压缩）
- `Build/boot-uefi.img`（可刷入 boot 分区的最终镜像，32 MiB；GitHub Actions 中作为
  `boot-uefi-<run_number>` artifact 单独上传）

## GitHub Actions

 workflow `.github/workflows/build.yml` 在 push / PR / 手动触发时运行：

1. 安装依赖；
2. 按固定 commit 拉取 `edk2` 与 `edk2-platforms`；
3. 安装 `edk2toolext`；
4. `./build.sh -s` 初始化环境，`./build.sh` 编译；
5. 上传 `build.log`、固件 bundle（`*.fd`, `*.map`），并把最终可刷镜像
   `Build/boot-uefi.img` 单独上传为 `boot-uefi-<run_number>` artifact。

构建失败时仍会上传 `build.log` artifact，便于复现；workflow 最后一步会因失败而退出非零。
不要在 workflow 中打印 token 或读取 `security/` 目录。

## boot.img 打包细节

`Platform/OPPO/A3Pkg/PlatformBuild.py` 调用 `Tools/mkbootimg-a3.py`，其参数来自
原机 `Android_boot.img` 与 `mainline-linux-mt6771` 实测：

- header v2, page size 2048, 32 MiB 镜像；
- kernel_addr=0x40080000, ramdisk_addr=0x55000000, tags_addr=0x54000000；
- DTB 同时追加到 kernel 段并作为独立段，独立段前加 64 字节 MTK DTB 头（0xD7B7AB1E）；
- 从 `Blob/stock-vbmeta.bin` 嫁接 AVB vbmeta 并写 footer（LK 校验结构需要）。

若替换 `Blob/a3-v186-tgpio.dtb`，必须保留 `/__symbols__`，否则 LK 套用 DTBO 时会失败。
