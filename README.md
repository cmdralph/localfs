# 🚀 LocalFS

A fast, lightweight **local development server written in C** for macOS.  
Designed for serving static websites with a clean CLI workflow and useful developer features.

---

## 📦 Build Instructions (macOS)

No external dependencies required.

### 1. Install Apple Command Line Tools
```bash
xcode-select --install
```

### 2. Compile
```bash
cc -Wall -Wextra -O2 main.c -o localfs -pthread
```

### 3. Run
```bash
./localfs --folder ./site --port 8080
```

---

## 🖥️ CLI Usage Examples

### Serve one folder
```bash
./localfs --folder ./site
```
➡️ Runs at: `http://127.0.0.1:8080`

### Custom port
```bash
./localfs --folder ./site --port 3000
```

### Multiple folders
```bash
./localfs \
  --folder ./site1 --port 8080 \
  --folder ./site2 --port 8081
```

### Custom host
```bash
./localfs --host 127.0.0.1 --folder ./site --port 8080
```

### Bind to all interfaces (LAN access)
```bash
./localfs --host 0.0.0.0 --folder ./site --port 8080
```

Access locally via:
```
http://127.0.0.1:8080
```

### Open browser automatically
```bash
./localfs --open --folder ./site
```

### Logging modes
```bash
./localfs --verbose --folder ./site   # detailed logs
./localfs --quiet --folder ./site     # minimal output
```

### Enable directory listing
```bash
./localfs --dir-list --folder ./site
```

### Enable CORS
```bash
./localfs --cors --folder ./site
```

### Enable caching headers
```bash
./localfs --cache --folder ./site
```

### Route alias
```bash
./localfs --folder ./site --route /assets=./shared-assets
```

Then:
```
http://127.0.0.1:8080/assets/logo.png
```

Serves:
```
./shared-assets/logo.png
```

### List config + port availability
```bash
./localfs --folder ./site --port 8080 --list
```

---

## ⚙️ Config File Support

Create a `server.conf`:

```ini
host=127.0.0.1

folder=./site
port=8080
route=/assets=./shared-assets

folder=./docs
port=8081

dir_list=true
cache=false
cors=true
verbose=true
```

### Run with config
```bash
./localfs
```

or:
```bash
./localfs --config server.conf
```

---

## ✨ Features Explained

### 📁 Static File Serving
Requests like:
```
/assets/style.css
```

Are:
1. URL decoded  
2. validated for safety  
3. resolved using `realpath()`  
4. streamed to the browser  

---

### 📄 Automatic `index.html`
When visiting:
```
/
```

The server serves:
```
index.html
```

---

### 🚫 Custom 404 Page
If present:
```
404.html
```

It will be returned automatically for missing files.

---

### 🔐 Safe Path Handling
Prevents:
```
../
```

Also blocks symlinks escaping the root directory.

Example blocked:
```
/../../etc/passwd
```

---

### ⚡ Concurrency Model

- 1 thread per server
- 1 detached thread per client

Allows multiple simultaneous requests without blocking.

---

### 📦 Large File Streaming

Files are streamed in chunks:

```c
#define FILE_STREAM_BUFFER_SIZE 65536
```

Prevents high memory usage.

---

### 🔀 Route Aliases

```bash
--route /assets=./shared-assets
```

Maps:
```
/assets → ./shared-assets
```

---

### ⚙️ Config System

Simple format:
```
key=value
```

Rules:
- `folder` defines a server block
- `port` + `route` apply to the last folder

---

## 🧠 Customization Guide

| Upgrade | Where to edit |
|--------|------|
| Add MIME types | `mime_type_for_path()` |
| Increase servers | `#define MAX_SERVERS 3` |
| Increase routes | `#define MAX_ROUTES 8` |
| Change default port | `#define DEFAULT_PORT 8080` |
| Modify cache headers | `send_headers()` |
| Add SPA fallback | `serve_request_path()` |
| Add JSON logs | `handle_client()` |
| Add IPv6 | use `getaddrinfo()` |

---

## 🔮 Suggested Next Feature

### SPA Mode (`--spa`)

Fallback unknown routes to `index.html`.

Useful for:
- React
- Vue
- Svelte

---

## 🎯 Summary

- ⚡ Fast C-based HTTP server  
- 🧩 Multiple folders / ports  
- 🔐 Safe file handling  
- 🧵 Concurrent requests  
- ⚙️ Config + CLI support  
- 🧪 Ideal for local web development
