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
