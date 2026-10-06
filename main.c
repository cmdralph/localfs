/*
    localfs - CLI-first local HTTP static file server for macOS

    Features:
    - Serve up to 3 folders at the same time
    - POSIX sockets
    - pthreads
    - Static file serving
    - GET and HEAD support
    - Custom 404.html support
    - Optional directory listing
    - Optional cache headers
    - Optional CORS headers
    - Route aliases like /assets=./shared-assets
    - Config file support
    - Graceful shutdown on Ctrl+C
    - macOS-friendly --open support

    Build:
        cc -Wall -Wextra -O2 main.c -o localserver -pthread

    Example:
        ./localserver --folder ./site --port 8080
*/

#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_SERVERS 3
#define MAX_ROUTES 8

#define DEFAULT_PORT 8080
#define BACKLOG 64

#define REQUEST_BUFFER_SIZE 8192
#define SMALL_BUFFER_SIZE 512
#define URL_BUFFER_SIZE 2048
#define FILE_STREAM_BUFFER_SIZE 65536

typedef struct {
    char mount[256];
    char source_input[PATH_MAX];
    char source_real[PATH_MAX];
} RouteAlias;

typedef struct {
    char folder_input[PATH_MAX];
    char root_real[PATH_MAX];

    int port;
    bool port_set;

    RouteAlias routes[MAX_ROUTES];
    int route_count;
} SiteConfig;

typedef struct {
    char host[64];

    SiteConfig sites[MAX_SERVERS];
    int server_count;

    bool open_browser;
    bool verbose;
    bool quiet;
    bool dir_list;
    bool cache;
    bool cors;
    bool list_only;
    bool color;
} AppConfig;

typedef struct {
    int id;
    int listen_fd;

    volatile sig_atomic_t running;
    volatile sig_atomic_t stop_requested;

    pthread_t thread;

    AppConfig *app;
    SiteConfig *site;
} ServerRuntime;

typedef struct {
    int client_fd;
    ServerRuntime *server;
    struct sockaddr_in client_addr;
} ClientJob;

typedef struct {
    const char *root;
    char relative[URL_BUFFER_SIZE];
    char physical[PATH_MAX];
} ResolvedPath;

typedef struct {
    char name[PATH_MAX];
    bool is_dir;
} DirEntry;

static AppConfig g_config;
static ServerRuntime g_servers[MAX_SERVERS];
static volatile sig_atomic_t g_should_stop = 0;

/* ------------------------------------------------------------
   Terminal colors
------------------------------------------------------------ */

static const char *C_RESET = "\033[0m";
static const char *C_RED = "\033[31m";
static const char *C_GREEN = "\033[32m";
static const char *C_YELLOW = "\033[33m";
static const char *C_BLUE = "\033[34m";
static const char *C_DIM = "\033[2m";

static const char *colorize(const AppConfig *app, const char *color) {
    return app->color ? color : "";
}

static void log_normal(const AppConfig *app, const char *fmt, ...) {
    if (app->quiet) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}

static void log_verbose(const AppConfig *app, const char *fmt, ...) {
    if (app->quiet || !app->verbose) {
        return;
    }

    fprintf(stdout, "%s", colorize(app, C_DIM));

    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fprintf(stdout, "%s", colorize(app, C_RESET));
}

static void log_error(const AppConfig *app, const char *fmt, ...) {
    fprintf(stderr, "%s", colorize(app, C_RED));

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "%s", colorize(app, C_RESET));
}

/* ------------------------------------------------------------
   Small helpers
------------------------------------------------------------ */

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    char *end = s + strlen(s) - 1;

    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

static bool parse_bool(const char *value, bool *out) {
    if (!value) {
        return false;
    }

    if (
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0 ||
        strcmp(value, "1") == 0
    ) {
        *out = true;
        return true;
    }

    if (
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0 ||
        strcmp(value, "0") == 0
    ) {
        *out = false;
        return true;
    }

    return false;
}

