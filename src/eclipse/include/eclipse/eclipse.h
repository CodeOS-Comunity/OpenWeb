/*
 * eclipse — rendering engine for CodeOS.
 *
 * Skeleton: litehtml (HTML/CSS layout) + cairo (2D graphics).
 * This C API is the stable interface CodeOS (and later the JS engine)
 * calls.  It renders into an ARGB32 buffer supplied by the host.
 *
 * Build-time: requires litehtml and cairo.
 */

#ifndef ECLIPSE_ECLIPSE_H
#define ECLIPSE_ECLIPSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eclipse_context eclipse_context;

/* Version of the engine (libtool-style). */
#define ECLIPSE_VERSION_MAJOR 0
#define ECLIPSE_VERSION_MINOR 1
#define ECLIPSE_VERSION_PATCH 0

/* ARGB32 pixel, 8 bits per channel. */
typedef uint32_t eclipse_argb_t;

/* HTTP response structure. */
typedef struct eclipse_http_response {
    int status_code;
    const char *content_type;
    uint8_t *body;
    size_t body_len;
    void *opaque; /* For internal use by host */
} eclipse_http_response_t;

/* Callbacks the host provides for things eclipse cannot do on its own. */
typedef struct eclipse_callbacks {
    /* Render a solid rectangle.  color is 0xAARRGGBB.
     * x, y are absolute coordinates in the page. */
    void (*draw_rect)(void *user_data, int x, int y, int w, int h,
                      uint32_t color);

    /* Load an image referenced by src (relative to base URL already
     * resolved by the container).  On success return 1 and fill *w/*h and
     * *pixels (ARGB32, top-down, w*h entries).  Return 0 if unavailable. */
    int (*load_image)(void *user_data, const char *src,
                      uint32_t **pixels, int *w, int *h);

    /* Fetch an HTTP resource (GET by default).  The host should perform
     * the network request and fill in the response.  The response body
     * will be freed by the host via a separate callback.  Return 1 on
     * success (response filled), 0 on failure. */
    int (*fetch)(void *user_data, const char *url, const char *method,
                 const char *headers, const uint8_t *body, size_t body_len,
                 eclipse_http_response_t *response);

    /* Free an HTTP response allocated by fetch(). */
    void (*fetch_free)(void *user_data, eclipse_http_response_t *response);

    /* Called when the user clicks an anchor.  Return 1 if handled. */
    int (*on_anchor_click)(void *user_data, const char *url);

    /* Opaque pointer passed to every callback. */
    void *user_data;
} eclipse_callbacks_t;

/* Create a render context.  callbacks may be NULL. */
eclipse_context *eclipse_create(const eclipse_callbacks_t *callbacks,
                                int width, int height);

/* Destroy a context. */
void eclipse_destroy(eclipse_context *ctx);

/* Parse and lay out an HTML document.  html may be NULL-terminated (len<0)
 * or have explicit length.  Width is the viewport width in pixels.
 * Returns 0 on success, -1 on failure. */
int eclipse_load_html(eclipse_context *ctx, const char *html, int len, int width);

/* Re-render the current document after a viewport resize. */
int eclipse_resize(eclipse_context *ctx, int width, int height);

/* Render the document into a caller-supplied ARGB32 buffer.
 * buf is top-down, stride is the byte stride between rows.
 * The buffer must be at least width*height pixels large.
 * Returns the height actually painted (may exceed viewport). */
int eclipse_render(eclipse_context *ctx, uint32_t *buf, int width, int height,
                   int stride);

/* Get the laid-out content size (may exceed the viewport). */
int eclipse_get_width(const eclipse_context *ctx);
int eclipse_get_height(const eclipse_context *ctx);

/* Input handling.  Return 1 if the event was consumed. */
int eclipse_on_click(eclipse_context *ctx, int x, int y);
int eclipse_on_scroll(eclipse_context *ctx, int dx, int dy);

/* Set a base URL against which relative resources resolve. */
void eclipse_set_base_url(eclipse_context *ctx, const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* ECLIPSE_ECLIPSE_H */
