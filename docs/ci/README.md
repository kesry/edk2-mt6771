# CI 构建记录

## Run #1 — 45771069163 (2026-11-26, failure)

- workflow: `Build MT6771 UEFI`
- job: `build` (139743567352)
- 结果：**failure**，失败步骤：`Install build dependencies`
- 错误原文：
  ```
  E: Unable to locate package aarch64-linux-gnu
  ```
- 根因：workflow 的 apt 包列表包含了不存在的包名 `aarch64-linux-gnu`。
- 修复：改为 `gcc-aarch64-linux-gnu` + `libc6-dev-arm64-cross`（commit `4a6080c`）。
- 未验证：修复后是否进入 EDK2 编译阶段；待下一次 CI 运行结果确认。

完整日志：[run-45771069163.log](run-45771069163.log)

> 后续每次修复只追加记录，不覆盖历史日志。

## Run #2 — 45771408216 (2026-11-26, failure)

- 结果：**failure**，失败步骤：`Install Python build tools`
- 错误原文：
  ```
  ERROR: Could not find a version that satisfies the requirement edk2toolext (from versions: none)
  ```
- 根因：PyPI 上真实的包名是 `edk2-pytool-extensions` 和 `edk2-pytool-library`，
  不是 `edk2toolext` / `edk2toollib`（已用 `https://pypi.org/pypi/<name>/json` 核对）。
- 修复：workflow 改用正确包名（本次提交）。
- 完整日志：[run-45771408216.log](run-45771408216.log)

## Run #3 — 45772445402 (2026-11-26, failure)

- 结果：**failure**，失败步骤：`Initialize EDK2 environment`
- 错误原文：
  ```
  ERROR - Error when trying to resolve Common/edk2
  stderr: 'error: pathspec 'Common/edk2' did not match any file(s) known to git'
  ```
- 根因：初始提交用 `git archive` 导出树，丢失了 `.gitmodules` 对应的 gitlink，
  `stuart_setup` 调用 `git submodule sync -- Common/edk2` 时找不到 submodule。
- 修复：
  1. 用 `git update-index --cacheinfo 160000` 恢复两个 gitlink
     （`Common/edk2@e7aac7fc...`、`Common/edk2-platforms@98221266...`，与上游锁定值一致）；
  2. workflow 改为 `actions/checkout` 的 `submodules: true`，删除手工 fetch 目录的步骤。
- 完整日志：[run-45772445402.log](run-45772445402.log)

## Run #4 — 45772997971 (2026-11-26, failure)

- 结果：**failure**，失败步骤：`Initialize EDK2 environment`（stuart_update）
- 错误原文：
  ```
  ERROR - Mono is required to run NuGet on non-Windows platforms but was not found on the PATH.
  WARNING - [SDE] Failed to fetch NugetDependecy: mu_nasm@2.15.5
  ```
- 根因：EDK2 stuart 在 Linux 上需要 Mono 运行 NuGet 拉取 `mu_nasm` 等 ext_dep。
- 修复：在 apt 依赖中加入 `mono-complete`（本次提交）。
- 完整日志：[run-45772997971.log](run-45772997971.log)

## Run #5 — 45773461419 (2026-11-26, failure)

- 结果：**failure**，失败步骤：`Build platform`（`continue-on-error` 后由末步判失败）
- 进展：apt、submodule、Python 工具、`stuart_setup/update`、BaseTools 全部成功；
  真正进入 EDK2 编译阶段后失败。
- 错误原文：
  ```
  ERROR - Failed to convert Edk2Relative Path to an Absolute Path on this system.
  ERROR - Relative Path: A3Pkg/a3.dsc
  TypeError: stat: path should be string, bytes, os.PathLike or integer, not NoneType
  ```
- 根因：`PlatformBuild.py` 从 T1 模板复制后，`PackagesPath` 仍是 `"Platform/Mashtab"`，
  导致 `A3Pkg/a3.dsc` 无法解析；同时 `PackagesSupported = ("A3Pkg")` 少了逗号，不是 tuple。
- 修复：改为 `"Platform/OPPO"` 和 `("A3Pkg",)`（本次提交）。
- 完整日志：[build-run45773461419.log](build-run45773461419.log)

## Run #6 — 45774355830 (2026-11-26, failure)

- 结果：**failure**，失败步骤：`Build platform`（EDS 编译阶段）
- 错误原文：
  ```
  INFO - /bin/sh: 1: llvm-ar: not found
  ERROR - Compiler #7000 from : Failed to execute command
  ```
- 根因：EDK2 目标工具链是 `CLANG38`（Build 目录下 `RELEASE_CLANG38`），
  CI 未安装 `llvm-ar` 所在的 `llvm` 包。
- 修复：apt 依赖加入 `llvm clang`（本次提交）。
- 完整日志：[build-run45774355830.log](build-run45774355830.log)

## Run #7 — 45775389292 (2026-11-26, failure)

- 结果：**failure**，失败步骤：`Build platform`（EDS 编译阶段）
- 错误原文：
  ```
  INFO - /bin/sh: 1: iasl: not found
  INFO - error F002: Failed to build module ... RamDiskDxe.inf
  ```
- 根因：EDK2 编译 ASL/ACPI 需要 `iasl`（ACPI Component Architecture ASL Compiler）。
- 修复：apt 依赖加入 `iasl`（本次提交）。
- 完整日志：[build-run45775389292.log](build-run45775389292.log)
