# 正式发布契约 / Release Contract

本页是版本标签、发布命名和同源交付的权威；平台草稿操作见[平台 OTA](PLATFORM_OTA.md)。发布前仍须有本次发布授权。
This is the authority for tags, naming and consistent delivery; platform draft operations are separate and each publication requires authorization.

### OTA 版本更新日志 / OTA release notes

每次用户要求创建 OTA 版本标签时，基于上一个对应发布版本到本次提交的实际差异，先整理准确的中文更新日志，写入 `docs/ota-release-notes/<版本号>.txt` 并与代码一起提交，然后再创建版本标签。日志以 UTF-8 编码，去除首尾空白后不得为空或超过 383 字节；不要虚构功能、修复或验证结果。标签构建自动读取对应文件，随固件提交到平台草稿；发布仍须管理员审核。已存在的版本或标签不得覆盖或重新指向。

For each OTA version tag, summarize actual changes since the appropriate previous release in `docs/ota-release-notes/<version>.txt`, commit it before tagging, and keep the nonempty UTF-8 text within 383 bytes. Never invent changes or verification. The workflow submits it with the firmware draft; an administrator still reviews and publishes. Do not overwrite existing versions or move tags.

### 正式版发布 / Official releases

GitHub Release 只手动上传经验证的固件、USB 刷机包、校验值和构建来源文件；不制作、不上传整仓源码包或包含源码的自定义附件。此规则不影响 GitHub 按 tag 自动生成的 `Source code (zip)` / `Source code (tar.gz)` 链接，也不取消单独的 LGPL 合规和重链接验证。
Manually attach only verified firmware, USB flash bundles, checksums and build provenance. Do not create or attach a repository source bundle or other custom asset containing source. This does not affect the `Source code (zip)` and `Source code (tar.gz)` links GitHub generates from the tag, or separate LGPL compliance and relinking checks.

正式制品仅从干净的版本标签构建，启用 `-D MOGO_PUBLIC_RELEASE=ON`；只有 `Build and submit OTA firmware` 的构建步骤从 GitHub Actions Secret `MOGO_CLOUDZAO_INTEGRATION_ID` 注入 CloudZao 接入 ID。发版前由维护者在仓库 `Settings → Secrets and variables → Actions` 配置该 Secret，并限制发布标签的写权限。缺少或格式无效时构建失败；产物检查确认应用 BIN 已包含 ID，才继续打包与提交。相同应用 BIN 供 OTA、TF、USB 和官网刷机使用，不用无 ID 的普通 CI 构建代替。
Build release artifacts only from a clean version tag with `-D MOGO_PUBLIC_RELEASE=ON`. The firmware build step of `Build and submit OTA firmware` injects the ID from the GitHub Actions secret `MOGO_CLOUDZAO_INTEGRATION_ID`. Before releasing, the maintainer sets the secret in Settings → Secrets and variables → Actions and restricts who can write release tags. Invalid or missing values fail the build; verify the application BIN contains the ID before packaging and submission. Reuse that application BIN for OTA, TF, USB and the web flasher; never substitute a normal CI build without an ID.

接入 ID 不写入源码、sdkconfig、发布日志或编译命令；只在 build 下生成临时头文件，该文件不得打包。接入 ID 可从正式固件和同次构建的目标代码中提取，因此不是设备令牌，也不能拿设备令牌或开发者密钥代替。
Keep the ID out of sources, sdkconfig, release logs and compiler command lines. Do not package the temporary header generated in the build tree. The ID is extractable from release firmware and the corresponding object code; never substitute a device token or developer key.