static bool parse_port(const char *text, int *out_port) {
    if (!text || !*text) {
        return false;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (*end != '\0') {
        return false;
    }

    if (value < 1 || value > 65535) {
        return false;
    }

    *out_port = (int)value;
    return true;
}

static void normalize_mount(char *mount) {
    if (!mount || !*mount) {
        strcpy(mount, "/");
        return;
    }

    if (mount[0] != '/') {
        char temp[256];
        snprintf(temp, sizeof(temp), "/%s", mount);
        snprintf(mount, 256, "%s", temp);
    }

    size_t len = strlen(mount);

    while (len > 1 && mount[len - 1] == '/') {
        mount[len - 1] = '\0';
        len--;
    }
}

static void remove_query_and_fragment(char *path) {
    char *q = strchr(path, '?');

    if (q) {
        *q = '\0';
    }

    char *hash = strchr(path, '#');

    if (hash) {
        *hash = '\0';
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

static bool url_decode(const char *src, char *dest, size_t dest_size) {
    size_t di = 0;

    for (size_t si = 0; src[si] != '\0'; si++) {
        if (di + 1 >= dest_size) {
            return false;
        }

        if (src[si] == '%') {
            int hi = hex_value(src[si + 1]);
            int lo = hex_value(src[si + 2]);

            if (hi < 0 || lo < 0) {
                return false;
            }

            dest[di++] = (char)((hi << 4) | lo);
            si += 2;
        } else {
            dest[di++] = src[si];
        }
    }

    dest[di] = '\0';
    return true;
}

static bool contains_parent_reference(const char *path) {
    char copy[URL_BUFFER_SIZE];
    snprintf(copy, sizeof(copy), "%s", path);

    char *saveptr = NULL;
    char *part = strtok_r(copy, "/", &saveptr);

    while (part) {
        if (strcmp(part, "..") == 0) {
            return true;
        }

        part = strtok_r(NULL, "/", &saveptr);
    }

    return false;
}

static bool path_is_inside_root(const char *root, const char *path) {
    if (strcmp(root, "/") == 0) {
        return true;
    }

    size_t root_len = strlen(root);

    if (strncmp(root, path, root_len) != 0) {
        return false;
    }

    return path[root_len] == '\0' || path[root_len] == '/';
}

static bool join_path(const char *root, const char *relative, char *out, size_t out_size) {
    int written;

    if (!relative || relative[0] == '\0') {
        written = snprintf(out, out_size, "%s", root);
    } else if (strcmp(root, "/") == 0) {
        written = snprintf(out, out_size, "/%s", relative);
    } else {
        written = snprintf(out, out_size, "%s/%s", root, relative);
    }

    return written > 0 && (size_t)written < out_size;
}

static bool host_to_addr(const char *host, struct in_addr *out_addr) {
    if (strcmp(host, "localhost") == 0) {
        return inet_pton(AF_INET, "127.0.0.1", out_addr) == 1;
    }

    return inet_pton(AF_INET, host, out_addr) == 1;
}

/* ------------------------------------------------------------
   MIME types
------------------------------------------------------------ */

static const char *mime_type_for_path(const char *path) {
    const char *ext = strrchr(path, '.');

    if (!ext) {
        return "application/octet-stream";
    }

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }

    if (strcasecmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }

    if (strcasecmp(ext, ".js") == 0 || strcasecmp(ext, ".mjs") == 0) {
        return "application/javascript; charset=utf-8";
    }

    if (strcasecmp(ext, ".json") == 0) {
        return "application/json; charset=utf-8";
    }

    if (strcasecmp(ext, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }

    if (strcasecmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }

    if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    }

    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }

    if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    }

    if (strcasecmp(ext, ".webp") == 0) {
        return "image/webp";
    }

    if (strcasecmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }

    if (strcasecmp(ext, ".wasm") == 0) {
        return "application/wasm";
    }

    if (strcasecmp(ext, ".pdf") == 0) {
        return "application/pdf";
    }

    if (strcasecmp(ext, ".woff") == 0) {
        return "font/woff";
    }

    if (strcasecmp(ext, ".woff2") == 0) {
        return "font/woff2";
    }

    if (strcasecmp(ext, ".ttf") == 0) {
        return "font/ttf";
    }

    if (strcasecmp(ext, ".otf") == 0) {
        return "font/otf";
    }

    if (strcasecmp(ext, ".mp4") == 0) {
        return "video/mp4";
    }

    if (strcasecmp(ext, ".mp3") == 0) {
        return "audio/mpeg";
    }

    return "application/octet-stream";
}

/* ------------------------------------------------------------
   HTTP response helpers
------------------------------------------------------------ */

static bool send_all(int fd, const void *buffer, size_t length) {
    const char *data = (const char *)buffer;
    size_t sent_total = 0;

    while (sent_total < length) {
        ssize_t sent = send(fd, data + sent_total, length - sent_total, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (sent == 0) {
            return false;
        }

        sent_total += (size_t)sent;
    }

    return true;
}

static void http_date_now(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm utc_time;

    gmtime_r(&now, &utc_time);
    strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &utc_time);
}

static void send_headers(
    int client_fd,
    const AppConfig *app,
    int status,
    const char *reason,
    const char *content_type,
    long long content_length,
    const char *extra_headers
) {
    char date[128];
    char headers[2048];

    http_date_now(date, sizeof(date));

    int written = snprintf(
        headers,
        sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Server: localserver-c\r\n"
        "Date: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n"
        "%s"
        "%s"
        "%s"
        "\r\n",
        status,
        reason,
        date,
        content_type ? content_type : "application/octet-stream",
        content_length,
        app->cache
            ? "Cache-Control: public, max-age=3600\r\n"
            : "Cache-Control: no-store\r\n",
        app->cors
            ? "Access-Control-Allow-Origin: *\r\n"
            : "",
        extra_headers ? extra_headers : ""
    );

    if (written > 0 && (size_t)written < sizeof(headers)) {
        send_all(client_fd, headers, (size_t)written);
    }
}

static void send_text_response(
    int client_fd,
    const AppConfig *app,
    int status,
    const char *reason,
    const char *body,
    bool head_only
) {
    long long length = (long long)strlen(body);

    send_headers(
        client_fd,
        app,
        status,
        reason,
        "text/html; charset=utf-8",
        length,
        NULL
    );

    if (!head_only) {
        send_all(client_fd, body, (size_t)length);
    }
}

/* ------------------------------------------------------------
   Directory listing helpers
------------------------------------------------------------ */

static int compare_dir_entries(const void *a, const void *b) {
    const DirEntry *ea = (const DirEntry *)a;
    const DirEntry *eb = (const DirEntry *)b;

    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }

    return strcasecmp(ea->name, eb->name);
}

static void html_escape(const char *src, char *dest, size_t dest_size) {
    size_t di = 0;

    for (size_t si = 0; src[si] != '\0' && di + 1 < dest_size; si++) {
        const char *replacement = NULL;

        switch (src[si]) {
            case '&':
                replacement = "&amp;";
                break;
            case '<':
                replacement = "&lt;";
                break;
            case '>':
                replacement = "&gt;";
                break;
            case '"':
                replacement = "&quot;";
                break;
            case '\'':
                replacement = "&#39;";
                break;
            default:
                break;
        }

        if (replacement) {
            size_t len = strlen(replacement);

            if (di + len >= dest_size) {
                break;
            }

            memcpy(dest + di, replacement, len);
            di += len;
        } else {
            dest[di++] = src[si];
        }
    }

    dest[di] = '\0';
}

