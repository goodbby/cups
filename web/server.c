/*
 * cups-print-server.c — 纯 C 零依赖 HTTP 打印面板后端
 *
 * 功能（与旧版 Flask 版等价）：
 *   GET  /                      网页面板
 *   GET  /api/printers          列出 CUPS 打印机
 *   POST /api/upload            上传文件 -> 自动转 PDF（soffice / img2pdf / 直接拷贝）
 *   GET  /preview/<id>.pdf      预览生成的 PDF
 *   POST /api/print             调用 lp 打印（双面/页码范围/方向）
 *   GET  /api/health            健康检查
 *
 * 构建: gcc -O2 -Wall -o server server.c
 * 依赖: CUPS(lp/lpstat/lpadmin) + LibreOffice(soffice) + img2pdf + poppler-utils(pdfinfo)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <stdint.h>

/* ---------- 配置 ---------- */
#define MAX_BODY   (100LU * 1024 * 1024)   /* 上传上限 100MB */
#define READ_UNIT  65536
#define ID_LEN     12

static const char *WORK_DIR  = "/tmp/print";
static const char *TEMPLATE  = "/app/web/templates/index.html";

/* 可直接转 PDF 的图片类型 */
static const char *IMG_EXTS[] = {".jpg",".jpeg",".png",".bmp",".gif",".webp",".tif",".tiff",NULL};
/* LibreOffice 可转换的文档类型 */
static const char *DOC_EXTS[] = {".doc",".docx",".xls",".xlsx",".ppt",".pptx",
                                 ".odt",".ods",".odp",".rtf",".csv",".txt",".wps",".et",".dps",NULL};

/* ---------- 工具 ---------- */
static void *xmalloc(size_t n){ void *p = malloc(n); if(!p){ perror("malloc"); exit(1);} return p; }

static void send_all(int fd, const char *buf, size_t len){
    size_t off = 0;
    while(off < len){
        ssize_t w = write(fd, buf + off, len - off);
        if(w <= 0){ if(errno == EINTR) continue; return; }
        off += (size_t)w;
    }
}

