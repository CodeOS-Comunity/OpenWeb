/*
 * eclipse_demo — renders a small HTML page through the eclipse engine and
 * writes the result to out.png.
 *
 * This exercises the public C API (eclipse_render_*) exactly the way
 * CodeOS will call it.  The cairo bits are only used here to persist
 * the ARGB32 buffer produced by eclipse_render() to a PNG file.
 */

#include <eclipse/eclipse.h>

#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* k_html =
    "<html><head><style>"
    "body { background: #f5f5f5; font-family: sans-serif; margin: 0; padding: 16px; }"
    "h1 { color: #1a6fb5; }"
    ".box { background: #ffffff; border: 1px solid #cccccc; border-radius: 8px; "
    "       padding: 12px; margin: 12px 0; }"
    "a { color: #1a6fb5; }"
    ".warn { color: #b5621a; font-weight: bold; }"
    "</style></head><body>"
    "<h1>Hello, tech-for-everyone here</h1>"
    "<p>eclipse renders HTML + CSS through <b>litehtml</b> and draws with "
    "<i>cairo</i>.</p>"
    "<div class=\"box\">A rounded, bordered box.</div>"
    "<p class=\"warn\">Skeleton engine — JavaScript comes later.</p>"
    "<a href=\"https://example.com\">A link</a>"
    "</body></html>";

int main(void)
{
    const int viewport_w = 640;
    const int viewport_h = 480;

    eclipse_context* ctx = eclipse_create(NULL, viewport_w, viewport_h);
    if(!ctx)
    {
        fprintf(stderr, "eclipse_create failed\n");
        return 1;
    }

    if(eclipse_load_html(ctx, k_html, (int)strlen(k_html), viewport_w) != 0)
    {
        fprintf(stderr, "eclipse_load_html failed\n");
        eclipse_destroy(ctx);
        return 1;
    }

    int page_w = eclipse_get_width(ctx);
    int page_h = eclipse_get_height(ctx);
    if(page_h <= 0)
    {
        page_h = viewport_h;
    }
    if(page_w <= 0)
    {
        page_w = viewport_w;
    }
    printf("document size: %d x %d\n", page_w, page_h);

    /* ARGB32 buffer, top-down, 4 bytes per pixel. */
    uint32_t* buf = (uint32_t*)calloc((size_t)page_w * (size_t)page_h, 4);
    if(!buf)
    {
        fprintf(stderr, "out of memory\n");
        eclipse_destroy(ctx);
        return 1;
    }

    int painted = eclipse_render(ctx, buf, page_w, page_h, page_w * 4);
    printf("painted height: %d\n", painted);

    /* Persist via cairo. */
    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        (unsigned char*)buf, CAIRO_FORMAT_ARGB32, page_w, page_h, page_w * 4);
    cairo_status_t st = cairo_surface_write_to_png(surf, "out.png");
    if(st != CAIRO_STATUS_SUCCESS)
    {
        fprintf(stderr, "cairo_surface_write_to_png failed: %s\n",
                cairo_status_to_string(st));
        cairo_surface_destroy(surf);
        free(buf);
        eclipse_destroy(ctx);
        return 1;
    }
    cairo_surface_destroy(surf);

    printf("wrote out.png (%d x %d)\n", page_w, page_h);

    free(buf);
    eclipse_destroy(ctx);
    return 0;
}