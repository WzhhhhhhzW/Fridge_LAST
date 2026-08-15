# RA6M5 OTA 服务器一次性部署

下面的命令不会把签名私钥上传到服务器。服务器只需要
`fridge-firmware-public.pem` 公钥。

## 1. Windows 本地打包和上传

先在 `D:\Desktop\fridge` 打开 PowerShell。将服务器地址和私钥路径改成
你的实际值：

```powershell
Set-Location "D:\Desktop\fridge"

python .\server\package_server_source.py

$Server = "root@101.245.87.7"
$PrivateKey = "D:\你的安全目录\fridge-signing-private.pem"
$PublicKey = Join-Path $env:TEMP "fridge-firmware-public.pem"

openssl ec -in $PrivateKey -pubout -out $PublicKey

scp .\fridge-server-ota-20260725.zip "${Server}:/tmp/"
scp $PublicKey "${Server}:/tmp/"
ssh $Server
```

如果 `openssl` 不存在，可以用 MCUboot 的 Python 环境导出公钥；不要为了
省事把私钥传到云服务器。

## 2. Ubuntu 服务器完整部署命令

登录服务器后一次性粘贴下面整段。它会依次备份、解压、安装依赖、运行
全部测试、安装文件、重启并检查端口。任一关键步骤失败都会因为
`set -Eeuo pipefail` 立即停止，不会继续重启服务。

