#!/usr/bin/env bash
set -Eeuo pipefail

archive="/tmp/fridge-server-health-page-1.0.0+22.zip"
expected_sha="780BB2617E68F33611B564ADB0C84A9432C9C850D08DBA0B2317CB8286CA2714"
stamp="$(date +%Y%m%d-%H%M%S)"
deploy_dir="/tmp/fridge-health-page-${stamp}"
backup_file="/root/fridge-deepseek-before-health-page-${stamp}.tar.gz"
venv_python="/opt/fridge-deepseek/.venv/bin/python"

if [[ "${EUID}" -ne 0 ]]; then
    echo "ERROR: run this script as root."
    exit 1
fi

echo "[1/10] Verify uploaded package"
test -f "${archive}"
echo "${expected_sha}  ${archive}" | sha256sum -c -

echo "[2/10] Back up current deployment"
backup_items=(
    "opt/fridge-deepseek"
    "etc/systemd/system/fridge-deepseek.service"
)
if [[ -f /etc/fridge-deepseek.env ]]; then
    backup_items+=("etc/fridge-deepseek.env")
fi
tar -C / -czf "${backup_file}" "${backup_items[@]}"
echo "BACKUP_FILE=${backup_file}"

echo "[3/10] Extract staged release"
mkdir -p "${deploy_dir}"
python3 -m zipfile -e "${archive}" "${deploy_dir}"
chown -R fridge:fridge "${deploy_dir}"

echo "[4/10] Install/check Python dependencies"
runuser -u fridge -- "${venv_python}" -m pip install -r "${deploy_dir}/requirements.txt"

echo "[5/10] Run syntax checks"
runuser -u fridge -- "${venv_python}" -m py_compile "${deploy_dir}"/*.py

echo "[6/10] Run all server tests"
runuser -u fridge -- env PYTHONPATH="${deploy_dir}" \
    "${venv_python}" -m unittest discover \
    -s "${deploy_dir}" -p "test_*.py" -v

echo "[7/10] Install tested release"
install -o fridge -g fridge -m 0644 "${deploy_dir}"/*.py /opt/fridge-deepseek/
install -o fridge -g fridge -m 0644 \
    "${deploy_dir}/requirements.txt" \
    /opt/fridge-deepseek/requirements.txt
install -o fridge -g fridge -m 0644 \
    "${deploy_dir}/fridge-deepseek.env.example" \
    /opt/fridge-deepseek/fridge-deepseek.env.example

mkdir -p /opt/fridge-deepseek/web
cp -a "${deploy_dir}/web/." /opt/fridge-deepseek/web/
chown -R fridge:fridge /opt/fridge-deepseek/web

install -o root -g root -m 0644 \
    "${deploy_dir}/fridge-deepseek.service" \
    /etc/systemd/system/fridge-deepseek.service

grep -n "AUD1 HEALTH" /opt/fridge-deepseek/fridge_deepseek_server.py

echo "[8/10] Restart service"
systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/fridge-deepseek.service
systemctl restart fridge-deepseek.service
systemctl is-active --quiet fridge-deepseek.service

echo "[9/10] Check ports and TCP ping"
ss -lntp | grep -E ':10086|:10087'
"${venv_python}" -c 'import socket; s=socket.create_connection(("127.0.0.1",10086),5); s.sendall(b"/ping\n"); print(s.recv(4096).decode("utf-8",errors="replace")); s.close()'

echo "[10/10] Show service status and recent log"
systemctl --no-pager --full status fridge-deepseek.service
journalctl -u fridge-deepseek.service --since "3 minutes ago" --no-pager -n 100

echo "DEPLOYMENT_OK"
echo "BACKUP_FILE=${backup_file}"
