# CUPS AirPrint 打印服务（Brother DCP-7080D）

一个开箱即用的容器化打印服务：**CUPS + LibreOffice + 中文网页打印面板 + AirPrint 隔空打印**，
针对 **Brother DCP-7080D**（USB）预配置，默认 **双面 / A4 / 黑白**。

上传到 GitHub 后会通过 GitHub Actions 自动编译出 `linux/amd64` + `linux/arm64` 的 Docker 镜像（发布到 GHCR），斐讯 N1 / 飞牛 NAS 都能直接拉取运行。

## 功能

- 📱 **AirPrint 隔空打印**：内置 avahi，iPhone/iPad/Mac 自动发现打印机
- 🌐 **中文网页打印面板**：手机/PC 浏览器上传文件 → 自动转 PDF → 打印
  - 后端是**纯 C 零依赖** HTTP 服务（`web/server.c`，`gcc` 编译，无 Python/第三方库）
  - 支持 PDF / Word / Excel / PPT / 图片 / TXT 等（PDF 和图片原件直通打印不转换；Office 文档经 LibreOffice 转 PDF）
  - 双面/单面切换（默认**双面**）
  - 页码范围（留空=全部，如 `1-5 8`）
  - 预览（纵向/横向，A4）
- 🖨️ **打印机自动配置**：首次启动自动 `lpadmin` 配置 Brother DCP-7080D
  - 驱动：`drv:///brlaser.drv/br7080d.ppd`（开源 brlaser，无需官方驱动）
  - 默认：双面长边、A4、黑白
- 🀄 **中文字体**：Noto CJK + 文泉驿正黑，Word/Excel 中文不乱码

## 项目结构

```
cups-airprint/
├── .github/workflows/build.yml   # GitHub Actions 自动编译（amd64+arm64 → GHCR）
├── Dockerfile                    # Debian bookworm + CUPS + LibreOffice + brlaser
├── docker-compose.yml            # host 网络 + USB 直通
├── entrypoint.sh                 # 启动 dbus/avahi/cupsd + 自动配打印机 + 启动网页
├── cups/cupsd.conf               # CUPS 配置（631 端口、免登录、共享）
├── avahi/avahi-daemon.conf       # avahi 配置（AirPrint 广播）
├── scripts/setup-printer.sh      # 自动配置 Brother DCP-7080D（幂等）
└── web/
    ├── server.c                  # 纯 C 零依赖 HTTP 后端（gcc 编译，转换/打印 API）
    └── templates/index.html      # 中文打印面板（见截图样式）
```

## 上传 GitHub 自动编译

1. 在 GitHub 新建一个空仓库（如 `cups-airprint`）
2. 本地执行：

```bash
cd cups-airprint
git init
git add .
git commit -m "feat: cups airprint service for Brother DCP-7080D"
git branch -M main
git remote add origin https://github.com/<你的用户名>/cups-airprint.git
git push -u origin main
```

3. 推送后 GitHub Actions 自动构建双架构镜像并发布到：
   `ghcr.io/<你的用户名>/cups-airprint:latest`（仓库 Settings → Packages 可见，需设为 Public 或登录拉取）

## 在斐讯 N1（arm64）上部署

```bash
# 拉取（首次可用 NAS 中转，方法同既有习惯）
docker pull ghcr.io/<你的用户名>/cups-airprint:latest

mkdir -p /root/cups-airprint && cd /root/cups-airprint
# 上传 docker-compose.yml 后：
docker compose up -d

# 查看日志确认打印机配置成功
docker logs -f cups-print
```

> ⚠️ 如果 N1 上已有原生 CUPS（占 631 端口），请先停掉：`service cups stop`，否则端口冲突。

## 在飞牛 NAS（x86_64）上部署

同样 `docker compose up -d`，镜像会自动匹配 amd64 架构。

## 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `PORT` | `8080` | 网页面板端口（host 网络模式） |
| `PRINTER_NAME` | `Brother_DCP-7080D` | CUPS 队列名 |
| `PRINTER_DESC` | `Brother DCP-7080D` | 打印机描述 |

## 使用

- **网页打印**：浏览器打开 `http://<设备IP>:8080`，上传文件 →（可选改双面/页码范围）→ 点绿色打印按钮
- **AirPrint**：iPhone 打开任意可打印 App → 共享 → 打印，会自动发现 `Brother_DCP-7080D`
- **CUPS 后台**：浏览器打开 `http://<设备IP>:631`（容器内已免登录、打印机已共享）

## 常用排查命令

```bash
docker exec cups-print lpstat -p          # 打印机状态
docker exec cups-print lpstat -o          # 打印队列
docker exec cups-print lpinfo -v | grep usb   # 检测 USB 打印机
docker exec cups-print cupsenable Brother_DCP-7080D
docker exec cups-print cupsaccept Brother_DCP-7080D
```