static void url_encode_component(const char *src, char *dest, size_t dest_size) {
    static const char *hex = "0123456789ABCDEF";
    size_t di = 0;

    for (size_t si = 0; src[si] != '\0' && di + 1 < dest_size; si++) {
        unsigned char c = (unsigned char)src[si];

        if (
            isalnum(c) ||
            c == '-' ||
            c == '_' ||
            c == '.' ||
            c == '~' ||
            c == '/'
        ) {
            dest[di++] = (char)c;
        } else {
            if (di + 3 >= dest_size) {
                break;
            }

            dest[di++] = '%';
            dest[di++] = hex[c >> 4];
            dest[di++] = hex[c & 15];
        }
    }

    dest[di] = '\0';
}

static void send_directory_listing(
    int client_fd,
    const AppConfig *app,
    const ResolvedPath *resolved,
    const char *url_path,
    bool head_only
) {
    DIR *dir = opendir(resolved->physical);

    if (!dir) {
        send_text_response(
            client_fd,
            app,
            403,
            "Forbidden",
            "<h1>403 Forbidden</h1>",
            head_only
        );
        return;
    }

    size_t capacity = 64;
    size_t count = 0;

    DirEntry *entries = calloc(capacity, sizeof(DirEntry));

    if (!entries) {
        closedir(dir);
        send_text_response(
            client_fd,
            app,
            500,
            "Internal Server Error",
            "<h1>500 Internal Server Error</h1>",
            head_only
        );
        return;
    }

    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        if (count == capacity) {
            capacity *= 2;

            DirEntry *new_entries = realloc(entries, capacity * sizeof(DirEntry));

            if (!new_entries) {
                break;
            }

            entries = new_entries;
        }

        snprintf(entries[count].name, sizeof(entries[count].name), "%s", de->d_name);

        char child_path[PATH_MAX];
        struct stat st;

        if (
            join_path(resolved->physical, de->d_name, child_path, sizeof(child_path)) &&
            stat(child_path, &st) == 0
        ) {
            entries[count].is_dir = S_ISDIR(st.st_mode);
        }

        count++;
    }

    closedir(dir);

    qsort(entries, count, sizeof(DirEntry), compare_dir_entries);

    size_t body_capacity = 1024 * 1024;
    char *body = malloc(body_capacity);

    if (!body) {
        free(entries);
        send_text_response(
            client_fd,
            app,
            500,
            "Internal Server Error",
            "<h1>500 Internal Server Error</h1>",
            head_only
        );
        return;
    }

    char escaped_url[URL_BUFFER_SIZE * 2];
    html_escape(url_path, escaped_url, sizeof(escaped_url));

    size_t used = 0;

    used += (size_t)snprintf(
        body + used,
        body_capacity - used,
        "<!doctype html>"
        "<html>"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<title>Index of %s</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:2rem;background:#0f172a;color:#e5e7eb;}"
        "a{display:block;padding:.35rem 0;color:#93c5fd;text-decoration:none;}"
        "a:hover{text-decoration:underline;}"
        ".muted{color:#94a3b8;}"
        ".box{max-width:900px;margin:auto;background:#111827;border:1px solid #334155;border-radius:16px;padding:24px;}"
        "</style>"
        "</head>"
        "<body>"
        "<main class=\"box\">"
        "<h1>Index of %s</h1>"
        "<p class=\"muted\">Directory listing is enabled.</p>",
        escaped_url,
        escaped_url
    );

    if (strcmp(url_path, "/") != 0 && used + 64 < body_capacity) {
        used += (size_t)snprintf(
            body + used,
            body_capacity - used,
            "<a href=\"../\">../</a>"
        );
    }

    for (size_t i = 0; i < count && used + 2048 < body_capacity; i++) {
        char display_name[PATH_MAX * 2];
        char encoded_name[PATH_MAX * 3];
        char name_with_suffix[PATH_MAX + 2];

        snprintf(
            name_with_suffix,
            sizeof(name_with_suffix),
            "%s%s",
            entries[i].name,
            entries[i].is_dir ? "/" : ""
        );

        html_escape(name_with_suffix, display_name, sizeof(display_name));
        url_encode_component(name_with_suffix, encoded_name, sizeof(encoded_name));

        used += (size_t)snprintf(
            body + used,
            body_capacity - used,
            "<a href=\"%s\">%s</a>",
            encoded_name,
            display_name
        );
    }

    if (used + 32 < body_capacity) {
        used += (size_t)snprintf(
            body + used,
            body_capacity - used,
            "</main></body></html>"
        );
    }

    send_headers(
        client_fd,
        app,
        200,
        "OK",
        "text/html; charset=utf-8",
        (long long)used,
        NULL
    );

    if (!head_only) {
        send_all(client_fd, body, used);
    }

    free(body);
    free(entries);
}

/* ------------------------------------------------------------
   Config parsing
------------------------------------------------------------ */

static void init_config(AppConfig *app) {
    memset(app, 0, sizeof(*app));

    snprintf(app->host, sizeof(app->host), "127.0.0.1");

    app->color = isatty(STDOUT_FILENO);

    for (int i = 0; i < MAX_SERVERS; i++) {
        app->sites[i].port = DEFAULT_PORT + i;
    }
}

