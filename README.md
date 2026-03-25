# TinyServe

English | [中文](README_CN.md)

A lightweight, single-binary HTTP file server / stub HTTP server / TCP port forwarder written in pure C11 with [libuv](https://libuv.org/).

Faster, lighter, and more versatile than `python -m http.server`.

## Features

- **File Server** (`-m f`): Directory browsing, file download, Range/206 (single & multipart), MIME types, keep-alive, path traversal protection
- **Stub HTTP Server** (`-m s`): Route-based mock server via simple config file, supports GET/POST/HEAD
- **TCP Port Forwarder** (`-m p`): Bidirectional byte-stream proxy with backpressure and TCP half-close support
- **Auth**: Optional Basic Auth and/or custom Header Auth (OR logic), with strict CLI validation
- **Fully async I/O**: Single-threaded libuv event loop, async file open/read/close, no blocking
- **HTTP/1.0 & HTTP/1.1**: Responds with the same HTTP version the client uses, with proper keep-alive and pipelining support
- **HEAD support**: All response paths (200, 206, 301, 401, 404, 405, 416) correctly suppress body for HEAD requests
- **Safe directory listing**: HTML-escaped display names, URL-encoded hrefs, XSS-safe
- **IPv4 & IPv6**: Dual-stack support, bind to `127.0.0.1`, `::1`, `0.0.0.0`, `::`, etc.

## Build

```bash
# Prerequisites: cmake >= 3.16, C11 compiler, libuv
# macOS: brew install libuv cmake
# Ubuntu/Debian: apt install libuv1-dev cmake build-essential

cmake -S . -B build
cmake --build build

# Binary is at ./build/tinyserve
```

## Usage

```
tinyserve [options]
```

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `-m` | Mode: `f` (file), `s` (stub HTTP), `p` (proxy) | `f` |
| `-a` | Bind address | `127.0.0.1` |
| `-p` | Listen port (1-65535, strictly validated) | `8000` |
| `-d` | Root directory (file mode) | `.` |
| `-c` | Route config file (stub mode) | — |
| `-u` | Basic Auth username (requires `-w`) | — |
| `-w` | Basic Auth password (requires `-u`) | — |
| `-k` | Header auth header name (requires `-v`) | — |
| `-v` | Header auth expected value (requires `-k`) | — |
| `-t` | Proxy target host | — |
| `-q` | Proxy target port (1-65535, strictly validated) | — |
| `-l` | Log level: `error`, `warn`, `info` | `info` |
| `-h` | Show help | — |

## Mode Examples

### File Server (`-m f`)

```bash
# Serve current directory on port 8000 (IPv4 localhost)
./build/tinyserve

# Serve on IPv6 localhost
./build/tinyserve -m f -d /var/www -a ::1 -p 3000

# Bind to all interfaces (IPv4 + IPv6)
./build/tinyserve -m f -d ./public -a :: -p 8080

# With Basic Auth
./build/tinyserve -m f -d ./public -u admin -w secret

# With both Basic Auth and Header Auth (either one passes)
./build/tinyserve -m f -d ./public -u admin -w secret -k X-Token -v mytoken123
```

### Stub HTTP Server (`-m s`)

```bash
./build/tinyserve -m s -c routes.conf -p 9000
```

Route config file format (`routes.conf`):

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

---
method = GET
path = /health
status = 200
content-type = text/plain
body = OK
```

### TCP Port Forwarder (`-m p`)

```bash
# Forward local port 8080 to remote-host:3000
./build/tinyserve -m p -p 8080 -t remote-host -q 3000

# Forward to a local service
./build/tinyserve -m p -p 9090 -t 127.0.0.1 -q 5432
```

## Auth

Auth is **off by default**. Enable by providing credentials:

- `-u admin -w password` → Basic Auth
- `-k X-API-Key -v secret` → Custom Header Auth
- Both together → either method passes (OR logic)

Auth pairs are validated at startup: `-u` without `-w` (or `-k` without `-v`) is a fatal error.

Auth applies to file mode and stub mode. Proxy mode ignores auth (pure TCP forwarding).

## curl Verification Examples

### HEAD Requests

HEAD requests return correct headers but no body for all status codes:

```bash
# HEAD on file
curl -I http://127.0.0.1:8000/file.txt
# HTTP/1.1 200 OK
# Content-Length: 1234
# (no body)

# HEAD on missing path
curl -I http://127.0.0.1:8000/missing
# HTTP/1.1 404 Not Found
# Content-Length: 25
# (no body)

# HEAD with Range
curl -I -H "Range: bytes=0-99" http://127.0.0.1:8000/file.txt
# HTTP/1.1 206 Partial Content
# Content-Range: bytes 0-99/1234
# (no body)

# HEAD with multipart Range
curl -I -H "Range: bytes=0-9,20-29" http://127.0.0.1:8000/file.txt
# HTTP/1.1 206 Partial Content
# Content-Type: multipart/byteranges; boundary=tinyserve_...
# (no body, no part headers leaked)
```

### 401 Unauthorized

```bash
# Start with auth
./build/tinyserve -m f -d /tmp -u admin -w secret &

# No credentials → 401
curl -I http://127.0.0.1:8000/
# HTTP/1.1 401 Unauthorized
# WWW-Authenticate: Basic realm="tinyserve"

# Correct credentials → 200
curl -u admin:secret http://127.0.0.1:8000/
```

### 404 Not Found

```bash
curl http://127.0.0.1:8000/does-not-exist
# Nothing Found in the PATH
```

### 206 Partial Content (Single Range)

```bash
# First 100 bytes
curl -H "Range: bytes=0-99" -i http://127.0.0.1:8000/largefile.bin
# HTTP/1.1 206 Partial Content
# Content-Range: bytes 0-99/102400
# Content-Length: 100

# Last 1024 bytes
curl -H "Range: bytes=-1024" http://127.0.0.1:8000/largefile.bin

# From byte 1024 to end
curl -H "Range: bytes=1024-" http://127.0.0.1:8000/largefile.bin
```

### 206 Multipart Ranges

```bash
curl -H "Range: bytes=0-99,200-299" -i http://127.0.0.1:8000/largefile.bin
# HTTP/1.1 206 Partial Content
# Content-Type: multipart/byteranges; boundary=tinyserve_...
#
# --tinyserve_...
# Content-Type: application/octet-stream
# Content-Range: bytes 0-99/102400
#
# [data]
# --tinyserve_...
# Content-Type: application/octet-stream
# Content-Range: bytes 200-299/102400
#
# [data]
# --tinyserve_...--
```

### 416 Range Not Satisfiable

```bash
curl -H "Range: bytes=999999-" -i http://127.0.0.1:8000/smallfile.txt
# HTTP/1.1 416 Range Not Satisfiable
# Content-Range: bytes */16
```

### Content-Length Validation

Invalid `Content-Length` values are rejected with 400 Bad Request:

```bash
curl -H "Content-Length: -1" http://127.0.0.1:8000/
# 400 Bad Request

curl -H "Content-Length: abc" http://127.0.0.1:8000/
# 400 Bad Request

curl -H "Content-Length: 99999999999999999999" http://127.0.0.1:8000/
# 400 Bad Request
```

### Proxy Mode

```bash
# Terminal 1: Start a backend
./build/tinyserve -m s -c routes.conf -p 9000

# Terminal 2: Start proxy (supports TCP half-close)
./build/tinyserve -m p -p 8080 -t 127.0.0.1 -q 9000

# Terminal 3: Access via proxy
curl http://127.0.0.1:8080/hello
# Hello, World!

# Half-close: client sends request, closes write side, reads response
(printf "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"; sleep 1) | nc 127.0.0.1 8080
# HTTP/1.1 200 OK
# ...
# Hello, World!
```

## Multi-connection Download Tools

TinyServe supports `axel`, `aria2`, and other multi-connection download tools:

```bash
# axel (multi-connection download)
axel http://127.0.0.1:8000/largefile.bin

# aria2 (multi-connection + resume)
aria2c -x 4 http://127.0.0.1:8000/largefile.bin

# curl resume (interrupt and resume)
curl -C - -O http://127.0.0.1:8000/largefile.bin
```

## Project Structure

```
src/
├── main.c             # Entry point, CLI
├── tinyserve.h        # Shared types and constants
├── server.h/c         # libuv event loop, connection management, write completion tracking
├── config.h/c         # CLI argument parsing with strict validation
├── http_parser.h/c    # HTTP/1.1 request parser with strict Content-Length validation
├── http_response.h/c  # HTTP response builder with HEAD support and write tracking
├── auth.h/c           # Basic Auth + Header Auth
├── file_serve.h/c     # File server mode with safe directory listing
├── range.h/c          # Range/206 request handling with overflow protection
├── route.h/c          # Stub HTTP server routing
├── proxy.h/c          # TCP port forwarding with half-close support
├── mime.h/c           # MIME type lookup
├── path_utils.h/c     # URL decoding (no +→space in paths), path traversal prevention
└── log.h/c            # Logging with levels
```

## License

MIT