用户 2026-11-03 确认：今后提到「发布版本」，均指发布到本仓库当前 `origin` 远端，替代此前固定的旧发布仓库；发布前核对 `git remote get-url origin`，当前为 `chopper1026/mogo-e4p`。推送代码，创建并推送对应版本的 Git tag，触发 OTA 网站的固件草稿构建与提交，并在同一远端创建 GitHub Release。附上经验证的 TF/OTA 应用 Bin、完整 USB 刷机包、SHA-256 校验值和简要更新日志；所有附件与 OTA 提交必须来自同一次正式构建。OTA 草稿仍须管理员审核发布，不得把草稿提交成功视为已经上线。
Per the user's 2026-11-03 decision, "publish a version" means publishing to this repository's current `origin`, replacing the previously fixed legacy release repository. Check `git remote get-url origin` before publishing; it currently points to `chopper1026/mogo-e4p`. Push the code and a matching Git tag to trigger the OTA website's draft build/submission, and create a GitHub Release in the same remote repository. Attach the verified TF/OTA application Bin, complete USB flash bundle, SHA-256 checksums and brief release notes; all attachments and the OTA submission must come from the same official build. OTA drafts still require administrator review and publication; successful submission does not mean they are live.

用户 2026-11-04 确认 GitHub Release 的展示与命名规范如下；`<版本号>` 不含 `v`，Git tag 仍使用 `v<版本号>`（例如 `v1.0.3`）。
Per the user's 2026-11-04 decision, use the following GitHub Release presentation and naming rules. `<version>` excludes `v`; Git tags remain `v<version>` (for example, `v1.0.3`).

| 项目 / Item | 格式 / Format | 1.0.3 示例 / Example |
| --- | --- | --- |
| Release 标题 / Release title | `mogo E4p <版本号>` / `mogo E4p <version>` | `mogo E4p 1.0.3` |
| TF/OTA 应用镜像 / TF/OTA application | `mogo-e4p-update-<版本号>.bin` / `mogo-e4p-update-<version>.bin` | `mogo-e4p-update-1.0.3.bin` |
| USB 单文件整机镜像 / Single-file USB image | `mogo-e4p-<版本号>-full.bin` / `mogo-e4p-<version>-full.bin` | `mogo-e4p-1.0.3-full.bin` |
| 完整 USB 刷机包 / Complete USB bundle | `mogo-e4p-<版本号>-usb.zip` / `mogo-e4p-<version>-usb.zip` | `mogo-e4p-1.0.3-usb.zip` |

Release 说明按更新日志、下载、验证组织，下载文件名与实际附件、包外 `README.txt`、`SHA256SUMS` 保持一致，保留 `release-provenance.json` 构建来源。保留当前目标硬件与刷写方式说明，不再添加「不适用于旧ESP32-S3设备」一句。以上仅调整 GitHub Release 的标题、下载文件名及说明，不改变固件界面品牌、目标硬件、构建部件名、备份文件名、内部协议或官网/OTA 地址；未经用户明确要求，不更名或替换已经发布的附件。
Organize Release descriptions as updates, downloads and verification. Keep download names consistent across assets, the outer `README.txt` and `SHA256SUMS`, and retain build provenance in `release-provenance.json`. Describe the current target hardware and flashing methods without adding the old ESP32-S3 exclusion sentence. These rules affect only GitHub Release titles, download filenames and descriptions, not firmware UI branding, hardware targets, build component names, backup filenames, internal protocols or website/OTA URLs. Do not rename or replace published assets without an explicit user request.

GitHub Release 与官网刷机、TF 下载和联网 OTA 同步发布。保留官网现有刷机清单、固件及版本下载地址；OTA 清单继续使用固件允许的官方 HTTPS 域名。核对三种交付的版本、大小、校验值和基础包兼容性，避免影响正在刷机或升级的用户。本地测试仍只生成本地包，不创建正式 Release。
Publish GitHub Releases alongside the web flasher, TF downloads and online OTA. Preserve existing website manifests, firmware and versioned download URLs; keep OTA URLs on the official HTTPS origin accepted by the firmware. Verify matching versions, sizes, checksums and base-layout compatibility across the deliveries so ongoing flashes and updates remain valid. Local tests remain local bundles without official Releases.

