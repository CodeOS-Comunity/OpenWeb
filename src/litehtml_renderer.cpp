#include "litehtml_renderer.h"

#include <cairo.h>
#include <litehtml.h>
#include <string>

#include "container_cairo_pango.h"

class OpenWebContainer final : public container_cairo_pango {
public:
    void load_image(const char *, const char *, bool) override {}
    void set_caption(const char *) override {}
    void set_base_url(const char *) override {}
    void on_anchor_click(const char *, const litehtml::element::ptr &) override {}
    void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
    void set_cursor(const char *) override {}
    void import_css(std::string &, const std::string &, std::string &) override {}

    cairo_surface_t *get_image(const std::string &) override { return nullptr; }
    double get_screen_dpi() const override { return 96.0; }
    int get_screen_width() const override { return 1024; }
    int get_screen_height() const override { return 768; }

    void get_viewport(litehtml::position &viewport) const override {
        viewport = litehtml::position(0, 0, get_screen_width(), get_screen_height());
    }
};

extern "C" int render_html_to_png(const char *html, size_t html_length,
                                   const char *output_path) {
    if (!html || !output_path) return 0;

    OpenWebContainer container;
    std::string source(html, html_length);
    auto document = litehtml::document::createFromString(source, &container);
    if (!document) return 0;

    document->render(container.get_screen_width());
    int width = document->width() > 0 ? document->width() : container.get_screen_width();
    int height = document->height() > 0 ? document->height() : container.get_screen_height();
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return 0;
    }

    cairo_t *context = cairo_create(surface);
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_paint(context);
    litehtml::position clip(0, 0, width, height);
    document->draw(reinterpret_cast<litehtml::uint_ptr>(context), 0, 0, &clip);
    cairo_destroy(context);
    cairo_status_t status = cairo_surface_write_to_png(surface, output_path);
    cairo_surface_destroy(surface);
    return status == CAIRO_STATUS_SUCCESS ? 1 : 0;
}