static SiteConfig *add_site(
    AppConfig *app,
    const char *folder,
    char *err,
    size_t err_size
) {
    if (app->server_count >= MAX_SERVERS) {
        snprintf(err, err_size, "Only up to %d folders are supported.", MAX_SERVERS);
        return NULL;
    }

    SiteConfig *site = &app->sites[app->server_count];

    memset(site, 0, sizeof(*site));

    site->port = DEFAULT_PORT + app->server_count;

    snprintf(site->folder_input, sizeof(site->folder_input), "%s", folder);

    app->server_count++;

    return site;
}

static bool add_route_to_site(
    SiteConfig *site,
    const char *route_spec,
    char *err,
    size_t err_size
) {
    if (!site) {
        snprintf(err, err_size, "--route must come after --folder.");
        return false;
    }

    if (site->route_count >= MAX_ROUTES) {
        snprintf(
            err,
            err_size,
            "Only up to %d routes are supported per folder.",
            MAX_ROUTES
        );
        return false;
    }

    const char *equals = strchr(route_spec, '=');

    if (!equals || equals == route_spec || equals[1] == '\0') {
        snprintf(err, err_size, "Invalid route '%s'. Use /mount=./folder.", route_spec);
        return false;
    }

    size_t mount_len = (size_t)(equals - route_spec);

    if (mount_len >= sizeof(site->routes[site->route_count].mount)) {
        snprintf(err, err_size, "Route mount path is too long.");
        return false;
    }

    RouteAlias *route = &site->routes[site->route_count];

    memset(route, 0, sizeof(*route));

    memcpy(route->mount, route_spec, mount_len);
    route->mount[mount_len] = '\0';

    normalize_mount(route->mount);

    snprintf(route->source_input, sizeof(route->source_input), "%s", equals + 1);

    site->route_count++;

    return true;
}

static bool load_config_file(
    AppConfig *app,
    const char *path,
    bool required,
    char *err,
    size_t err_size
) {
    FILE *file = fopen(path, "r");

    if (!file) {
        if (required) {
            snprintf(err, err_size, "Could not open config file '%s'.", path);
            return false;
        }

        return true;
    }

    char line[PATH_MAX + 256];
    int line_no = 0;
    SiteConfig *current_site = NULL;

    while (fgets(line, sizeof(line), file)) {
        line_no++;

        char *s = trim(line);

        if (*s == '\0' || *s == '#') {
            continue;
        }

        char *equals = strchr(s, '=');

        if (!equals) {
            snprintf(err, err_size, "%s:%d: expected key=value.", path, line_no);
            fclose(file);
            return false;
        }

        *equals = '\0';

        char *key = trim(s);
        char *value = trim(equals + 1);

        if (strcmp(key, "host") == 0) {
            snprintf(app->host, sizeof(app->host), "%s", value);
        } else if (strcmp(key, "folder") == 0) {
            current_site = add_site(app, value, err, err_size);

            if (!current_site) {
                fclose(file);
                return false;
            }
        } else if (strcmp(key, "port") == 0) {
            if (!current_site) {
                snprintf(err, err_size, "%s:%d: port must come after folder.", path, line_no);
                fclose(file);
                return false;
            }

            if (!parse_port(value, &current_site->port)) {
                snprintf(err, err_size, "%s:%d: invalid port '%s'.", path, line_no, value);
                fclose(file);
                return false;
            }

            current_site->port_set = true;
        } else if (strcmp(key, "route") == 0) {
            if (!add_route_to_site(current_site, value, err, err_size)) {
                char detail[SMALL_BUFFER_SIZE];
                snprintf(detail, sizeof(detail), "%s", err);
                snprintf(err, err_size, "%s:%d: %s", path, line_no, detail);
                fclose(file);
                return false;
            }
        } else if (strcmp(key, "open") == 0) {
            if (!parse_bool(value, &app->open_browser)) {
                goto bad_bool;
            }
        } else if (strcmp(key, "verbose") == 0) {
            if (!parse_bool(value, &app->verbose)) {
                goto bad_bool;
            }
        } else if (strcmp(key, "quiet") == 0) {
            if (!parse_bool(value, &app->quiet)) {
                goto bad_bool;
            }
        } else if (strcmp(key, "dir_list") == 0 || strcmp(key, "dir-list") == 0) {
            if (!parse_bool(value, &app->dir_list)) {
                goto bad_bool;
            }
        } else if (strcmp(key, "cache") == 0) {
            if (!parse_bool(value, &app->cache)) {
                goto bad_bool;
            }
        } else if (strcmp(key, "cors") == 0) {
            if (!parse_bool(value, &app->cors)) {
                goto bad_bool;
            }
        } else {
            snprintf(err, err_size, "%s:%d: unknown key '%s'.", path, line_no, key);
            fclose(file);
            return false;
        }

        continue;

bad_bool:
        snprintf(
            err,
            err_size,
            "%s:%d: '%s' must be true or false.",
            path,
            line_no,
            key
        );
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

static void print_help(const char *program_name) {
    printf(
        "localserver - CLI-first local static file server\n\n"
        "Usage:\n"
        "  %s --folder ./site\n"
        "  %s --folder ./site --port 8080\n"
        "  %s --folder ./site1 --port 8080 --folder ./site2 --port 8081\n"
        "  %s --host 127.0.0.1 --folder ./site --port 8080\n"
        "  %s --open --folder ./site\n"
        "  %s --list\n\n"
        "Options:\n"
        "  --folder PATH          Add a folder to serve. Up to 3 folders.\n"
        "  --port PORT            Port for the most recent --folder.\n"
        "                         Defaults: 8080, 8081, 8082.\n"
        "  --host HOST            IPv4 host/interface to bind.\n"
        "                         Default: 127.0.0.1.\n"
        "                         Also supports localhost and 0.0.0.0.\n"
        "  --route /URL=PATH      Add alias route to the most recent folder.\n"
        "                         Example: --route /assets=./shared-assets\n"
        "  --open                 Open each server URL in the default browser.\n"
        "  --dir-list             Enable directory listings.\n"
        "  --cache                Send Cache-Control: public, max-age=3600.\n"
        "  --cors                 Send Access-Control-Allow-Origin: *.\n"
        "  --verbose              Print detailed request logs.\n"
        "  --quiet                Only print errors.\n"
        "  --config FILE          Load key=value config file.\n"
        "  --list                 Show parsed config and port availability, then exit.\n"
        "  --no-color             Disable ANSI terminal colors.\n"
        "  --help                 Show this help message.\n\n"
        "server.conf example:\n"
        "  host=127.0.0.1\n"
        "  folder=./site\n"
        "  port=8080\n"
        "  route=/assets=./shared-assets\n"
        "  dir_list=true\n"
        "  cache=false\n"
        "  cors=true\n",
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name
    );
}

static bool parse_args(
    AppConfig *app,
    int argc,
    char **argv,
    char *err,
    size_t err_size
) {
    SiteConfig *current_site = NULL;
    bool explicit_config_loaded = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                snprintf(err, err_size, "--config requires a file path.");
                return false;
            }

            if (!load_config_file(app, argv[++i], true, err, err_size)) {
                return false;
            }

            explicit_config_loaded = true;

            if (app->server_count > 0) {
                current_site = &app->sites[app->server_count - 1];
            }
        } else if (strcmp(argv[i], "--host") == 0) {
            if (i + 1 >= argc) {
                snprintf(err, err_size, "--host requires a value.");
                return false;
            }

            snprintf(app->host, sizeof(app->host), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--folder") == 0) {
            if (i + 1 >= argc) {
                snprintf(err, err_size, "--folder requires a path.");
                return false;
            }

            current_site = add_site(app, argv[++i], err, err_size);

            if (!current_site) {
                return false;
            }
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                snprintf(err, err_size, "--port requires a number.");
                return false;
            }

            if (!current_site) {
                snprintf(err, err_size, "--port must come after --folder.");
                return false;
            }

            if (!parse_port(argv[++i], &current_site->port)) {
                snprintf(err, err_size, "Invalid port '%s'.", argv[i]);
                return false;
            }

            current_site->port_set = true;
        } else if (strcmp(argv[i], "--route") == 0) {
            if (i + 1 >= argc) {
                snprintf(err, err_size, "--route requires /mount=path.");
                return false;
            }

            if (!add_route_to_site(current_site, argv[++i], err, err_size)) {
                return false;
            }
        } else if (strcmp(argv[i], "--open") == 0) {
            app->open_browser = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            app->verbose = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            app->quiet = true;
        } else if (strcmp(argv[i], "--dir-list") == 0) {
            app->dir_list = true;
        } else if (strcmp(argv[i], "--cache") == 0) {
            app->cache = true;
        } else if (strcmp(argv[i], "--cors") == 0) {
            app->cors = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            app->list_only = true;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            app->color = false;
        } else {
            snprintf(err, err_size, "Unknown argument '%s'. Try --help.", argv[i]);
            return false;
        }
    }

    /*
        If the user did not pass --folder and did not pass --config,
        automatically load ./server.conf if it exists.
    */
    if (
        !explicit_config_loaded &&
        app->server_count == 0 &&
        access("server.conf", R_OK) == 0
    ) {
        if (!load_config_file(app, "server.conf", false, err, err_size)) {
            return false;
        }
    }

    return true;
}

