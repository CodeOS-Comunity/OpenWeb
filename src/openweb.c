#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "litehtml_renderer.h"

#define PORT 3000
#define REQUEST_SIZE 16384
#define RESPONSE_SIZE 262144

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

static void normalize_url(const char *input, char *output, size_t output_size) {
    while (isspace((unsigned char)*input)) input++;
    if (strncmp(input, "http://", 7) == 0 || strncmp(input, "https://", 8) == 0) {
        snprintf(output, output_size, "%s", input);
    } else {
        snprintf(output, output_size, "https://%s", input);
    }
}

static int looks_like_url(const char *input) {
    return strstr(input, "://") || strstr(input, "localhost") ||
           strstr(input, "127.") || (strchr(input, '.') && !strchr(input, ' '));
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

static void serve_search(int client, const char *body) {
    char query[2048] = "";
    char engine[32] = "google";
    char url[4096];
    json_string(body, "query", query, sizeof(query));
    json_string(body, "engine", engine, sizeof(engine));
    if (looks_like_url(query)) {
        normalize_url(query, url, sizeof(url));
    } else {
        char encoded[2048] = "";
        size_t used = 0;
        for (size_t i = 0; query[i] && used + 1 < sizeof(encoded); i++) {
            encoded[used++] = query[i] == ' ' ? '+' : query[i];
        }
        encoded[used] = '\0';
        snprintf(url, sizeof(url), "https://%s.com/search?q=%s",
                 strcmp(engine, "ddg") == 0 ? "duckduckgo" : "www.google", encoded);
    }
    char response[8192];
    const char *selected_engine = looks_like_url(query) ? "https" :
        (strcmp(engine, "ddg") == 0 ? "duckduckgo" : "google");
    const char *mode = looks_like_url(query) ? "direct" : "search";
    char safe_query[4096], safe_url[8192];
    json_escape(query, safe_query, sizeof(safe_query));
    json_escape(url, safe_url, sizeof(safe_url));
    snprintf(response, sizeof(response),
        "{\"engine\":\"%s\",\"query\":\"%.2000s\",\"url\":\"%.4000s\",\"mode\":\"%s\"}",
        selected_engine, safe_query, safe_url, mode);
    send_text(client, "200 OK", "application/json; charset=utf-8", response);
}

static void serve_render(int client, const char *request) {
    char target[4096] = "https://codeos.dev";
    char decoded[4096], safe_target[8192];
    const char *value = strstr(request, "target=");
    if (value) {
        value += 7;
        sscanf(value, "%4095[^ &]", target);
        url_decode(target, decoded, sizeof(decoded));
        snprintf(target, sizeof(target), "%s", decoded);
    }
    normalize_url(target, decoded, sizeof(decoded));
    html_escape(decoded, safe_target, sizeof(safe_target));
    char page[16384];
    snprintf(page, sizeof(page),
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>OpenWeb secure view</title>"
        "<style>html,body{width:1024px;height:768px;margin:0}"
        "body{font-family:sans-serif;background:#1d1e1f;color:#111;position:relative}"
        ".card{position:absolute;left:192px;top:300px;width:600px;min-height:205px;"
        "box-sizing:border-box;padding:10px 16px;background:#fff}"
        "h1{margin:0 0 28px;color:#176aa3;font-size:32px}"
        "p{margin:0 0 12px;font-size:16px}"
        ".sample{padding:10px 12px;border:1px solid #c6c6c6;border-radius:9px;background:#fff}"
        ".note{color:#a36124;font-weight:bold}.link{color:#176aa3}</style></head><body>"
        "<div class=\"card\"><h1>Hello, tech-for-everyone here</h1>"
        "<p>OpenWeb renders HTML + CSS through litehtml and draws with Cairo.</p>"
        "<p class=\"sample\">A rounded, bordered box.</p>"
        "<p class=\"note\">Cairo surface renderer - JavaScript comes later.</p>"
        "<p><a class=\"link\" href=\"%.4000s\">Open externally</a></p></div>"
        "</body></html>", safe_target);
    char output_path[] = "/tmp/openweb-render-XXXXXX";
    int output = mkstemp(output_path);
    if (output < 0) {
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "Unable to create render surface\n");
        return;
    }
    close(output);
    if (!render_html_to_png(page, strlen(page), output_path)) {
        unlink(output_path);
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "litehtml could not render the page\n");
        return;
    }
    FILE *image = fopen(output_path, "rb");
    if (!image) {
        unlink(output_path);
        send_text(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "Unable to open rendered page\n");
        return;
    }
    fseek(image, 0, SEEK_END);
    long length = ftell(image);
    rewind(image);
    unsigned char *pixels = length > 0 ? malloc((size_t)length) : NULL;
    size_t bytes_read = pixels ? fread(pixels, 1, (size_t)length, image) : 0;
    fclose(image);
    unlink(output_path);
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
