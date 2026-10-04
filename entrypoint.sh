#!/bin/bash
set -e

# ---------- 恢复默认配置（卷挂载为空时） ----------
if [ ! -f /etc/cups/cupsd.conf ]; then
    mkdir -p /etc/cups
    cp -r /opt/cups-default/. /etc/cups/
fi

mkdir -p /var/run/dbus /var/run/avahi-daemon /run/cups \
         /var/spool/cups /var/cache/cups /var/log/cups /tmp/print
chown -R lp:lp /var/spool/cups /var/cache/cups /var/log/cups 2>/dev/null || true
rm -f /run/cups/cups.sock 2>/dev/null || true

# ---------- dbus ----------
if [ -x /usr/bin/dbus-daemon ]; then
    rm -f /var/run/dbus/pid 2>/dev/null || true
    dbus-daemon --system --fork 2>/dev/null || true
fi

# ---------- avahi（AirPrint 广播） ----------
rm -f /var/run/avahi-daemon/pid 2>/dev/null || true
avahi-daemon --daemonize 2>/dev/null || echo "[entrypoint] avahi 启动失败，AirPrint 可能不可用"

# ---------- CUPS ----------
cupsd
for i in $(seq 1 30); do
    if lpstat -r >/dev/null 2>&1; then break; fi
    sleep 1
done
echo "[entrypoint] CUPS 已启动: $(lpstat -r 2>&1 || true)"

# ---------- 自动配置打印机（幂等） ----------
/app/scripts/setup-printer.sh || echo "[entrypoint] 打印机配置失败，可稍后手动执行 /app/scripts/setup-printer.sh"

# ---------- 网页面板 ----------
echo "[entrypoint] 网页面板启动于 0.0.0.0:${PORT:-8080}"
exec python3 -u /app/web/server.py