/* ------------------------------------------------------------
   Validation
------------------------------------------------------------ */

static bool check_port_available(const AppConfig *app, int port) {
    struct in_addr host_addr;

    if (!host_to_addr(app->host, &host_addr)) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return false;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr = host_addr;
    addr.sin_port = htons((uint16_t)port);

    bool ok = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;

    close(fd);

    return ok;
}

static bool validate_and_resolve_config(
    AppConfig *app,
    char *err,
    size_t err_size
) {
    struct in_addr ignored;

    if (!host_to_addr(app->host, &ignored)) {
        snprintf(
            err,
            err_size,
            "Invalid host '%s'. Use 127.0.0.1, localhost, or 0.0.0.0.",
            app->host
        );
        return false;
    }

    if (app->server_count == 0) {
        snprintf(err, err_size, "No folders configured. Use --folder ./site.");
        return false;
    }

    for (int i = 0; i < app->server_count; i++) {
        SiteConfig *site = &app->sites[i];

        if (!realpath(site->folder_input, site->root_real)) {
            snprintf(
                err,
                err_size,
                "Folder '%s' does not exist or cannot be resolved.",
                site->folder_input
            );
            return false;
        }

        struct stat st;

        if (stat(site->root_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
            snprintf(err, err_size, "'%s' is not a folder.", site->folder_input);
            return false;
        }

        for (int j = i + 1; j < app->server_count; j++) {
            if (site->port == app->sites[j].port) {
                snprintf(
                    err,
                    err_size,
                    "Duplicate port %d. Each server needs a different port.",
                    site->port
                );
                return false;
            }
        }

        for (int r = 0; r < site->route_count; r++) {
            RouteAlias *route = &site->routes[r];

            if (!realpath(route->source_input, route->source_real)) {
                snprintf(
                    err,
                    err_size,
                    "Route source '%s' does not exist.",
                    route->source_input
                );
                return false;
            }

            if (stat(route->source_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
                snprintf(
                    err,
                    err_size,
                    "Route source '%s' is not a folder.",
                    route->source_input
                );
                return false;
            }
        }
    }

    return true;
}

/* ------------------------------------------------------------
   Request path mapping
------------------------------------------------------------ */

static bool route_matches(const char *request_path, const char *mount) {
    size_t len = strlen(mount);

    if (strcmp(mount, "/") == 0) {
        return true;
    }

    if (strncmp(request_path, mount, len) != 0) {
        return false;
    }

    return request_path[len] == '\0' || request_path[len] == '/';
}

static bool map_request_to_path(
    const SiteConfig *site,
    const char *target,
    ResolvedPath *resolved
) {
    char target_copy[URL_BUFFER_SIZE];
    char decoded[URL_BUFFER_SIZE];

    snprintf(target_copy, sizeof(target_copy), "%s", target);
    remove_query_and_fragment(target_copy);

    if (!url_decode(target_copy, decoded, sizeof(decoded))) {
        return false;
    }

    if (decoded[0] != '/') {
        char temp[URL_BUFFER_SIZE];
        snprintf(temp, sizeof(temp), "/%s", decoded);
        snprintf(decoded, sizeof(decoded), "%s", temp);
    }

    resolved->root = site->root_real;
    resolved->relative[0] = '\0';
    resolved->physical[0] = '\0';

    const RouteAlias *best_route = NULL;
    size_t best_len = 0;

    for (int i = 0; i < site->route_count; i++) {
        const RouteAlias *route = &site->routes[i];
        size_t len = strlen(route->mount);

        if (route_matches(decoded, route->mount) && len > best_len) {
            best_route = route;
            best_len = len;
        }
    }

    const char *relative_start = decoded;

    if (best_route) {
        resolved->root = best_route->source_real;

        if (strcmp(best_route->mount, "/") == 0) {
            relative_start = decoded;
        } else {
            relative_start = decoded + strlen(best_route->mount);
        }
    }

    while (*relative_start == '/') {
        relative_start++;
    }

    snprintf(resolved->relative, sizeof(resolved->relative), "%s", relative_start);

    if (contains_parent_reference(resolved->relative)) {
        return false;
    }

    if (!join_path(resolved->root, resolved->relative, resolved->physical, sizeof(resolved->physical))) {
        return false;
    }

    return true;
}

/* ------------------------------------------------------------
   File serving
------------------------------------------------------------ */

static void send_file_response(
    int client_fd,
    const AppConfig *app,
    const char *file_path,
    bool head_only
) {
    FILE *file = fopen(file_path, "rb");

    if (!file) {
        send_text_response(
            client_fd,
            app,
            404,
            "Not Found",
            "<h1>404 Not Found</h1>",
            head_only
        );
        return;
    }

    struct stat st;

    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(file);

        send_text_response(
            client_fd,
            app,
            404,
            "Not Found",
            "<h1>404 Not Found</h1>",
            head_only
        );
        return;
    }

    send_headers(
        client_fd,
        app,
        200,
        "OK",
        mime_type_for_path(file_path),
        (long long)st.st_size,
        NULL
    );

    if (!head_only) {
        char *buffer = malloc(FILE_STREAM_BUFFER_SIZE);

        if (!buffer) {
            fclose(file);
            return;
        }

        while (!feof(file)) {
            size_t bytes_read = fread(buffer, 1, FILE_STREAM_BUFFER_SIZE, file);

            if (bytes_read > 0) {
                if (!send_all(client_fd, buffer, bytes_read)) {
                    break;
                }
            }

            if (ferror(file)) {
                break;
            }
        }

        free(buffer);
    }

    fclose(file);
}

static bool try_send_custom_404(
    int client_fd,
    const AppConfig *app,
    const SiteConfig *site,
    bool head_only
) {
    char custom_404_path[PATH_MAX];
    char custom_404_real[PATH_MAX];

    if (!join_path(site->root_real, "404.html", custom_404_path, sizeof(custom_404_path))) {
        return false;
    }

    if (!realpath(custom_404_path, custom_404_real)) {
        return false;
    }

    if (!path_is_inside_root(site->root_real, custom_404_real)) {
        return false;
    }

    struct stat st;

    if (stat(custom_404_real, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    FILE *file = fopen(custom_404_real, "rb");

    if (!file) {
        return false;
    }

    send_headers(
        client_fd,
        app,
        404,
        "Not Found",
        "text/html; charset=utf-8",
        (long long)st.st_size,
        NULL
    );

    if (!head_only) {
        char buffer[8192];

        while (!feof(file)) {
            size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);

            if (bytes_read > 0) {
                if (!send_all(client_fd, buffer, bytes_read)) {
                    break;
                }
            }

            if (ferror(file)) {
                break;
            }
        }
    }

    fclose(file);
    return true;
}

static void send_404(
    int client_fd,
    const AppConfig *app,
    const SiteConfig *site,
    bool head_only
) {
    if (try_send_custom_404(client_fd, app, site, head_only)) {
        return;
    }

    send_text_response(
        client_fd,
        app,
        404,
        "Not Found",
        "<!doctype html>"
        "<html>"
        "<head><meta charset=\"utf-8\"><title>404 Not Found</title></head>"
        "<body><h1>404 Not Found</h1><p>The requested file was not found.</p></body>"
        "</html>",
        head_only
    );
}

static void serve_request_path(
    int client_fd,
    const AppConfig *app,
    const SiteConfig *site,
    const char *target,
    bool head_only
) {
    ResolvedPath resolved;

    if (!map_request_to_path(site, target, &resolved)) {
        send_text_response(
            client_fd,
            app,
            400,
            "Bad Request",
            "<h1>400 Bad Request</h1><p>Invalid request path.</p>",
            head_only
        );
        return;
    }

    char real_target[PATH_MAX];
    struct stat st;

    if (!realpath(resolved.physical, real_target)) {
        send_404(client_fd, app, site, head_only);
        return;
    }

    if (!path_is_inside_root(resolved.root, real_target)) {
        send_text_response(
            client_fd,
            app,
            403,
            "Forbidden",
            "<h1>403 Forbidden</h1><p>Path is outside the server root.</p>",
            head_only
        );
        return;
    }

    if (stat(real_target, &st) != 0) {
        send_404(client_fd, app, site, head_only);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        char index_path[PATH_MAX];
        char index_real[PATH_MAX];

        if (join_path(real_target, "index.html", index_path, sizeof(index_path))) {
            if (
                realpath(index_path, index_real) &&
                path_is_inside_root(resolved.root, index_real)
            ) {
                struct stat index_st;

                if (stat(index_real, &index_st) == 0 && S_ISREG(index_st.st_mode)) {
                    send_file_response(client_fd, app, index_real, head_only);
                    return;
                }
            }
        }

        if (app->dir_list) {
            char clean_url[URL_BUFFER_SIZE];
            snprintf(clean_url, sizeof(clean_url), "%s", target);
            remove_query_and_fragment(clean_url);

            send_directory_listing(client_fd, app, &resolved, clean_url, head_only);
            return;
        }

        send_404(client_fd, app, site, head_only);
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        send_404(client_fd, app, site, head_only);
        return;
    }

    send_file_response(client_fd, app, real_target, head_only);
}

/* ------------------------------------------------------------
   Client request handling
------------------------------------------------------------ */

static void client_ip_string(
    const struct sockaddr_in *addr,
    char *out,
    size_t out_size
) {
    const char *result = inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)out_size);

    if (!result) {
        snprintf(out, out_size, "unknown");
    }
}