/* 运行 shell 命令并捕获 stdout(含 2>&1)；返回退出码，输出写入 out */
static int run_pipe(const char *cmd, char *out, size_t outsz){
    FILE *f = popen(cmd, "r");
    if(!f) return -1;
    size_t n = 0;
    if(out && outsz){
        size_t r;
        while((r = fread(out + n, 1, outsz - n - 1, f)) > 0){
            n += r;
            if(n >= outsz - 1) break;
        }
        out[n] = '\0';
    } else {
        while(fgetc(f) != EOF){}
    }
    int st = pclose(f);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int has_suffix(const char *fn, const char *const *list){
    size_t fl = strlen(fn);
    for(int i = 0; list[i]; i++){
        size_t sl = strlen(list[i]);
        if(fl >= sl && strcasecmp(fn + fl - sl, list[i]) == 0) return 1;
    }
    return 0;
}

/* 12 位小写 hex id */
static void gen_id(char *out){
    static const char hex[] = "0123456789abcdef";
    srand((unsigned)(time(NULL) ^ (getpid() << 8) ^ (getpid() >> 3)));
    for(int i = 0; i < ID_LEN; i++) out[i] = hex[rand() & 0xf];
    out[ID_LEN] = '\0';
}

/* ---------- HTTP 回复 ---------- */
static void http_reply(int fd, int code, const char *status, const char *ctype,
                       const char *body, size_t blen){
    char h[512];
    int hl = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", code, status, ctype, blen);
    send_all(fd, h, (size_t)hl);
    send_all(fd, body, blen);
}

static void http_file(int fd, const char *path, const char *ctype){
    FILE *f = fopen(path, "rb");
    if(!f){ http_reply(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found", 9); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if(sz < 0){ fclose(f); http_reply(fd, 500, "Error", "text/plain; charset=utf-8", "Error", 5); return; }
    char *buf = xmalloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f); fclose(f);
    http_reply(fd, 200, "OK", ctype, buf, rd);
    free(buf);
}

/* ---------- 请求解析 ---------- */
static long get_content_length(const char *hdr, size_t hlen){
    const char *p = hdr;
    const char *end = hdr + hlen;
    while((p = memmem(p, (size_t)(end - p), "Content-Length:", 15)) != NULL){
        p += 15;
        while(p < end && (*p == ' ' || *p == '\t')) p++;
        if(p >= end) break;
        return strtol(p, NULL, 10);
    }
    return -1;
}

static void get_header_value(const char *hdr, size_t hlen, const char *name, char *out, size_t outsz){
    out[0] = '\0';
    const char *p = hdr;
    const char *end = hdr + hlen;
    while((p = memmem(p, (size_t)(end - p), name, strlen(name))) != NULL){
        p += strlen(name);
        while(p < end && (*p == ' ' || *p == '\t')) p++;
        if(p < end && *p == ':'){
            p++;
            while(p < end && (*p == ' ' || *p == '\t')) p++;
            size_t i = 0;
            while(p < end && *p != '\r' && *p != '\n' && i + 1 < outsz) out[i++] = *p++;
            out[i] = '\0';
            return;
        }
    }
}

/* 简易 JSON 取值 */
static int json_str(const char *b, const char *key, char *out, size_t outsz){
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(b, pat);
    if(!p) return 0;
    p += strlen(pat);
    while(*p && (*p == ' ' || *p == ':')) p++;
    if(*p != '"') return 0;
    p++; size_t i = 0;
    while(*p && *p != '"' && i + 1 < outsz){
        if(*p == '\\' && p[1]){ p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}
static int json_bool(const char *b, const char *key, int def){
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(b, pat);
    if(!p) return def;
    p += strlen(pat);
    while(*p && (*p == ' ' || *p == ':')) p++;
    if(strncmp(p, "true", 4) == 0) return 1;
    if(strncmp(p, "false", 5) == 0) return 0;
    return def;
}

static int valid_hex12(const char *s){
    if(!s || strlen(s) != ID_LEN) return 0;
    for(int i = 0; i < ID_LEN; i++)
        if(!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* JSON 字符串转义（引号/反斜杠/控制字符），返回写入长度 */
static size_t json_escape(const char *in, char *out, size_t outsz){
    size_t j = 0;
    for(const unsigned char *p = (const unsigned char *)in; *p && j + 2 < outsz; p++){
        unsigned char c = *p;
        if(c == '"' || c == '\\'){ out[j++] = '\\'; out[j++] = (char)c; }
        else if(c == '\n'){ out[j++] = '\\'; out[j++] = 'n'; }
        else if(c == '\r'){ out[j++] = '\\'; out[j++] = 'r'; }
        else if(c == '\t'){ out[j++] = '\\'; out[j++] = 't'; }
        else if(c >= 0x20){ out[j++] = (char)c; }
        /* 其他控制字符直接丢弃 */
    }
    out[j] = '\0';
    return j;
}

/* ---------- 转换 ---------- */
static int pdf_pages(const char *pdf){
    char cmd[2048]; snprintf(cmd, sizeof cmd, "pdfinfo '%s' 2>/dev/null", pdf);
    char out[4096]; out[0] = '\0';
    run_pipe(cmd, out, sizeof out);
    char *p = strstr(out, "Pages:");
    if(p){ return atoi(p + 6); }
    return 0;
}

/* 把上传文件转成 PDF。返回 0 成功，-1 失败(错误信息写入 err) */
static int convert_to_pdf(const char *upload, const char *ext, const char *pdf, char *err, size_t errsz){
    int rc;
    if(strcasecmp(ext, ".pdf") == 0){
        char cmd[2048]; snprintf(cmd, sizeof cmd, "cp '%s' '%s'", upload, pdf);
        rc = system(cmd);
    } else if(has_suffix(ext, IMG_EXTS)){
        char cmd[2048]; snprintf(cmd, sizeof cmd, "img2pdf '%s' -o '%s' 2>&1", upload, pdf);
        char out[2048]; out[0] = '\0';
        rc = run_pipe(cmd, out, sizeof out);
        if(rc != 0 || access(pdf, R_OK) != 0){
            snprintf(err, errsz, "图片转PDF失败: %s", out); return -1;
        }
        return 0;
    } else if(has_suffix(ext, DOC_EXTS)){
        char dir[2048]; snprintf(dir, sizeof dir, "%s", pdf);
        char *sl = strrchr(dir, '/'); if(sl) *sl = '\0';
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
            "HOME=/tmp soffice --headless --norestore --convert-to pdf --outdir '%s' '%s' 2>&1",
            dir, upload);
        char out[2048]; out[0] = '\0';
        run_pipe(cmd, out, sizeof out);
        /* soffice 退出码不可靠（javaldx 警告等也会导致非0），以产物文件为准 */
        if(access(pdf, R_OK) != 0){
            snprintf(err, errsz, "文档转PDF失败(LibreOffice): %s", out); return -1;
        }
        return 0;
    } else {
        snprintf(err, errsz, "不支持的文件类型: %s", ext);
        return -1;
    }
    if(rc != 0){ snprintf(err, errsz, "PDF 处理失败"); return -1; }
    return 0;
}

/* ---------- 处理上传 (multipart) ---------- */
static void handle_upload(int fd, char *body, size_t bodylen, const char *boundary){
    char delim[300]; snprintf(delim, sizeof delim, "--%s", boundary);

    char *p = memmem(body, bodylen, delim, strlen(delim));
    if(!p){ http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                       "{\"ok\":false,\"error\":\"未找到 multipart 边界\"}", 38); return; }

    char *hdrend = memmem(p, bodylen - (size_t)(p - body), "\r\n\r\n", 4);
    if(!hdrend){ http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                            "{\"ok\":false,\"error\":\"解析 part 头失败\"}", 35); return; }

    char orig_name[256]; orig_name[0] = '\0';
    char *fn = memmem(p, (size_t)(hdrend - p), "filename=\"", 10);
    if(fn){
        fn += 10;
        size_t i = 0;
        while(*fn && *fn != '"' && i + 1 < sizeof orig_name) orig_name[i++] = *fn++;
        orig_name[i] = '\0';
    }

    char *cstart = hdrend + 4;
    char crlfdelim[300]; snprintf(crlfdelim, sizeof crlfdelim, "\r\n--%s", boundary);
    char *cend = memmem(cstart, bodylen - (size_t)(cstart - body), crlfdelim, strlen(crlfdelim));
    if(!cend){ http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                          "{\"ok\":false,\"error\":\"未找到 part 结束\"}", 35); return; }

    /* 扩展名 */
    char ext[16]; ext[0] = '\0';
    char *dot = strrchr(orig_name, '.');
    if(dot && strlen(dot) <= 12) snprintf(ext, sizeof ext, "%s", dot);

    char id[ID_LEN + 1]; gen_id(id);

    char upload[1024], pdf[1024];
    snprintf(upload, sizeof upload, "%s/uploads/%s%s", WORK_DIR, id, ext[0] ? ext : ".bin");
    snprintf(pdf,   sizeof pdf,   "%s/pdf/%s.pdf",      WORK_DIR, id);

    FILE *f = fopen(upload, "wb");
    if(!f){ http_reply(fd, 500, "Error", "application/json; charset=utf-8",
                       "{\"ok\":false,\"error\":\"写入临时文件失败\"}", 36); return; }
    fwrite(cstart, 1, (size_t)(cend - cstart), f); fclose(f);

    char err[1024]; err[0] = '\0';
    int rc = convert_to_pdf(upload, ext[0] ? ext : ".bin", pdf, err, sizeof err);
    unlink(upload);

    if(rc != 0){
        char esc[1024]; json_escape(err[0] ? err : "转换失败", esc, sizeof esc);
        char buf[1400]; int n = snprintf(buf, sizeof buf,
            "{\"ok\":false,\"error\":\"%s\"}", esc);
        http_reply(fd, 500, "Error", "application/json; charset=utf-8", buf, (size_t)n);
        return;
    }

    int pages = pdf_pages(pdf);
    char esc_name[600]; json_escape(orig_name, esc_name, sizeof esc_name);
    char buf[2048];
    int n = snprintf(buf, sizeof buf,
        "{\"ok\":true,\"file_id\":\"%s\",\"name\":\"%s\",\"pages\":%d,\"pdf_url\":\"/preview/%s.pdf\"}",
        id, esc_name, pages, id);
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", buf, (size_t)n);
}

/* ---------- 打印机列表 ---------- */
static void handle_printers(int fd){
    char out[8192]; out[0] = '\0';
    run_pipe("LC_ALL=C lpstat -p 2>/dev/null", out, sizeof out);
    char def[256]; def[0] = '\0';
    char dout[1024]; dout[0] = '\0';
    run_pipe("LC_ALL=C lpstat -d 2>/dev/null", dout, sizeof dout);
    char *dp = strstr(dout, "system default destination: ");
    if(dp){
        char *n = dp + 26;
        while(*n && (*n == ' ' || *n == ':' || *n == '\t')) n++;
        snprintf(def, sizeof def, "%s", n);
    }
    char *sp = strchr(def, '\n'); if(sp) *sp = '\0';

    /* 构造 JSON 数组 */
    char *json = xmalloc(16384);
    int jl = snprintf(json, 16384, "{\"ok\":true,\"printers\":[");
    char *line = strtok(out, "\n");
    int first = 1;
    while(line){
        char pname[256]; pname[0] = '\0';
        if(sscanf(line, "printer %255s", pname) == 1){
            int isdef = (def[0] && strcmp(pname, def) == 0);
            jl += snprintf(json + jl, 16384 - (size_t)jl,
                "%s{\"name\":\"%s\",\"default\":%s}", first ? "" : ",",
                pname, isdef ? "true" : "false");
            first = 0;
        }
        line = strtok(NULL, "\n");
    }
    jl += snprintf(json + jl, 16384 - (size_t)jl, "]}");
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)jl);
    free(json);
}

