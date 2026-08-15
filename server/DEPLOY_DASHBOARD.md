# 智慧药品冷藏多页面控制台与固件中心部署指南

服务器在同一个 `10087` 端口提供公开页面和管理员页面：

- 公开：`/`、`/monitor`、`/inventory`、`/firmware`。
- 管理：`/admin`、`/admin/devices`、`/admin/monitor`、`/admin/inventory`、`/admin/logs`、`/admin/firmware`、`/admin/ai`。

W800仍使用 `10086`，手机App继续使用 `X-Fridge-Token`，不需要修改App或RA6M5协议。当前部署方式是公网HTTP演示模式，登录密码和控制指令没有传输加密，正式长期运行应升级HTTPS。

## 一、本地上传文件

在Windows PowerShell执行：

```powershell
Set-Location "D:\Desktop\fridge _codex"
$ServerIp = "101.245.87.7"
$SshUser = Read-Host "请输入服务器 SSH 用户名"

ssh "${SshUser}@${ServerIp}" "mkdir -p ~/fridge-dashboard-upload"
python .\server\prepare_firmware_seed.py

scp .\server\fridge_deepseek_server.py .\server\dashboard.py .\server\generate_admin_password.py .\server\import_firmware_seed.py .\server\requirements.txt "${SshUser}@${ServerIp}:~/fridge-dashboard-upload/"
scp -r .\server\web "${SshUser}@${ServerIp}:~/fridge-dashboard-upload/"
scp -r .\server\firmware_seed "${SshUser}@${ServerIp}:~/fridge-dashboard-upload/"
```

上传完成后登录服务器：

```powershell
ssh "${SshUser}@${ServerIp}"
```

后续命令全部在Ubuntu服务器中执行。

## 二、备份线上版本

```bash
sudo systemctl status fridge-deepseek.service --no-pager
find ~/fridge-dashboard-upload -maxdepth 2 -type f -print

BACKUP_FILE="/var/backups/fridge-deepseek-$(date +%Y%m%d-%H%M%S).tar.gz"
sudo install -d -m 0750 /var/backups
sudo tar -C /opt -czf "$BACKUP_FILE" fridge-deepseek
echo "备份完成：$BACKUP_FILE"
```

请保存最后显示的备份路径，回滚时需要使用。

## 三、安装服务器和网页文件

```bash
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/fridge_deepseek_server.py /opt/fridge-deepseek/fridge_deepseek_server.py
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/dashboard.py /opt/fridge-deepseek/dashboard.py
sudo install -o fridge -g fridge -m 0755 ~/fridge-dashboard-upload/generate_admin_password.py /opt/fridge-deepseek/generate_admin_password.py
sudo install -o fridge -g fridge -m 0755 ~/fridge-dashboard-upload/import_firmware_seed.py /opt/fridge-deepseek/import_firmware_seed.py
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/requirements.txt /opt/fridge-deepseek/requirements.txt

sudo install -d -o fridge -g fridge -m 0755 /opt/fridge-deepseek/web
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/web/index.html /opt/fridge-deepseek/web/index.html
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/web/dashboard.css /opt/fridge-deepseek/web/dashboard.css
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/web/dashboard.js /opt/fridge-deepseek/web/dashboard.js
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/web/admin.html /opt/fridge-deepseek/web/admin.html
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/web/admin.css /opt/fridge-deepseek/web/admin.css
sudo install -o fridge -g fridge -m 0644 ~/fridge-dashboard-upload/web/admin.js /opt/fridge-deepseek/web/admin.js

sudo install -d -o fridge -g fridge -m 0755 /opt/fridge-deepseek/firmware
sudo install -d -o fridge -g fridge -m 0755 /opt/fridge-deepseek/firmware_seed
sudo cp -a ~/fridge-dashboard-upload/firmware_seed/. /opt/fridge-deepseek/firmware_seed/

sudo -u fridge /opt/fridge-deepseek/.venv/bin/pip install -r /opt/fridge-deepseek/requirements.txt
sudo chown -R fridge:fridge /opt/fridge-deepseek
```

`chown`必须使用 `-R`，否则旧的 `__pycache__` 或SQLite文件可能仍由root所有，导致服务用户没有写入权限。

## 四、生成管理员密码哈希

执行：

```bash
cd /opt/fridge-deepseek
sudo -u fridge ./.venv/bin/python generate_admin_password.py
```

程序会要求输入两次管理员密码。输入过程不会显示字符。成功后会输出类似：

