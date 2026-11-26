# 认证与凭据说明（不含任何 token）

本项目使用 GitHub Personal Access Token (PAT) 通过 HTTPS 推送，不使用 SSH key。

## 一次性配置（本地）

1. 把 PAT 放在仓库外的 `security/` 目录（该目录已在 `.gitignore` 中排除）。
2. 配置 credential helper，把凭据保存在仓库外、仅当前用户可读的文件中：

   ```sh
   git config --global credential.helper store
   chmod 600 ~/.git-credentials
   ```

3. 首次推送时，用户名输入 `kesry`，密码输入 PAT。之后 helper 会缓存，免交互推送：

   ```sh
   git push -u origin main
   git push    # 不再要求输入用户名/密码
   ```

## 禁止事项

- 不把 token 写入 remote URL（会出现在 `.git/config` 和日志中）。
- 不把 token 写入 README、docs、workflow、commit message 或任何仓库文件。
- 不提交 `security/` 目录或任何凭据文件。
- GitHub Actions 不依赖本地凭据；如需访问私有资源，使用 GitHub Secrets，
  且不要在日志中回显。

remote 保持 HTTPS 形式：`https://github.com/kesry/edk2-mt6771.git`。
