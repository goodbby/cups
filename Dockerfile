FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=zh_CN.UTF-8 \
    LC_ALL=zh_CN.UTF-8

# CUPS + Brother 开源驱动(brlaser) + AirPrint(avahi) + LibreOffice(转PDF) + 中文字体
RUN apt-get update && apt-get install -y --no-install-recommends \
        cups cups-client cups-bsd cups-filters \
        printer-driver-brlaser \
        avahi-daemon avahi-utils dbus libnss-mdns \
        libreoffice-writer libreoffice-calc libreoffice-impress \
        fonts-noto-cjk fonts-wqy-zenhei \
        poppler-utils img2pdf qpdf \
        gcc libc6-dev \
        locales ca-certificates procps \
    && sed -i 's/^# *zh_CN.UTF-8/zh_CN.UTF-8/' /etc/locale.gen \
    && locale-gen \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# 配置文件
COPY cups/cupsd.conf /etc/cups/cupsd.conf
COPY avahi/avahi-daemon.conf /etc/avahi/avahi-daemon.conf

# 保留一份 CUPS 默认配置（卷挂载 /etc/cups 为空时可恢复）
RUN cp -r /etc/cups /opt/cups-default

# 应用：C 后端编译 + 脚本
COPY scripts /app/scripts
COPY web /app/web
COPY entrypoint.sh /app/entrypoint.sh
RUN gcc -O2 -Wall -o /app/web/server /app/web/server.c
RUN chmod +x /app/entrypoint.sh /app/scripts/setup-printer.sh /app/web/server \
    && mkdir -p /var/spool/cups /var/cache/cups /var/log/cups \
    && chown -R lp:lp /var/spool/cups /var/cache/cups /var/log/cups

EXPOSE 631 8080

ENTRYPOINT ["/app/entrypoint.sh"]