```text
pbkdf2_sha256$600000$一段随机字符$一段哈希字符
```

复制完整一行。这个哈希不是登录密码，网页登录时仍输入刚才设置的原始密码。

## 五、修改环境变量

```bash
sudoedit /etc/fridge-deepseek.env
```

保留原有 `DEEPSEEK_API_KEY`、`FRIDGE_APP_TOKEN`、端口和数据库配置，在文件末尾确认存在：

```text
DASHBOARD_DB_PATH=/opt/fridge-deepseek/dashboard.db
DASHBOARD_WEB_ROOT=/opt/fridge-deepseek/web
FIRMWARE_ROOT=/opt/fridge-deepseek/firmware
DASHBOARD_ADMIN_PASSWORD_HASH=把第四步复制的完整哈希粘贴到这里
DASHBOARD_ALLOW_INSECURE_ADMIN=1
DASHBOARD_COOKIE_SECURE=0
```

注意：

- 哈希中的 `$` 必须全部保留，不要增加引号或空格。
- 当前是HTTP演示，所以允许不安全登录并关闭Secure Cookie。
- 将来部署HTTPS后，把 `DASHBOARD_ALLOW_INSECURE_ADMIN` 改为 `0`，`DASHBOARD_COOKIE_SECURE` 改为 `1`。

保存后执行：

```bash
sudo chmod 600 /etc/fridge-deepseek.env
sudo chown root:root /etc/fridge-deepseek.env
sudo -u fridge test -w /opt/fridge-deepseek && echo "数据库目录可写"
```

## 六、导入首批固件版本

先确认种子文件已上传，再以服务用户执行一次幂等导入：

```bash
find /opt/fridge-deepseek/firmware_seed -maxdepth 1 -type f -printf '%f %s bytes\n' | sort
cd /opt/fridge-deepseek
sudo -u fridge ./.venv/bin/python import_firmware_seed.py \
  --seed /opt/fridge-deepseek/firmware_seed \
  --db /opt/fridge-deepseek/dashboard.db \
  --firmware-root /opt/fridge-deepseek/firmware
```

重复执行不会重复建立版本或文件记录。首批应看到5个RA6M5版本、1个HMI版本；视觉模块暂时为空，后续在管理员“固件中心”上传。

## 七、检查代码并重启

```bash
cd /opt/fridge-deepseek
sudo -u fridge ./.venv/bin/python -m py_compile fridge_deepseek_server.py dashboard.py generate_admin_password.py import_firmware_seed.py
sudo systemctl restart fridge-deepseek.service
sudo systemctl status fridge-deepseek.service --no-pager
sudo journalctl -u fridge-deepseek.service -n 100 --no-pager
```

语法检查无输出表示通过。日志中应看到：

- W800监听 `10086`。
- HTTP监听 `10087`。
- dashboard数据库和web目录路径正确。

## 八、服务器本机验证

```bash
curl -s -o /dev/null -w "公开首页 HTTP %{http_code}\n" http://127.0.0.1:10087/
curl -s -o /dev/null -w "管理页面 HTTP %{http_code}\n" http://127.0.0.1:10087/admin
curl -s -o /dev/null -w "监控页面 HTTP %{http_code}\n" http://127.0.0.1:10087/monitor
curl -s -o /dev/null -w "固件页面 HTTP %{http_code}\n" http://127.0.0.1:10087/firmware
curl -s http://127.0.0.1:10087/api/public/dashboard/overview
curl -s http://127.0.0.1:10087/api/public/firmware/releases
curl -i http://127.0.0.1:10087/api/fridge/status
```

预期：

- 公开首页和管理页面返回 `200`。
- 公开总览返回 `public_demo: true`。
- 没有Token和登录会话的 `/api/fridge/status` 返回 `401`。

继续验证手机App Token接口没有受到影响：

```bash
sudo bash -c 'set -a; . /etc/fridge-deepseek.env; set +a; curl -s -H "X-Fridge-Token: $FRIDGE_APP_TOKEN" http://127.0.0.1:10087/api/ping'
sudo bash -c 'set -a; . /etc/fridge-deepseek.env; set +a; curl -s -H "X-Fridge-Token: $FRIDGE_APP_TOKEN" http://127.0.0.1:10087/api/fridge/status'
```

`/api/ping` 应返回 `pong`。

## 九、浏览器验收

先打开公开控制台：

```text
http://101.245.87.7:10087/
```

依次检查总览、实时监控、智慧仓储和固件中心，然后打开：