```bash
set -Eeuo pipefail

archive=/tmp/fridge-server-ota-20260725.zip
public_key_upload=/tmp/fridge-firmware-public.pem
stamp="$(date +%Y%m%d-%H%M%S)"
deploy_dir="/tmp/fridge-ota-deploy-$stamp"
backup_file="/var/backups/fridge-deepseek-before-ota-$stamp.tar.gz"

test -s "$archive"
test -s "$public_key_upload"
test -d /opt/fridge-deepseek
test -x /opt/fridge-deepseek/.venv/bin/python

install -d -m 0750 /var/backups
tar -C /opt -czf "$backup_file" fridge-deepseek
printf '备份完成：%s\n' "$backup_file"

install -d -m 0755 "$deploy_dir"
python3 -m zipfile -e "$archive" "$deploy_dir"

test -s "$deploy_dir/fridge_deepseek_server.py"
test -s "$deploy_dir/dashboard.py"
test -s "$deploy_dir/firmware_ota.py"
test -s "$deploy_dir/speech_pipeline.py"
test -s "$deploy_dir/requirements.txt"
test -s "$deploy_dir/web/admin.html"
test -s "$deploy_dir/web/admin.css"
test -s "$deploy_dir/web/admin.js"

/opt/fridge-deepseek/.venv/bin/pip install \
    -r "$deploy_dir/requirements.txt"

cd "$deploy_dir"
PYTHONPATH="$deploy_dir" /opt/fridge-deepseek/.venv/bin/python \
    -m py_compile \
    fridge_deepseek_server.py \
    dashboard.py \
    firmware_ota.py \
    speech_pipeline.py

PYTHONPATH="$deploy_dir" /opt/fridge-deepseek/.venv/bin/python \
    -m unittest discover -s . -p 'test_*.py'

install -o root -g root -m 0644 \
    "$public_key_upload" \
    /etc/fridge-firmware-public.pem

if grep -q '^FRIDGE_OTA_TOKEN=' /etc/fridge-deepseek.env; then
    ota_token="$(sed -n 's/^FRIDGE_OTA_TOKEN=//p' /etc/fridge-deepseek.env | tail -n 1)"
else
    ota_token="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
    printf '\nFRIDGE_OTA_TOKEN=%s\n' "$ota_token" >> /etc/fridge-deepseek.env
fi

grep -q '^FRIDGE_OTA_DEVICE_ID=' /etc/fridge-deepseek.env ||
    printf 'FRIDGE_OTA_DEVICE_ID=fridge-main-01\n' >> /etc/fridge-deepseek.env
grep -q '^FRIDGE_OTA_TARGET_PART=' /etc/fridge-deepseek.env ||
    printf 'FRIDGE_OTA_TARGET_PART=R7FA6M5BF2CBG\n' >> /etc/fridge-deepseek.env
grep -q '^FRIDGE_FIRMWARE_PUBLIC_KEY=' /etc/fridge-deepseek.env ||
    printf 'FRIDGE_FIRMWARE_PUBLIC_KEY=/etc/fridge-firmware-public.pem\n' \
        >> /etc/fridge-deepseek.env

chown root:root /etc/fridge-deepseek.env /etc/fridge-firmware-public.pem
chmod 600 /etc/fridge-deepseek.env
chmod 644 /etc/fridge-firmware-public.pem

for file in \
    fridge_deepseek_server.py \
    dashboard.py \
    firmware_ota.py \
    speech_pipeline.py \
    requirements.txt \
    test_dashboard.py \
    test_firmware_ota.py \
    test_ota_protocol.py \
    test_speech_pipeline.py \
    fridge-deepseek.env.example
do
    install -o fridge -g fridge -m 0644 \
        "$deploy_dir/$file" \
        "/opt/fridge-deepseek/$file"
done

install -d -o fridge -g fridge -m 0755 /opt/fridge-deepseek/web
cp -a "$deploy_dir/web/." /opt/fridge-deepseek/web/
chown -R fridge:fridge /opt/fridge-deepseek/web

install -o root -g root -m 0644 \
    "$deploy_dir/fridge-deepseek.service" \
    /etc/systemd/system/fridge-deepseek.service

systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/fridge-deepseek.service
systemctl restart fridge-deepseek.service

systemctl is-active fridge-deepseek.service
systemctl status fridge-deepseek.service --no-pager
journalctl -u fridge-deepseek.service -n 80 --no-pager
ss -lntp | grep -E ':10086|:10087'

curl -fsS http://127.0.0.1:10087/api/public/dashboard/overview
curl -fsS http://127.0.0.1:10087/api/public/firmware/releases

python3 - <<'PY'
import socket

with socket.create_connection(("127.0.0.1", 10086), timeout=5) as sock:
    sock.sendall(b"/ping\n")
    print("旧协议 /ping：", sock.recv(4096).decode("utf-8", errors="replace"))

with socket.create_connection(("127.0.0.1", 10086), timeout=5) as sock:
    sock.sendall(
        b"FW1 CHECK fridge-main-01 R7FA6M5BF2CBG "
        b"1.0.0+0 1.0.0+0 0 deliberately-wrong-token\n"
    )
    print("OTA 错误 Token：", sock.recv(4096).decode("utf-8", errors="replace"))
PY

printf '\n全部检查完成。\n'
printf '必须复制到 ota_update_config.h 的 FRIDGE_OTA_TOKEN：%s\n' "$ota_token"
printf '本次回滚备份：%s\n' "$backup_file"
```

预期结果：

- 单元测试显示 `Ran 61 tests` 和 `OK`。
- 服务状态为 `active (running)`。
- `10086` 和 `10087` 都处于 LISTEN。
- `/ping` 返回 `pong`。
- 错误 OTA Token 返回 `FW1 ERROR unauthorized`。

## 3. 回滚命令

只有新服务部署失败时才执行。把路径改成第二部分最后打印的真实备份：

```bash
set -Eeuo pipefail
backup_file=/var/backups/fridge-deepseek-before-ota-YYYYMMDD-HHMMSS.tar.gz
failed_dir="/opt/fridge-deepseek.failed-$(date +%Y%m%d-%H%M%S)"

systemctl stop fridge-deepseek.service
mv /opt/fridge-deepseek "$failed_dir"
tar -C /opt -xzf "$backup_file"
chown -R fridge:fridge /opt/fridge-deepseek
systemctl start fridge-deepseek.service
systemctl status fridge-deepseek.service --no-pager
```
