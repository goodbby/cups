#!/bin/bash
# 自动配置 Brother DCP-7080D（幂等：已存在则跳过）
set -e

PRINTER_NAME="${PRINTER_NAME:-Brother_DCP-7080D}"
PRINTER_DESC="${PRINTER_DESC:-Brother DCP-7080D}"

# ---------- 纸张收紧为仅 A4（幂等函数，两条路径都要执行） ----------
# iPhone 纸张面板读 media-ready(ReadyPaperSizes)，尺寸列表由 PPD 四张表决定，三处一起改
restrict_a4() {
    local PPD="/etc/cups/ppd/${PRINTER_NAME}.ppd"
    [ -f "$PPD" ] || return 0
    [ -f "${PPD}.bak-allsizes" ] || cp "$PPD" "${PPD}.bak-allsizes"
    local T
    for T in PageSize PageRegion ImageableArea PaperDimension; do
        sed -i -E "/^\*${T} /{ /^\*${T} A4\//!d }" "$PPD"
        sed -i -E "s/^\*Default${T}:.*/\*Default${T}: A4/" "$PPD"
    done
    if grep -qE "^[[:space:]]*ReadyPaperSizes" /etc/cups/cupsd.conf; then
        sed -i -E "s/^[[:space:]]*ReadyPaperSizes.*/ReadyPaperSizes A4/" /etc/cups/cupsd.conf
    else
        echo "ReadyPaperSizes A4" >> /etc/cups/cupsd.conf
    fi
    # PPD 改动需重启 cupsd 才生效（此时网页面板还没起，安全）
    pkill -x cupsd 2>/dev/null || true
    sleep 1
    cupsd
    local i
    for i in $(seq 1 15); do lpstat -r >/dev/null 2>&1 && break; sleep 1; done
    echo "[setup] 纸张已收紧为仅 A4"
}

if lpstat -p "$PRINTER_NAME" >/dev/null 2>&1; then
    echo "[setup] 打印机 $PRINTER_NAME 已存在，强制确保共享/启用/默认选项"
    # 手动添加的打印机可能未勾选共享 → iPhone 搜不到隔空打印，这里幂等强制
    lpadmin -p "$PRINTER_NAME" -o printer-is-shared=true \
        -o media=A4 -o sides=two-sided-long-edge -o print-color-mode=monochrome
    cupsenable "$PRINTER_NAME" 2>/dev/null || true
    cupsaccept "$PRINTER_NAME" 2>/dev/null || true
    lpoptions -d "$PRINTER_NAME" >/dev/null 2>&1 || true
    restrict_a4
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

restrict_a4