/* ---------- 打印 ---------- */
static void handle_print(int fd, char *body, size_t bodylen){
    (void)bodylen;
    char file_id[64], printer[256], pages[256], orient[64];
    file_id[0] = printer[0] = pages[0] = orient[0] = '\0';
    json_str(body, "file_id", file_id, sizeof file_id);
    json_str(body, "printer", printer, sizeof printer);
    json_str(body, "pages", pages, sizeof pages);
    json_str(body, "orientation", orient, sizeof orient);
    int duplex = json_bool(body, "duplex", 1);

    if(!valid_hex12(file_id)){
        http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                   "{\"ok\":false,\"error\":\"无效的文件\"}", 31); return;
    }
    /* 打印机名只允许字母数字 - _（防止命令注入） */
    for(char *q = printer; *q; q++){
        if(!isalnum((unsigned char)*q) && *q != '-' && *q != '_'){
            http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                       "{\"ok\":false,\"error\":\"无效的打印机名\"}", 33); return;
        }
    }
    char pdf[1024]; snprintf(pdf, sizeof pdf, "%s/pdf/%s.pdf", WORK_DIR, file_id);
    if(access(pdf, R_OK) != 0){
        http_reply(fd, 404, "Not Found", "application/json; charset=utf-8",
                   "{\"ok\":false,\"error\":\"文件不存在，请重新上传\"}", 39); return;
    }

    /* 校验页码范围格式: 1-5 8 / 1-5,8 */
    char norm[256]; norm[0] = '\0';
    if(pages[0]){
        size_t j = 0;
        for(size_t i = 0; pages[i] && j + 1 < sizeof norm; i++){
            /* 全角逗号（U+FF0C，UTF-8: EF BC 8C）按半角逗号处理 */
            if((unsigned char)pages[i] == 0xEF && (unsigned char)pages[i+1] == 0xBC
               && (unsigned char)pages[i+2] == 0x8C){
                norm[j++] = ','; i += 2;
            }
            else if(pages[i] == ',' || pages[i] == ' ') norm[j++] = ',';
            else norm[j++] = pages[i];
        }
        norm[j] = '\0';

        int ok = 1;
        if(norm[0] == '\0' || norm[strlen(norm) - 1] == ',' || strstr(norm, ",,") ||
           strspn(norm, "0123456789,-") != strlen(norm)){
            ok = 0;
        } else {
            char *tok = strtok(norm, ",");
            while(tok){
                if(strchr(tok, '-')){
                    int a = atoi(tok), b = atoi(strchr(tok, '-') + 1);
                    if(a <= 0 || b <= 0 || a > b){ ok = 0; break; }
                } else if(atoi(tok) <= 0){ ok = 0; break; }
                tok = strtok(NULL, ",");
            }
        }
        if(!ok){
            http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                       "{\"ok\":false,\"error\":\"页码范围格式不正确，如：1-5 8\"}", 47); return;
        }
    }

    /* 组装 lp 命令 */
    char cmd[2048];
    int cl = snprintf(cmd, sizeof cmd, "LC_ALL=C lp%s%s -o media=A4 -o %s -o print-color-mode=monochrome",
                      printer[0] ? " -d " : " ", printer[0] ? printer : "",
                      duplex ? "sides=two-sided-long-edge" : "sides=one-sided");
    if(norm[0]) cl += snprintf(cmd + cl, sizeof cmd - (size_t)cl, " -o page-ranges=%s", norm);
    snprintf(cmd + cl, sizeof cmd - (size_t)cl, " -o orientation-requested=%s '%s'",
             (orient[0] && strcmp(orient, "landscape") == 0) ? "4" : "3", pdf);

    char out[1024]; out[0] = '\0';
    int rc = run_pipe(cmd, out, sizeof out);
    if(rc != 0){
        char esc[1024]; json_escape(out[0] ? out : "lp 返回非0", esc, sizeof esc);
        char buf[1400]; int n = snprintf(buf, sizeof buf,
            "{\"ok\":false,\"error\":\"打印失败: %s\"}", esc);
        http_reply(fd, 500, "Error", "application/json; charset=utf-8", buf, (size_t)n);
        return;
    }
    char job[128]; job[0] = '\0';
    char *q = strstr(out, "request id is ");
    if(q) snprintf(job, sizeof job, "%s", q + 14);
    char *nl = strpbrk(job, "\r\n"); if(nl) *nl = '\0';

    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "{\"ok\":true,\"job_id\":\"%s\",\"message\":\"已发送到打印队列\"}", job);
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", buf, (size_t)n);
}