static void handle_client(ClientJob *job) {
    ServerRuntime *server = job->server;
    AppConfig *app = server->app;
    SiteConfig *site = server->site;

    char request[REQUEST_BUFFER_SIZE];

    ssize_t received = recv(job->client_fd, request, sizeof(request) - 1, 0);

    if (received <= 0) {
        return;
    }

    request[received] = '\0';

    char method[16];
    char target[URL_BUFFER_SIZE];
    char version[32];

    method[0] = '\0';
    target[0] = '\0';
    version[0] = '\0';

    if (sscanf(request, "%15s %2047s %31s", method, target, version) != 3) {
        send_text_response(
            job->client_fd,
            app,
            400,
            "Bad Request",
            "<h1>400 Bad Request</h1>",
            false
        );
        return;
    }

    char ip[64];
    client_ip_string(&job->client_addr, ip, sizeof(ip));

    if (app->verbose) {
        log_verbose(
            app,
            "[server %d] %s %s %s from %s\n",
            server->id + 1,
            method,
            target,
            version,
            ip
        );
    } else if (!app->quiet) {
        log_normal(
            app,
            "[server %d] %s %s\n",
            server->id + 1,
            method,
            target
        );
    }

    bool head_only = strcmp(method, "HEAD") == 0;

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        const char *body =
            "<h1>405 Method Not Allowed</h1>"
            "<p>Only GET and HEAD are supported.</p>";

        send_headers(
            job->client_fd,
            app,
            405,
            "Method Not Allowed",
            "text/html; charset=utf-8",
            (long long)strlen(body),
            "Allow: GET, HEAD\r\n"
        );

        send_all(job->client_fd, body, strlen(body));
        return;
    }

    serve_request_path(job->client_fd, app, site, target, head_only);
}

