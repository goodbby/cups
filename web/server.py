#!/usr/bin/env python3
"""CUPS 网页打印面板后端（Flask）
上传文件 → 自动转 PDF → 调用 lp 打印（支持双面/页码范围/方向）。
"""
import os
import re
import shutil
import subprocess
import uuid
from pathlib import Path

from flask import Flask, jsonify, render_template, request, send_file

PORT = int(os.environ.get("PORT", "8080"))
WORK = Path(os.environ.get("WORK_DIR", "/tmp/print"))
PDF_DIR = WORK / "pdf"
UPLOAD_DIR = WORK / "uploads"
PDF_DIR.mkdir(parents=True, exist_ok=True)
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)

# LibreOffice 可转换的类型
OFFICE_EXTS = {
    ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
    ".odt", ".ods", ".odp", ".rtf", ".csv", ".txt", ".wps", ".et", ".dps",
}
# 直接转 PDF 的图片类型
IMG_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff"}

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 100 * 1024 * 1024  # 100MB


def run(cmd, timeout=180, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)


def pdf_pages(pdf: Path) -> int:
    try:
        r = run(["pdfinfo", str(pdf)], timeout=30)
        m = re.search(r"Pages:\s+(\d+)", r.stdout)
        return int(m.group(1)) if m else 0
    except Exception:
        return 0


def list_printers():
    r = run(["lpstat", "-p"], timeout=15)
    printers = []
    for line in r.stdout.splitlines():
        m = re.match(r"printer (\S+) (.*)", line.strip())
        if m:
            printers.append({"name": m.group(1), "state": m.group(2)})
    r2 = run(["lpstat", "-d"], timeout=15)
    default = None
    m = re.search(r"system default destination: (\S+)", r2.stdout)
    if m:
        default = m.group(1)
    for p in printers:
        p["default"] = p["name"] == default
    return printers


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/api/printers")
def api_printers():
    try:
        return jsonify({"ok": True, "printers": list_printers()})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e), "printers": []})


@app.post("/api/upload")
def api_upload():
    f = request.files.get("file")
    if not f or not f.filename:
        return jsonify({"ok": False, "error": "未收到文件"}), 400

    name = Path(f.filename).name
    ext = Path(name).suffix.lower()
    file_id = uuid.uuid4().hex[:12]
    src = UPLOAD_DIR / f"{file_id}_{name}"
    f.save(str(src))
    pdf_path = PDF_DIR / f"{file_id}.pdf"

    try:
        if ext == ".pdf":
            shutil.copy(str(src), str(pdf_path))
        elif ext in IMG_EXTS:
            r = run(["img2pdf", str(src), "-o", str(pdf_path)], timeout=120)
            if r.returncode != 0 or not pdf_path.exists():
                raise RuntimeError("图片转 PDF 失败: " + (r.stderr or "").strip()[:200])
        elif ext in OFFICE_EXTS:
            env = dict(os.environ, HOME="/tmp")
            r = run(["soffice", "--headless", "--norestore", "--convert-to",
                     "pdf", "--outdir", str(PDF_DIR), str(src)], timeout=300, env=env)
            produced = PDF_DIR / (src.stem + ".pdf")
            if r.returncode != 0 or not produced.exists():
                raise RuntimeError("文档转 PDF 失败（LibreOffice）")
            produced.rename(str(pdf_path))
        else:
            return jsonify({"ok": False, "error": f"不支持的文件类型: {ext or '未知'}"}), 400

        return jsonify({
            "ok": True,
            "file_id": file_id,
            "name": name,
            "pages": pdf_pages(pdf_path),
            "pdf_url": f"/preview/{file_id}.pdf",
        })
    except subprocess.TimeoutExpired:
        return jsonify({"ok": False, "error": "转换超时，请重试"}), 500
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500
    finally:
        try:
            src.unlink()
        except OSError:
            pass


@app.get("/preview/<file_id>.pdf")
def api_preview(file_id):
    path = PDF_DIR / f"{file_id}.pdf"
    if not path.is_file() or not re.fullmatch(r"[0-9a-f]{12}", file_id):
        return "Not Found", 404
    return send_file(str(path), mimetype="application/pdf")


@app.post("/api/print")
def api_print():
    data = request.get_json(force=True, silent=True) or {}
    file_id = data.get("file_id", "")
    printer = data.get("printer") or ""
    duplex = bool(data.get("duplex", True))
    pages = (data.get("pages") or "").strip()
    orientation = data.get("orientation") or "portrait"

    if not re.fullmatch(r"[0-9a-f]{12}", file_id):
        return jsonify({"ok": False, "error": "无效的文件"}), 400
    pdf_path = PDF_DIR / f"{file_id}.pdf"
    if not pdf_path.is_file():
        return jsonify({"ok": False, "error": "文件不存在，请重新上传"}), 404

    cmd = ["lp", "-d", printer] if printer else ["lp"]
    cmd += ["-o", "media=A4"]
    cmd += ["-o", "sides=two-sided-long-edge" if duplex else "sides=one-sided"]
    cmd += ["-o", "print-color-mode=monochrome"]
    if pages:
        # 规范化页码：允许 "1-5 8" / "1-5,8" / "1,3-5"
        norm = re.sub(r"[,，\s]+", ",", pages)
        if not re.fullmatch(r"\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*", norm):
            return jsonify({"ok": False, "error": "页码范围格式不正确，如：1-5 8"}), 400
        cmd += ["-o", f"page-ranges={norm}"]
    cmd += ["-o", "orientation-requested=" + ("4" if orientation == "landscape" else "3")]
    cmd += [str(pdf_path)]

    r = run(cmd, timeout=60)
    if r.returncode != 0:
        return jsonify({"ok": False, "error": "打印失败: " + (r.stderr or r.stdout).strip()[:200]}), 500
    m = re.search(r"request id is (\S+)", r.stdout)
    return jsonify({"ok": True, "job_id": m.group(1) if m else "", "message": "已发送到打印队列"})


@app.get("/api/health")
def api_health():
    return jsonify({"ok": True})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT, threaded=True)
