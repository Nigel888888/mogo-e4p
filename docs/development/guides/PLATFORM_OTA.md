# Mogo 平台 OTA 发布

1. 在 mogo-platform 后端设置 `MOGO_INTEGRATION_FIRMWARE_TOKEN` 为至少 32 字符的随机密钥，并通过 HTTPS 部署；确认机器接口没有其他入口绕过认证。
2. 在本仓库 GitHub Actions 的变量中设置 `MOGO_PLATFORM_URL`（HTTPS 网站根地址），在 Secrets 中设置 `MOGO_FIRMWARE_SUBMISSION_TOKEN`（与网站相同），并按[正式发布契约](RELEASE.md)配置 `MOGO_CLOUDZAO_INTEGRATION_ID`。限制有权创建或移动发布标签的人，避免不可信代码借发布工作流读取密钥。
3. 推送 `v1.2.3` 或 `v1.2.3-rc1` 标签。工作流以 ESP-IDF 6.1 的 ESP32-S31 产品配置构建；普通版本进入 stable，rc 版本进入 beta。设备默认读取 stable，beta 不会自动推送给默认设备。
4. 在网站后台检查版本、来源、文件、更新说明和最低基础版本后手动发布。第一次使用联网升级的设备可能需要先通过网页或 TF 卡安装包含升级入口和正确分区布局的引导版本。
5. 在真实设备上验证升级、断网恢复、数据保留后再宣布可用。更换密钥时同步更新网站和 GitHub Secret。不要把密钥提交到仓库。

既有标签的宿主测试夹具失效时，可从当前工作流手动重试原标签，并将 `validation_commit` 指定为已提交修复的完整 SHA。入口拒绝该提交在 `tools/` 以外的任何差异；正式构建仍使用原标签的干净源码，完整宿主回归在独立检出中运行。验证前后核对应用摘要与两份源码身份，保存验证提交、差异补丁和 `validation-provenance.json`。不得借此改变产品实现或移动标签。
When only the tagged host harness needs repair, retry the unchanged tag with an exact committed `validation_commit`. Reject every difference outside `tools/`, build clean tagged sources and run the full host suite in a separate checkout. Verify source identities and the application digest afterward, preserving the validation commit, patch and provenance. This cannot change product implementation or move a tag.

## 发布附件与 LGPL / Release assets and LGPL

正式标签工作流保留两组同源产物：`firmware-<tag>-<run>` 用于 USB、TF 与 OTA；`lgpl-relink-<tag>-<run>` 保存静态库、应用入口目标文件、链接脚本、工具链响应文件与 specs、原始链接命令、工具链标识与实际使用的托管组件源码。工作流先执行原链接命令并逐字节比较 ELF；不通过就不提交 OTA 草稿。
Official tag runs preserve firmware delivery inputs and a separate LGPL relinking artifact with static libraries, the application entry object, linker scripts, toolchain response files and specs, the original link command, toolchain identity and managed-component sources. The original command must reproduce the ELF byte for byte before OTA draft submission.

GitHub Release 不再手动附加整仓源码包。发版前须确认对应标签源码和同次构建的可重链接材料另有长期可访问的获取途径，在 USB 包和发布说明中标注获取方式。材料包括实际使用的第三方源码与许可、静态库和应用目标代码、原工作目录、IDF 环境和重链接步骤，不包含生成的 CloudZao 接入 ID 头文件。不能只保留有期限的 Actions artifact 或仅提供固件；材料未准备好时不要开始公开分发。个人学习条款不限制第三方许可保障的修改、调试和重新链接权利。
Do not manually attach a repository source bundle to GitHub Release. Before publishing, verify durable access to the tagged corresponding source and same-build relinking materials, and document access in the USB bundle and release notes. Supply the third-party source and licenses, libraries and application objects, original working directory, IDF environment and relinking instructions, excluding the generated CloudZao integration header. Expiring Actions artifacts or firmware alone are insufficient; do not distribute publicly before the materials are ready. Preserve third-party modification, debugging and relinking rights.
