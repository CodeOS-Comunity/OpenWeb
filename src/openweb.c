#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "litehtml_renderer.h"

#define PORT 3000
#define REQUEST_SIZE 16384
#define RESPONSE_SIZE 262144
#define OW_MAX_REDIRECTS 8

static void send_body(int client, const char *status, const char *type,
                      const void *body, size_t length) {
    char header[512];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        status, type, length);
    send(client, header, (size_t)header_length, 0);
    send(client, body, length, 0);
}

static void send_text(int client, const char *status, const char *type,
                      const char *body) {
    send_body(client, status, type, body, strlen(body));
}

static void html_escape(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    const char *replacement;
    for (; *input && used + 1 < output_size; input++) {
        switch (*input) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default:
            output[used++] = *input;
            continue;
        }
        if (used + strlen(replacement) >= output_size) break;
        strcpy(output + used, replacement);
        used += strlen(replacement);
    }
    output[used] = '\0';
}

static void json_escape(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    for (; *input && used + 1 < output_size; input++) {
        const char *replacement = NULL;
        if (*input == '\\') replacement = "\\\\";
        else if (*input == '"') replacement = "\\\"";
        else if (*input == '\n') replacement = "\\n";
        else if (*input == '\r') replacement = "\\r";
        if (replacement) {
            size_t length = strlen(replacement);
            if (used + length >= output_size) break;
            memcpy(output + used, replacement, length);
            used += length;
        } else {
            output[used++] = *input;
        }
    }
    output[used] = '\0';
}

static int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static void url_decode(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    while (*input && used + 1 < output_size) {
        if (*input == '%' && hex_value(input[1]) >= 0 && hex_value(input[2]) >= 0) {
            output[used++] = (char)(hex_value(input[1]) * 16 + hex_value(input[2]));
            input += 3;
        } else {
            output[used++] = *input == '+' ? ' ' : *input;
            input++;
        }
    }
    output[used] = '\0';
}

static void json_string(const char *body, const char *key, char *output,
                        size_t output_size) {
    char marker[64];
    const char *value;
    size_t used = 0;
    snprintf(marker, sizeof(marker), "\"%s\"", key);
    value = strstr(body, marker);
    if (!value) return;
    value = strchr(value + strlen(marker), ':');
    if (!value) return;
    value++;
    while (*value && isspace((unsigned char)*value)) value++;
    if (*value != '"') return;
    for (value++; *value && *value != '"' && used + 1 < output_size; value++) {
        if (*value == '\\' && value[1]) value++;
        output[used++] = *value;
    }
    output[used] = '\0';
}