static void *client_thread_main(void *arg) {
    ClientJob *job = (ClientJob *)arg;

    handle_client(job);

    close(job->client_fd);
    free(job);

    return NULL;
}

/* ------------------------------------------------------------
   Server runtime
------------------------------------------------------------ */

static void *server_thread_main(void *arg) {
    ServerRuntime *server = (ServerRuntime *)arg;

    while (!server->stop_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server->listen_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (
                server->stop_requested ||
                errno == EBADF ||
                errno == EINVAL
            ) {
                break;
            }

            continue;
        }

        ClientJob *job = malloc(sizeof(ClientJob));

        if (!job) {
            close(client_fd);
            continue;
        }

        job->client_fd = client_fd;
        job->server = server;
        job->client_addr = client_addr;

        pthread_t client_thread;

        if (pthread_create(&client_thread, NULL, client_thread_main, job) == 0) {
            pthread_detach(client_thread);
        } else {
            handle_client(job);
            close(job->client_fd);
            free(job);
        }
    }

    return NULL;
}

static bool start_server(
    ServerRuntime *server,
    AppConfig *app,
    int index,
    char *err,
    size_t err_size
) {
    SiteConfig *site = &app->sites[index];

    struct in_addr host_addr;

    if (!host_to_addr(app->host, &host_addr)) {
        snprintf(err, err_size, "Invalid host '%s'.", app->host);
        return false;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0) {
        snprintf(err, err_size, "socket() failed: %s", strerror(errno));
        return false;
    }

    int yes = 1;

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        snprintf(err, err_size, "setsockopt() failed: %s", strerror(errno));
        close(listen_fd);
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr = host_addr;
    addr.sin_port = htons((uint16_t)site->port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(
            err,
            err_size,
            "Port %d is not available on %s: %s",
            site->port,
            app->host,
            strerror(errno)
        );
        close(listen_fd);
        return false;
    }

    if (listen(listen_fd, BACKLOG) != 0) {
        snprintf(
            err,
            err_size,
            "listen() failed on port %d: %s",
            site->port,
            strerror(errno)
        );
        close(listen_fd);
        return false;
    }

    memset(server, 0, sizeof(*server));

    server->id = index;
    server->listen_fd = listen_fd;
    server->running = 1;
    server->stop_requested = 0;
    server->app = app;
    server->site = site;

    if (pthread_create(&server->thread, NULL, server_thread_main, server) != 0) {
        snprintf(
            err,
            err_size,
            "Could not create server thread for port %d.",
            site->port
        );
        close(listen_fd);
        server->listen_fd = -1;
        server->running = 0;
        return false;
    }

    return true;
}

