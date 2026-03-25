# TinyServe

[English](README.md) | 中文

轻量级单二进制 HTTP 文件服务器 / Stub HTTP 服务器 / TCP 端口转发器，纯 C11 + [libuv](https://libuv.org/)。

比 `python -m http.server` 更快、更轻、更全面。

## 功能

- **文件服务器** (`-m f`)：目录浏览、文件下载、Range/206（单段 & 多段）、MIME 类型、keep-alive、路径穿越防护
- **Stub HTTP 服务器** (`-m s`)：基于配置文件的路由 mock 服务器，支持 GET/POST/HEAD
- **TCP 端口转发** (`-m p`)：双向字节流代理，带背压控制和 TCP 半关闭支持
- **鉴权**：可选 Basic Auth 和/或自定义 Header Auth（OR 逻辑），CLI 严格校验
- **全异步 I/O**：单线程 libuv 事件循环，异步文件打开/读取/关闭，无阻塞
- **HTTP/1.0 & HTTP/1.1**：响应使用客户端请求的 HTTP 版本，正确处理 keep-alive 和 pipelining
- **HEAD 支持**：所有响应路径（200、206、301、401、404、405、416）对 HEAD 请求正确抑制 body
- **安全目录列表**：HTML 转义显示名、URL 编码 href，防 XSS
- **IPv4 & IPv6**：双栈支持，可绑定 `127.0.0.1`、`::1`、`0.0.0.0`、`::` 等

## 构建

```bash
# 依赖：cmake >= 3.16, C11 编译器, libuv
# macOS: brew install libuv cmake
# Ubuntu/Debian: apt install libuv1-dev cmake build-essential

cmake -S . -B build
cmake --build build

# 产物: ./build/tinyserve
```

## 用法

```
tinyserve [选项]
```

### 选项

| 标志 | 说明 | 默认值 |
|------|------|--------|
| `-m` | 模式：`f`(文件)、`s`(stub HTTP)、`p`(代理) | `f` |
| `-a` | 绑定地址 | `127.0.0.1` |
| `-p` | 监听端口（1-65535，严格校验） | `8000` |
| `-d` | 根目录（文件模式） | `.` |
| `-c` | 路由配置文件（stub 模式） | — |
| `-u` | Basic Auth 用户名（需配合 `-w`） | — |
| `-w` | Basic Auth 密码（需配合 `-u`） | — |
| `-k` | Header Auth 头名称（需配合 `-v`） | — |
| `-v` | Header Auth 期望值（需配合 `-k`） | — |
| `-t` | 代理目标主机 | — |
| `-q` | 代理目标端口（1-65535，严格校验） | — |
| `-l` | 日志级别：`error`、`warn`、`info` | `info` |
| `-h` | 显示帮助 | — |

## 模式示例

### 文件服务器 (`-m f`)

```bash
# 在 8000 端口提供当前目录
./build/tinyserve

# IPv6 本地
./build/tinyserve -m f -d /var/www -a ::1 -p 3000

# 绑定所有接口
./build/tinyserve -m f -d ./public -a :: -p 8080

# 带 Basic Auth
./build/tinyserve -m f -d ./public -u admin -w secret
```

### Stub HTTP 服务器 (`-m s`)

```bash
./build/tinyserve -m s -c routes.conf -p 9000
```

路由配置格式（`routes.conf`）：

```
---
method = GET
path = /hello
status = 200
content-type = text/plain
body = Hello, World!

---
method = POST
path = /api/data
status = 201
content-type = application/json
header = X-Request-Id: abc123
body = {"created": true}
```

### TCP 端口转发 (`-m p`)

```bash
# 将本地 8080 转发到 remote-host:3000
./build/tinyserve -m p -p 8080 -t remote-host -q 3000

# 转发到本地服务
./build/tinyserve -m p -p 9090 -t 127.0.0.1 -q 5432
```

## 鉴权

默认**关闭**。提供凭据即可启用：

- `-u admin -w password` → Basic Auth
- `-k X-API-Key -v secret` → 自定义 Header Auth
- 同时配置 → 任一通过即可（OR 逻辑）

启动时校验配对完整性：只给 `-u` 不给 `-w`（或只给 `-k` 不给 `-v`）会直接报错退出。

鉴权适用于文件模式和 stub 模式。代理模式忽略鉴权（纯 TCP 转发）。

## curl 验证示例

### HEAD 请求

所有状态码的 HEAD 响应均只返回 header，不返回 body：

```bash
curl -I http://127.0.0.1:8000/file.txt
# HTTP/1.1 200 OK / Content-Length: 1234 / (无 body)

curl -I http://127.0.0.1:8000/missing
# HTTP/1.1 404 Not Found / Content-Length: 25 / (无 body)

curl -I -H "Range: bytes=0-99" http://127.0.0.1:8000/file.txt
# HTTP/1.1 206 Partial Content / (无 body)

curl -I -H "Range: bytes=0-9,20-29" http://127.0.0.1:8000/file.txt
# HTTP/1.1 206 Partial Content / multipart / (无 body，无 part header 泄漏)
```

### 401 / 404 / 416

```bash
# 401
curl -I http://127.0.0.1:8000/          # 未带凭据 → 401 + WWW-Authenticate
curl -u admin:secret http://127.0.0.1:8000/  # 凭据正确 → 200

# 404
curl http://127.0.0.1:8000/does-not-exist
# Nothing Found in the PATH

# 416
curl -H "Range: bytes=999999-" -i http://127.0.0.1:8000/smallfile.txt
# HTTP/1.1 416 Range Not Satisfiable / Content-Range: bytes */16
```

### Content-Length 校验

非法 `Content-Length` 返回 400 Bad Request：

```bash
curl -H "Content-Length: -1" http://127.0.0.1:8000/           # 400
curl -H "Content-Length: abc" http://127.0.0.1:8000/           # 400
curl -H "Content-Length: 99999999999999999999" http://127.0.0.1:8000/  # 400
```

### 代理模式

```bash
# 终端 1: 启动后端
./build/tinyserve -m s -c routes.conf -p 9000

# 终端 2: 启动代理（支持 TCP 半关闭）
./build/tinyserve -m p -p 8080 -t 127.0.0.1 -q 9000

# 终端 3: 通过代理访问
curl http://127.0.0.1:8080/hello
# Hello, World!
```

## 项目结构

```
src/
├── main.c             # 入口
├── tinyserve.h        # 共享类型和常量
├── server.h/c         # libuv 事件循环、连接管理、写完成追踪
├── config.h/c         # CLI 参数解析（严格校验）
├── http_parser.h/c    # HTTP/1.1 请求解析器（严格 Content-Length 校验）
├── http_response.h/c  # HTTP 响应构建器（HEAD 支持、写追踪）
├── auth.h/c           # Basic Auth + Header Auth
├── file_serve.h/c     # 文件服务模式（安全目录列表）
├── range.h/c          # Range/206 处理（溢出保护）
├── route.h/c          # Stub HTTP 路由
├── proxy.h/c          # TCP 端口转发（半关闭支持）
├── mime.h/c           # MIME 类型查找
├── path_utils.h/c     # URL 解码（路径中 + 不解码为空格）、路径穿越防护
└── log.h/c            # 分级日志
```

## 许可证

MIT
