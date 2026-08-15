# RA6M5 OTA 一键打包上传工具

双击 `start_ota_packager.bat` 启动。工具不会修改 `configuration.xml` 或
任何 `ra_gen` 文件。

## 使用前

1. 在 `project_firdge/src/ota_update_config.h` 中把
   `OTA_UPDATE_APP_VERSION` 改成高于设备当前版本的版本号，例如
   `1.0.0+15`。
2. 在 e² studio 中执行 `Project > Clean`，然后 `Build Project`，必须是
   `0 errors`。
3. 保证本机存在：
   - Python 3（带 Tkinter）
   - Node.js
   - `arm-none-eabi-objcopy`
   - ECDSA-P256 私钥
4. 服务器已经配置与该私钥匹配的公钥。

## 按钮

- `检查环境`：检查 Node.js、objcopy 和源码版本。
- `一键打包`：生成两种文件：
  - `*_signed_keyhash_pending.bin`：上传网站的 OTA 文件。
  - `app_keyhash_lower_*_sparse.hex`：本地 USB 烧录文件。
- `一键打包并上传`：打包、登录网站、创建或复用对应版本、发布版本并
  上传 OTA BIN。服务器必须返回 `image_valid=true` 才算成功。

上传成功不会自动部署到设备。最后仍需登录网站固件中心，人工确认版本和
设备，然后点击“部署到设备”。

## 安全约束

- 管理员密码不会保存。
- 私钥路径会保存在当前 Windows 用户的工具设置中，但私钥内容不会上传。
- OTA 使用 `pending` trailer；新程序稳定运行 10 秒后由设备确认。
- 网站 OTA 不接受 `.elf`、`.srec` 或 `.hex`，只接受签名的 `.bin`。