static void stop_server(ServerRuntime *server) {
    if (!server->running) {
        return;
    }

    server->stop_requested = 1;

    if (server->listen_fd >= 0) {
        shutdown(server->listen_fd, SHUT_RDWR);
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    pthread_join(server->thread, NULL);

    server->running = 0;
}

static void stop_all_servers(AppConfig *app) {
    for (int i = 0; i < app->server_count; i++) {
        stop_server(&g_servers[i]);
    }
}

static void signal_handler(int sig) {
    (void)sig;
    g_should_stop = 1;
}

/* ------------------------------------------------------------
   User-facing output
------------------------------------------------------------ */

static void make_server_url(
    const AppConfig *app,
    const SiteConfig *site,
    char *out,
    size_t out_size
) {
    const char *host = app->host;

    if (strcmp(host, "0.0.0.0") == 0) {
        host = "127.0.0.1";
    }

    if (strcmp(host, "localhost") == 0) {
        host = "127.0.0.1";
    }

    snprintf(out, out_size, "http://%s:%d", host, site->port);
}

static void open_url_in_browser(const char *url) {
    pid_t pid = fork();

    if (pid == 0) {
        execlp("open", "open", url, (char *)NULL);
        _exit(127);
    }
}

static void print_startup_summary(const AppConfig *app) {
    if (app->quiet) {
        return;
    }

    printf(
        "%sLocal servers running:%s\n",
        colorize(app, C_GREEN),
        colorize(app, C_RESET)
    );

    for (int i = 0; i < app->server_count; i++) {
        char url[SMALL_BUFFER_SIZE];

        make_server_url(app, &app->sites[i], url, sizeof(url));

        printf(
            "  %s[%d]%s %s -> %s%s%s\n",
            colorize(app, C_BLUE),
            i + 1,
            colorize(app, C_RESET),
            app->sites[i].root_real,
            colorize(app, C_YELLOW),
            url,
            colorize(app, C_RESET)
        );

        for (int r = 0; r < app->sites[i].route_count; r++) {
            printf(
                "      route %s -> %s\n",
                app->sites[i].routes[r].mount,
                app->sites[i].routes[r].source_real
            );
        }
    }

    printf("\nPress Ctrl+C to stop.\n");
}

static void print_list(const AppConfig *app) {
    printf("Configured servers:\n");

    if (app->server_count == 0) {
        printf("  none\n");
        return;
    }

    for (int i = 0; i < app->server_count; i++) {
        const SiteConfig *site = &app->sites[i];

        char url[SMALL_BUFFER_SIZE];

        make_server_url(app, site, url, sizeof(url));

        printf("\n[%d] %s\n", i + 1, url);
        printf("    folder: %s\n", site->root_real[0] ? site->root_real : site->folder_input);
        printf("    port:   %d\n", site->port);
        printf(
            "    status: %s\n",
            check_port_available(app, site->port) ? "available" : "busy/unavailable"
        );

        for (int r = 0; r < site->route_count; r++) {
            printf(
                "    route:  %s -> %s\n",
                site->routes[r].mount,
                site->routes[r].source_real[0]
                    ? site->routes[r].source_real
                    : site->routes[r].source_input
            );
        }
    }
}

/* ------------------------------------------------------------
   main
------------------------------------------------------------ */

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    init_config(&g_config);

    char err[SMALL_BUFFER_SIZE];
    err[0] = '\0';

    if (!parse_args(&g_config, argc, argv, err, sizeof(err))) {
        log_error(&g_config, "Error: %s\n", err);
        return 1;
    }

    if (g_config.list_only && g_config.server_count == 0) {
        print_list(&g_config);
        return 0;
    }

    if (!validate_and_resolve_config(&g_config, err, sizeof(err))) {
        log_error(&g_config, "Error: %s\n", err);
        return 1;
    }

    if (g_config.list_only) {
        print_list(&g_config);
        return 0;
    }

    for (int i = 0; i < g_config.server_count; i++) {
        if (!check_port_available(&g_config, g_config.sites[i].port)) {
            log_error(
                &g_config,
                "Error: port %d is not available on %s.\n",
                g_config.sites[i].port,
                g_config.host
            );
            return 1;
        }
    }

    for (int i = 0; i < g_config.server_count; i++) {
        if (!start_server(&g_servers[i], &g_config, i, err, sizeof(err))) {
            log_error(&g_config, "Error: %s\n", err);
            stop_all_servers(&g_config);
            return 1;
        }
    }

    print_startup_summary(&g_config);

    if (g_config.open_browser) {
        for (int i = 0; i < g_config.server_count; i++) {
            char url[SMALL_BUFFER_SIZE];

            make_server_url(&g_config, &g_config.sites[i], url, sizeof(url));
            open_url_in_browser(url);
        }
    }

    while (!g_should_stop) {
        sleep(1);
    }

    log_normal(&g_config, "\nStopping servers...\n");

    stop_all_servers(&g_config);

    log_normal(&g_config, "Done.\n");

    return 0;
}