/* ---------- 主请求分发 ---------- */
static void handle_conn(int fd){
    char *buf = xmalloc(READ_UNIT);
    size_t cap = READ_UNIT, len = 0;
    ssize_t n;
    int complete = 0;
    while((n = read(fd, buf + len, cap - len)) > 0){
        len += (size_t)n;
        char *he = memmem(buf, len, "\r\n\r\n", 4);
        if(he){
            size_t hlen = (size_t)(he - buf) + 4;
            long cl = get_content_length(buf, hlen);
            if(cl < 0){ complete = 1; break; }
            if(len >= hlen + (size_t)cl){ complete = 1; break; }
        }
        if(len == cap){ cap *= 2; buf = realloc(buf, cap); if(!buf) return; }
        if(cap >= MAX_BODY + READ_UNIT) break;
    }
    if(!complete){ free(buf); return; }

    /* 解析请求行 + 路径 */
    char method[16], path[1024];
    if(sscanf(buf, "%15s %1023s", method, path) != 2){ free(buf); return; }

    char *he = memmem(buf, len, "\r\n\r\n", 4);
    size_t hlen = (size_t)(he - buf) + 4;
    char *body = buf + hlen;
    size_t bodylen = len - hlen;

    if(strcmp(method, "GET") == 0){
        if(strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0){
            http_file(fd, TEMPLATE, "text/html; charset=utf-8");
        } else if(strcmp(path, "/api/printers") == 0){
            handle_printers(fd);
        } else if(strcmp(path, "/api/health") == 0){
            http_reply(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true}", 11);
        } else if(strncmp(path, "/preview/", 9) == 0){
            char *pid = path + 9;
            size_t pl = strlen(pid);
            if(pl == ID_LEN + 4 && strcmp(pid + ID_LEN, ".pdf") == 0){
                char id13[ID_LEN + 1];
                memcpy(id13, pid, ID_LEN); id13[ID_LEN] = '\0';
                if(valid_hex12(id13)){
                    char pdf[2048];
                    snprintf(pdf, sizeof pdf, "%s/pdf/%s.pdf", WORK_DIR, id13);
                    http_file(fd, pdf, "application/pdf");
                } else {
                    http_reply(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found", 9);
                }
            } else {
                http_reply(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found", 9);
            }
        } else {
            http_reply(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found", 9);
        }
    } else if(strcmp(method, "POST") == 0){
        if(strcmp(path, "/api/upload") == 0){
            char ct[256]; ct[0] = '\0';
            get_header_value(buf, hlen, "Content-Type", ct, sizeof ct);
            char *b = strcasestr(ct, "boundary=");
            if(!b){ http_reply(fd, 400, "Bad Request", "application/json; charset=utf-8",
                               "{\"ok\":false,\"error\":\"缺少 boundary\"}", 31); free(buf); return; }
            b += 9;
            char boundary[256]; size_t i = 0;
            while(*b && *b != ';' && *b != ' ' && *b != '\r' && i + 1 < sizeof boundary) boundary[i++] = *b++;
            boundary[i] = '\0';
            handle_upload(fd, body, bodylen, boundary);
        } else if(strcmp(path, "/api/print") == 0){
            /* 取 body 文本（去掉可能的 charset 差异，直接当字符串） */
            char *txt = xmalloc(bodylen + 1);
            memcpy(txt, body, bodylen); txt[bodylen] = '\0';
            handle_print(fd, txt, bodylen);
            free(txt);
        } else {
            http_reply(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found", 9);
        }
    } else {
        http_reply(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Method Not Allowed", 17);
    }
    free(buf);
}

static void reap(int sig){ (void)sig; while(waitpid(-1, NULL, WNOHANG) > 0){} }

int main(void){
    int port = 8080;
    char *p = getenv("PORT"); if(p) port = atoi(p);

    mkdir(WORK_DIR, 0755);
    char d1[1024], d2[1024];
    snprintf(d1, sizeof d1, "%s/uploads", WORK_DIR);
    snprintf(d2, sizeof d2, "%s/pdf", WORK_DIR);
    mkdir(d1, 0755); mkdir(d2, 0755);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if(srv < 0){ perror("socket"); return 1; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if(bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0){ perror("bind"); return 1; }
    if(listen(srv, 16) < 0){ perror("listen"); return 1; }

    signal(SIGCHLD, reap);
    signal(SIGPIPE, SIG_IGN);
    printf("[server] 网页面板启动于 0.0.0.0:%d\n", port); fflush(stdout);

    while(1){
        int c = accept(srv, NULL, NULL);
        if(c < 0){ if(errno == EINTR) continue; continue; }
        pid_t pid = fork();
        if(pid == 0){
            close(srv);
            handle_conn(c);
            close(c);
            _exit(0);
        } else if(pid > 0){
            close(c);
        } else {
            close(c);
        }
    }
    return 0;
}