static const char *mime_type(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return "text/plain; charset=utf-8";
    if (strcmp(extension, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(extension, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(extension, ".js") == 0) return "application/javascript; charset=utf-8";
    return "application/octet-stream";
}

static void serve_file(int client, const char *path) {
    FILE *file = fopen(path, "rb");
    char *body;
    long length;
    if (!file) {
        send_text(client, "404 Not Found", "text/plain; charset=utf-8", "Not found\n");
        return;
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    if (length < 0 || length > RESPONSE_SIZE - 1 || !(body = malloc((size_t)length + 1))) {
        fclose(file);
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8", "File too large\n");
        return;
    }
    size_t bytes_read = fread(body, 1, (size_t)length, file);
    body[bytes_read] = '\0';
    fclose(file);
    send_text(client, "200 OK", mime_type(path), body);
    free(body);
}

/* ═══════════════════════════════════════════════════════════════════
   URL classification — mirrors kernel/kernel/rust_ow/src/lib.rs
   (is_unreserved / has_known_scheme / is_likely_url / ensure_scheme).
   A bare word with no dot is a search query; a dotted/hosted string or a
   known scheme is navigated directly with an "http://" scheme default.
   ═══════════════════════════════════════════════════════════════════ */

static int is_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

static int has_known_scheme(const char *url) {
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0 ||
           strncmp(url, "file://", 7) == 0 || strncmp(url, "data://", 7) == 0;
}

static int is_likely_url(const char *input) {
    size_t i;
    if (!input || !input[0]) return 0;
    if (has_known_scheme(input)) return 1;
    if (strncmp(input, "localhost", 9) == 0) return 1;
    for (i = 0; input[i]; i++)
        if (isspace((unsigned char)input[i]) || iscntrl((unsigned char)input[i]))
            return 0;
    return strchr(input, '.') != NULL;
}

static void ensure_scheme(char *url, size_t size) {
    size_t len;
    if (!url[0] || has_known_scheme(url)) return;
    len = strlen(url);
    if (len + 7 >= size) return;
    memmove(url + 7, url, len + 1);
    memcpy(url, "http://", 7);
}

/* RFC 3986 percent-encoding of a query string — same rule as rust_ow's
 * build_search_url: unreserved characters pass through, everything else
 * becomes %XX (uppercase hex). */
static void percent_encode_query(const char *input, char *output, size_t output_size) {
    size_t i, used = 0;
    for (i = 0; input[i] && used + 3 < output_size; i++) {
        unsigned char c = (unsigned char)input[i];
        if (is_unreserved((char)c)) output[used++] = (char)c;
        else {
            output[used++] = '%';
            output[used++] = "0123456789ABCDEF"[c >> 4];
            output[used++] = "0123456789ABCDEF"[c & 0x0F];
        }
    }
    output[used] = '\0';
}

/* Search URL builder. Defaults to the same Google endpoint as the kernel
 * (http://www.google.com/search?q=). "ddg" is an opt-in alternative. */
static void build_engine_url(const char *query, const char *engine,
                             char *url, size_t url_size) {
    char encoded[2000];
    percent_encode_query(query, encoded, sizeof(encoded));
    if (strcmp(engine, "ddg") == 0)
        snprintf(url, url_size, "https://duckduckgo.com/html/?q=%s", encoded);
    else
        snprintf(url, url_size, "http://www.google.com/search?q=%s", encoded);
}

static void serve_search(int client, const char *body) {
    char query[2048] = "";
    char engine[32] = "google";
    char url[4096];
    json_string(body, "query", query, sizeof(query));
    json_string(body, "engine", engine, sizeof(engine));
    if (is_likely_url(query)) {
        snprintf(url, sizeof(url), "%s", query);
        ensure_scheme(url, sizeof(url));
    } else {
        build_engine_url(query, engine, url, sizeof(url));
    }
    char response[8192];
    int direct = is_likely_url(query);
    const char *selected_engine = direct ? (strncmp(url, "https://", 8) == 0 ? "https" : "http")
                                         : (strcmp(engine, "ddg") == 0 ? "duckduckgo" : "google");
    const char *mode = direct ? "direct" : "search";
    char safe_query[4096], safe_url[8192];
    json_escape(query, safe_query, sizeof(safe_query));
    json_escape(url, safe_url, sizeof(safe_url));
    snprintf(response, sizeof(response),
        "{\"engine\":\"%s\",\"query\":\"%.2000s\",\"url\":\"%.4000s\",\"mode\":\"%s\"}",
        selected_engine, safe_query, safe_url, mode);
    send_text(client, "200 OK", "application/json; charset=utf-8", response);
}

/* ═══════════════════════════════════════════════════════════════════
   Fetching — mirrors kernel/kernel/rust_ow/src/lib.rs classify_and_fetch:
   GET over a plain TCP socket, status parsing, a bounded redirect chain
   (up to OW_MAX_REDIRECTS), and the same failure statuses the kernel tab
   shows ("IP needed:", "No data:", "HTTP error N", "Too many redirects",
   "Redirect without Location").
   ═══════════════════════════════════════════════════════════════════ */

static int parse_host_port_path(const char *url, char *host, size_t host_size,
                                unsigned short *port, char *path, size_t path_size) {
    const char *rest = strstr(url, "://");
    const char *slash;
    const char *colon;
    int is_https = strncmp(url, "https://", 8) == 0;
    size_t hl;
    if (rest) rest += 3; else rest = url;
    slash = strchr(rest, '/');
    hl = slash ? (size_t)(slash - rest) : strlen(rest);
    if (!hl || hl >= host_size) return -1;
    memcpy(host, rest, hl);
    host[hl] = '\0';
    *port = is_https ? 443 : 80;
    colon = strrchr(host, ':');
    if (colon) {
        int explicit_port = atoi(colon + 1);
        if (explicit_port > 0 && explicit_port <= 65535) *port = (unsigned short)explicit_port;
        host[colon - host] = '\0';
    }
    if (slash) {
        size_t pl = strlen(slash);
        if (pl >= path_size) pl = path_size - 1;
        memcpy(path, slash, pl);
        path[pl] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}

/* Resolve a (possibly relative / scheme-relative) Location header against the
 * current URL. Unlike rust_ow's resolver (which keeps only scheme://host), this
 * joins path-relative locations against the current directory. */
static int resolve_location(const char *cur, const char *loc, char *out, size_t out_size) {
    const char *scheme_end, *auth_start, *slash;
    if (!loc || !loc[0]) return 0;
    if (has_known_scheme(loc) || strncmp(loc, "//", 2) == 0) {
        snprintf(out, out_size, "%s", loc);
        return 1;
    }
    scheme_end = strstr(cur, "://");
    if (!scheme_end) { snprintf(out, out_size, "%s", loc); return 1; }
    scheme_end += 3;
    auth_start = scheme_end;
    slash = strchr(auth_start, '/');
    if (loc[0] == '/') {
        size_t auth_end = slash ? (size_t)(slash - cur) : strlen(cur);
        snprintf(out, out_size, "%.*s%s", (int)auth_end, cur, loc);
        return 1;
    }
    {
        const char *cut = slash ? slash + 1 : (auth_start + strlen(auth_start));
        snprintf(out, out_size, "%.*s%s", (int)(cut - cur), cur, loc);
        return 1;
    }
}

static int parse_http_status(const char *data, size_t len) {
    if (len < 12 || strncmp(data, "HTTP/", 5) != 0) return 0;
    if (data[9] < '0' || data[9] > '9') return 0;
    return (data[9] - '0') * 100 + (data[10] - '0') * 10 + (data[11] - '0');
}

static size_t header_body_boundary(const char *data, size_t len) {
    size_t i;
    for (i = 0; i + 3 < len; i++)
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return i + 4;
    return len;
}

static void extract_header_value(const char *head, size_t head_len,
                                 const char *name, char *out, size_t out_size) {
    const char *p = head;
    size_t name_len = strlen(name);
    out[0] = '\0';
    while (p && (size_t)(p - head) < head_len) {
        const char *eol = memchr(p, '\n', (size_t)(head + head_len - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(head + head_len - p);
        size_t trimmed = line_len;
        while (trimmed > 0 && (p[trimmed - 1] == '\r' || p[trimmed - 1] == '\n')) trimmed--;
        if (trimmed >= name_len &&
            strncasecmp(p, name, name_len) == 0 &&
            (trimmed == name_len || p[name_len] == ':')) {
            const char *value = p + name_len;
            if (*value == ':') value++;
            while (*value == ' ' || *value == '\t') value++;
            if ((size_t)(p + trimmed - value) < out_size) {
                memcpy(out, value, (size_t)(p + trimmed - value));
                out[p + trimmed - value] = '\0';
            }
            return;
        }
        if (!eol) break;
        p = eol + 1;
    }
}

enum {
    OW_FETCH_OK = 0,
    OW_FETCH_FAIL,
    OW_FETCH_EMPTY,
    OW_FETCH_TOOMANY,
    OW_FETCH_NOREDIR,
    OW_FETCH_STATUS
};

static int fetch_url(const char *url, char *body_out, size_t body_cap,
                     size_t *body_len_out, char *err, size_t err_cap) {
    char current[4096];
    char *resp = NULL;
    size_t resp_cap = body_cap + 4096;
    int redirects = 0;
    snprintf(current, sizeof(current), "%s", url);
    *body_len_out = 0;
    for (;;) {
        char host[128] = "", path[512] = "/";
        unsigned short port = 80;
        char host_buf[64];
        struct addrinfo hints, *address = NULL;
        int fd = -1, status = 0;
        size_t total = 0, boundary;
        if (parse_host_port_path(current, host, sizeof(host), &port, path, sizeof(path)) < 0) {
            snprintf(err, err_cap, "Bad URL: %s", current);
            if (resp) free(resp);
            return OW_FETCH_FAIL;
        }
        snprintf(host_buf, sizeof(host_buf), "%u", (unsigned)port);
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, host_buf, &hints, &address) != 0 || !address) {
            if (address) freeaddrinfo(address);
            snprintf(err, err_cap, "IP needed: %s", host);
            if (resp) free(resp);
            return OW_FETCH_FAIL;
        }
        {
            struct addrinfo *candidate;
            for (candidate = address; candidate; candidate = candidate->ai_next) {
                fd = socket(candidate->ai_family, candidate->ai_socktype,
                            candidate->ai_protocol);
                if (fd < 0) continue;
                if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) < 0) {
                    close(fd);
                    fd = -1;
                    continue;
                }
                break;
            }
        }
        freeaddrinfo(address);
        if (fd < 0) {
            snprintf(err, err_cap, "IP needed: %s", host);
            if (resp) free(resp);
            return OW_FETCH_FAIL;
        }
        {
            char request[1024];
            int request_len = snprintf(request, sizeof(request),
                "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\n"
                "Connection: close\r\n\r\n", path, host);
            send(fd, request, (size_t)request_len, MSG_NOSIGNAL);
        }
        if (!resp) {
            resp = malloc(resp_cap);
            if (!resp) { close(fd); snprintf(err, err_cap, "Out of memory"); return OW_FETCH_FAIL; }
        }
        for (;;) {
            ssize_t n = recv(fd, resp + total, resp_cap - total, 0);
            if (n <= 0) break;
            total += (size_t)n;
            if (total >= resp_cap) break;
        }
        close(fd);
        if (total == 0) {
            snprintf(err, err_cap, "No data: %s", host);
            free(resp);
            resp = NULL;
            return OW_FETCH_EMPTY;
        }
        status = parse_http_status(resp, total);

        if (status >= 300 && status < 400) {
            char location[512];
            char resolved[4096];
            if (++redirects >= OW_MAX_REDIRECTS) {
                snprintf(err, err_cap, "Too many redirects");
                free(resp); resp = NULL;
                return OW_FETCH_TOOMANY;
            }
            extract_header_value(resp, total, "location", location, sizeof(location));
            if (!resolve_location(current, location, resolved, sizeof(resolved))) {
                snprintf(err, err_cap, "Redirect without Location");
                free(resp); resp = NULL;
                return OW_FETCH_NOREDIR;
            }
            snprintf(current, sizeof(current), "%s", resolved);
            free(resp);
            resp = NULL;
            continue;
        }
        if (status >= 400) {
            snprintf(err, err_cap, "HTTP error %d", status);
            free(resp); resp = NULL;
            return OW_FETCH_STATUS;
        }

        boundary = header_body_boundary(resp, total);
        if (total > boundary) {
            size_t length = total - boundary;
            if (length > body_cap) length = body_cap;
            memcpy(body_out, resp + boundary, length);
            *body_len_out = length;
        }
        free(resp);
        resp = NULL;
        return OW_FETCH_OK;
    }
}

static void serve_render(int client, const char *request) {
    char target[4096] = "http://www.google.com";
    char decoded[4096], safe_target[8192];
    char err[192] = "";
    char *page = NULL;
    size_t body_len = 0;
    int code;
    const char *value = strstr(request, "target=");
    if (value) {
        value += 7;
        sscanf(value, "%4095[^ &]", target);
        url_decode(target, decoded, sizeof(decoded));
        snprintf(target, sizeof(target), "%s", decoded);
    }
    ensure_scheme(target, sizeof(target));

    page = malloc(RESPONSE_SIZE);
    if (!page) {
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "Unable to allocate render surface\n");
        return;
    }
    code = fetch_url(target, page, RESPONSE_SIZE, &body_len, err, sizeof(err));
    if (code != OW_FETCH_OK) {
        char safe_err[8192];
        int written;
        html_escape(err, safe_err, sizeof(safe_err));
        html_escape(target, safe_target, sizeof(safe_target));
        safe_target[256] = '\0';
        written = snprintf(page, RESPONSE_SIZE,
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>OpenWeb</title>"
            "<style>html,body{width:1024px;height:768px;margin:0;font-family:sans-serif}"
            "body{background:#1d1e1f;color:#111;display:flex;align-items:center;justify-content:center}"
            ".card{width:600px;box-sizing:border-box;padding:16px 20px;background:#fff}"
            "h1{margin:0 0 10px;color:#c0392b;font-size:26px}"
            "p{margin:0 0 8px;font-size:15px;color:#333;word-break:break-all}"
            "</style></head><body>"
            "<div class=\"card\"><h1>%s</h1><p>%s</p></div>"
            "</body></html>",
            safe_err[0] ? safe_err : "Unknown error", safe_target);
        body_len = written > 0 && (size_t)written < RESPONSE_SIZE
                       ? (size_t)written : 0;
    }

    char output_path[] = "/tmp/openweb-render-XXXXXX";
    int output = mkstemp(output_path);
    if (output < 0) {
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "Unable to create render surface\n");
        free(page);
        return;
    }
    close(output);
    if (!render_html_to_png(page, body_len, output_path)) {
        unlink(output_path);
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "litehtml could not render the page\n");
        free(page);
        return;
    }
    FILE *image = fopen(output_path, "rb");
    if (!image) {
        unlink(output_path);
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "Unable to open rendered page\n");
        free(page);
        return;
    }
    fseek(image, 0, SEEK_END);
    long length = ftell(image);
    rewind(image);
    unsigned char *pixels = length > 0 ? malloc((size_t)length) : NULL;
    size_t bytes_read = pixels ? fread(pixels, 1, (size_t)length, image) : 0;
    fclose(image);
    unlink(output_path);
    free(page);
    if (!pixels || bytes_read != (size_t)length) {
        free(pixels);
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "Unable to read rendered page\n");
        return;
    }
    send_body(client, "200 OK", "image/png", pixels, bytes_read);
    free(pixels);
}

static void handle_client(int client) {
    char request[REQUEST_SIZE + 1] = {0};
    ssize_t received = recv(client, request, REQUEST_SIZE, 0);
    if (received <= 0) return;
    request[received] = '\0';
    char method[8], path[4096];
    sscanf(request, "%7s %4095s", method, path);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        serve_file(client, "static/index.html");
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/static/", 8) == 0 &&
               !strstr(path, "..")) {
        char local_path[4096];
        snprintf(local_path, sizeof(local_path), "static/%s", path + 8);
        serve_file(client, local_path);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/render?", 8) == 0) {
        serve_render(client, path);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/search") == 0) {
        const char *payload = strstr(request, "\r\n\r\n");
        serve_search(client, payload ? payload + 4 : "{}");
    } else {
        send_text(client, "404 Not Found", "text/plain; charset=utf-8", "Not found\n");
    }
}

int main(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int option = 1;
    struct sockaddr_in address = {0};
    signal(SIGPIPE, SIG_IGN);
    if (server < 0) return 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 16) < 0) {
        perror("OpenWeb socket");
        close(server);
        return 1;
    }
    printf("OpenWeb listening on http://127.0.0.1:%d\n", PORT);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client >= 0) {
            handle_client(client);
            close(client);
        }
    }
}