```text
http://101.245.87.7:10087/admin
```

使用第四步设置的原始密码登录。依次检查：

1. `/admin/devices` 创建一个手机客户端，复制一次性Token到手机App“服务器设置 → Token”。
2. 手机请求后显示在线；停用后该手机请求返回 `403 client_disabled`，重新启用后恢复。
3. `/admin/monitor` 显示温度、0x40/0x41功率、总功率、网络RTT和心跳间隔曲线。
4. 设备在线时温度、风扇、制冷、水冷和开门按钮可以操作；设备离线时自动禁用。
5. `/admin/inventory` 可录入中文日期，例如 `2025年7月16日`，保存后显示为 `2025-07-16`。
6. `/admin/logs` 可查看结构化服务器日志、事件和审计记录。
7. `/admin/firmware` 可创建版本、上传、发布、下架和删除；普通用户只能下载已发布版本。
8. `/admin/ai` 能返回初步诊断和库存内推荐药品。

门锁属于一次性命令，网页显示“命令已下发”不等于物理门锁已经成功动作，应结合现场设备确认。

## 十、烧录新版RA6M5主控

本地工程已经把状态包升级为10字段。编译后的文件是：

```text
D:\Desktop\fridge _codex\Fridge\project_firdge\Debug\project_firdge.srec
```

使用现有烧录方式写入RA6M5。烧录后服务器日志中的状态包应包含两路功率和RTT；旧固件仍然兼容，只是网页功率、RTT曲线显示“等待遥测”。

## 十一、端口和安全组

华为云安全组确认存在：

- TCP `10086`：W800设备通道。
- TCP `10087`：网页和手机HTTP API。

如果Ubuntu启用了UFW：

```bash
sudo ufw allow 10086/tcp
sudo ufw allow 10087/tcp
sudo ufw status
```

Windows验证：

```powershell
Test-NetConnection 101.245.87.7 -Port 10086
Test-NetConnection 101.245.87.7 -Port 10087
curl.exe http://101.245.87.7:10087/api/public/dashboard/overview
```

公网HTTP演示会让密码和Cookie以明文形式经过网络。答辩结束后建议限制安全组来源IP，或者绑定域名并部署HTTPS。

## 十二、日志与故障定位

```bash
sudo journalctl -u fridge-deepseek.service -f
sudo ss -lntp | grep -E ':10086|:10087'
sudo -u fridge test -r /opt/fridge-deepseek/web/admin.html && echo "管理页面可读"
sudo -u fridge test -w /opt/fridge-deepseek/dashboard.db && echo "数据库可写"
sudo -u fridge test -w /opt/fridge-deepseek/firmware && echo "固件目录可写"
```

常见问题：

- 页面提示“管理员密码尚未配置”：检查 `DASHBOARD_ADMIN_PASSWORD_HASH` 是否为空。
- 页面提示“HTTP管理登录未启用”：HTTP演示需要 `DASHBOARD_ALLOW_INSECURE_ADMIN=1`。
- 修改环境变量后无变化：执行 `sudo systemctl restart fridge-deepseek.service`。
- 登录连续失败后被锁定：等待15分钟，或重启服务清除内存中的失败计数。
- 页面能登录但控制按钮灰色：设备超过8秒没有上报 `FRIDGE STATUS`，属于离线保护。
- 指令显示确认超时：服务器已保存目标值，但8秒内没有收到匹配的RA6M5状态。
- 功率曲线为空：确认烧录的是新版SREC，并在UART7日志中查看新10字段 `FRIDGE STATUS`。
- 固件下载404：检查数据库中的SHA-256记录和 `/opt/fridge-deepseek/firmware` 文件权限。
- 独立手机Token返回403：该客户端已被管理员停用；到 `/admin/devices` 重新启用。

## 十三、一键回滚

把路径替换为第二步实际输出的备份文件：

```bash
BACKUP_FILE=/var/backups/fridge-deepseek-YYYYMMDD-HHMMSS.tar.gz
sudo systemctl stop fridge-deepseek.service
sudo mv /opt/fridge-deepseek "/opt/fridge-deepseek.failed-$(date +%Y%m%d-%H%M%S)"
sudo tar -C /opt -xzf "$BACKUP_FILE"
sudo chown -R fridge:fridge /opt/fridge-deepseek
sudo systemctl start fridge-deepseek.service
sudo systemctl status fridge-deepseek.service --no-pager
```

回滚后再次测试W800、公开控制台、带Token的手机API和管理页面。
