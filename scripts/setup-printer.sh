#!/bin/bash
# 自动配置 Brother DCP-7080D（幂等：已存在则跳过）
set -e

PRINTER_NAME="${PRINTER_NAME:-Brother_DCP-7080D}"
PRINTER_DESC="${PRINTER_DESC:-Brother DCP-7080D}"

if lpstat -p "$PRINTER_NAME" >/dev/null 2>&1; then
    echo "[setup] 打印机 $PRINTER_NAME 已存在，跳过配置"
    lpoptions -d "$PRINTER_NAME" >/dev/null 2>&1 || true
    exit 0
fi

echo "[setup] 检测 USB 打印机（重试 5 次）..."
DEV=""
for i in 1 2 3 4 5; do
    DEV=$(lpinfo -v 2>/dev/null | awk '/^usb:\/\/Brother\/DCP-7080D/{print $2; exit}')
    if [ -z "$DEV" ]; then
        # 退而求其次：任意 Brother USB 设备
        DEV=$(lpinfo -v 2>/dev/null | awk '/^usb:\/\/Brother/{print $2; exit}')
    fi
    [ -n "$DEV" ] && break
    sleep 4
done

if [ -z "$DEV" ]; then
    # 静态回退：lpadmin 不校验设备是否存在，usb backend 在打印时解析
    # 插上打印机/重启容器后即可用
    DEV="usb://Brother/DCP-7080D"
    echo "[setup] 未检测到 Brother USB 打印机，使用静态 URI 回退: $DEV"
    echo "[setup] 若打印不出，请检查 USB 线 / 容器 privileged 与 /dev/bus/usb 挂载"
fi
echo "[setup] 使用设备: $DEV"

# 配置：brlaser 开源驱动 + 默认双面长边/A4/黑白 + 共享(AirPrint)
lpadmin -p "$PRINTER_NAME" \
    -E \
    -v "$DEV" \
    -m drv:///brlaser.drv/br7080d.ppd \
    -D "$PRINTER_DESC" \
    -L "CUPS Print Server" \
    -o media=A4 \
    -o sides=two-sided-long-edge \
    -o print-color-mode=monochrome \
    -o printer-is-shared=true

cupsenable "$PRINTER_NAME"
cupsaccept "$PRINTER_NAME"
lpoptions -d "$PRINTER_NAME" >/dev/null

echo "[setup] 打印机配置完成: $PRINTER_NAME ($DEV)"
echo "[setup] 默认选项: A4 / 双面长边 / 黑白 / 已共享(AirPrint)"
