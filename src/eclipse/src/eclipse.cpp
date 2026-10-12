/*
 * eclipse — CodeOS rendering engine.
 *
 * Skeleton implementation:
 *   - litehtml drives HTML/CSS parsing and layout.
 *   - the document_container is implemented on top of cairo.
 *   - text is drawn with cairo's toy font API (no pango/GTK dependency),
 *     which is enough for a working skeleton.  A freetype/harfbuzz text
 *     shaper can be dropped in later.
 */

#include "eclipse/eclipse.h"

#include <cairo.h>
#include <litehtml.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <litehtml/containers/cairo/container_cairo.h>
#include <litehtml/containers/cairo/cairo_images_cache.h>

/* ------------------------------------------------------------------ */
/* Font handling                                                       */
/* ------------------------------------------------------------------ */

namespace
{

struct eclipse_font
{
    std::string family;
    double      size;
    int         weight;
    litehtml::font_style style;
    cairo_font_weight_t  cairo_weight;
    cairo_font_slant_t   cairo_slant;
};

const char* font_family_or_default(const std::string& family)
{
    if(family.empty() || family == "sans-serif" || family == "serif" ||
       family == "monospace" || family == "cursive" || family == "fantasy")
    {
        return "sans";
    }
    return family.c_str();
}

/* Eclipse's concrete document_container.  Handles the parts container_cairo
 * leaves abstract: fonts, text, images, viewport, and host callbacks. */
class eclipse_container : public container_cairo
{
  public:
    explicit eclipse_container(const eclipse_callbacks_t* cbs, int width, int height) :
        m_callbacks(cbs), m_width(width), m_height(height), m_dpi(96.0)
    {
    }

    ~eclipse_container() override
    {
        for(auto& pair : m_fonts)
        {
            delete pair.second;
        }
    }

    void set_viewport(int width, int height)
    {
        m_width  = width;
        m_height = height;
    }

    void set_base_url(const char* base_url)
    {
        m_base_url = base_url ? base_url : "";
    }

    /* -- viewport / screen -------------------------------------------------- */

    litehtml::pixel_t pt_to_px(float pt) const override
    {
        return container_cairo::pt_to_px(pt);
    }

    void get_viewport(litehtml::position& viewport) const override
    {
        viewport.x      = 0;
        viewport.y      = 0;
        viewport.width  = m_width;
        viewport.height = m_height;
    }

    int get_screen_width() const override
    {
        return m_width;
    }

    int get_screen_height() const override
    {
        return m_height;
    }

    double get_screen_dpi() const override
    {
        return m_dpi;
    }

    /* -- fonts / text -------------------------------------------------------- */

    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
                                   const litehtml::document* /*doc*/,
                                   litehtml::font_metrics* fm) override
    {
        auto* font            = new eclipse_font();
        font->family          = descr.family;
        font->size            = descr.size.value();
        font->weight          = descr.weight;
        font->style           = descr.style;
        font->cairo_weight    = descr.weight >= 600 ? CAIRO_FONT_WEIGHT_BOLD
                                                    : CAIRO_FONT_WEIGHT_NORMAL;
        font->cairo_slant     = descr.style == litehtml::font_style_italic
                                    ? CAIRO_FONT_SLANT_ITALIC
                                    : CAIRO_FONT_SLANT_NORMAL;

        if(fm)
        {
            fm->font_size = descr.size;
            fm->height    = descr.size;
            fm->ascent    = descr.size.value() * 0.8f;
            fm->descent   = descr.size.value() * 0.2f;
            fm->x_height  = descr.size.value() * 0.5f;
            fm->ch_width  = descr.size.value() * 0.5f;
        }
        m_fonts.emplace(reinterpret_cast<litehtml::uint_ptr>(font), font);
        return reinterpret_cast<litehtml::uint_ptr>(font);
    }

    void delete_font(litehtml::uint_ptr hFont) override
    {
        auto it = m_fonts.find(hFont);
        if(it != m_fonts.end())
        {
            delete it->second;
            m_fonts.erase(it);
        }
    }

    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override
    {
        auto it = m_fonts.find(hFont);
        if(it == m_fonts.end() || !text)
        {
            return 0;
        }
        eclipse_font* font = it->second;

        /* Measure with a throwaway cairo context. */
        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t*         cr   = cairo_create(surf);
        cairo_select_font_face(cr, font_family_or_default(font->family),
                               font->cairo_slant, font->cairo_weight);
        cairo_set_font_size(cr, font->size);
        cairo_text_extents_t extents;
        cairo_text_extents(cr, text, &extents);
        double advance = extents.x_advance;
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return static_cast<float>(advance);
    }

    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position& pos) override
    {
        auto it = m_fonts.find(hFont);
        if(it == m_fonts.end() || !text)
        {
            return;
        }
        eclipse_font* font = it->second;
        cairo_t*    cr   = reinterpret_cast<cairo_t*>(hdc);
        if(!cr)
        {
            return;
        }

        cairo_save(cr);
        apply_clip(cr);
        cairo_select_font_face(cr, font_family_or_default(font->family),
                               font->cairo_slant, font->cairo_weight);
        cairo_set_font_size(cr, font->size);
        cairo_move_to(cr, pos.x.value(), pos.y.value());
        set_color(cr, color);
        cairo_show_text(cr, text);
        cairo_restore(cr);
    }

    /* -- images -------------------------------------------------------------- */

    void load_image(const char* src, const char* /*baseurl*/,
                    bool /*redraw_on_ready*/) override
    {
        (void)src;
    }

    /* Resolve a relative URL against the base URL. */
    std::string resolve_url(const std::string& url) const
    {
        if(url.empty())
            return "";
        /* Already absolute? */
        if(url.rfind("http://", 0) == 0 ||
           url.rfind("https://", 0) == 0 ||
           url.rfind("data:", 0) == 0 ||
           url[0] == '/')
        {
            return url;
        }
        /* Relative URL - resolve against base */
        if(m_base_url.empty())
            return url;
        std::string base = m_base_url;
        if(!base.empty() && base.back() != '/')
            base += '/';
        return base + url;
    }

    cairo_surface_t* get_image(const std::string& url) override
    {
        if(url.empty())
        {
            return nullptr;
        }

        std::string resolved = resolve_url(url);
        if(resolved.empty())
        {
            return nullptr;
        }

        if(m_cache.exists(resolved))
        {
            return m_cache.get_image(resolved);
        }

        /* Try fetch callback first (for network images) */
        if(m_callbacks && m_callbacks->fetch && m_callbacks->fetch_free)
        {
            eclipse_http_response_t resp = {0};
            if(m_callbacks->fetch(m_callbacks->user_data, resolved.c_str(), "GET", nullptr, nullptr, 0, &resp))
            {
                if(resp.status_code == 200 && resp.body && resp.body_len > 0)
                {
                    /* For now, we'll just pass the raw bytes to a simple image loader.
                     * In a real implementation, you'd use a proper image decoder (libpng, libjpeg, etc.) */
                    cairo_surface_t* surf = load_image_from_memory(resp.body, resp.body_len);
                    if(surf)
                    {
                        m_cache.add_image(resolved, surf);
                        m_callbacks->fetch_free(m_callbacks->user_data, &resp);
                        return m_cache.get_image(resolved);
                    }
                }
                m_callbacks->fetch_free(m_callbacks->user_data, &resp);
            }
        }

        /* Fallback to load_image callback for local/embedded images */
        if(m_callbacks && m_callbacks->load_image)
        {
            uint32_t* pixels = nullptr;
            int       w = 0, h = 0;
            if(m_callbacks->load_image(m_callbacks->user_data, resolved.c_str(),
                                       &pixels, &w, &h) &&
               pixels && w > 0 && h > 0)
            {
                cairo_surface_t* surf =
                    cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(pixels),
                                                        CAIRO_FORMAT_ARGB32, w, h,
                                                        w * 4);
                m_cache.add_image(resolved, surf);
                cairo_surface_destroy(surf);
                return m_cache.get_image(resolved);
            }
        }
        return nullptr;
    }

  private:
    struct PngState { const uint8_t* ptr; size_t len; size_t pos; };

    static cairo_status_t png_read_func(void* closure, unsigned char* data, unsigned int length)
    {
        PngState* state = static_cast<PngState*>(closure);
        if(state->pos + length > state->len)
            length = state->len - state->pos;
        if(length == 0)
            return CAIRO_STATUS_READ_ERROR;
        std::memcpy(data, state->ptr + state->pos, length);
        state->pos += length;
        return CAIRO_STATUS_SUCCESS;
    }

    cairo_surface_t* load_image_from_memory(const uint8_t* data, size_t len)
    {
        if(len < 8)
            return nullptr;
        /* PNG signature: 89 50 4E 47 0D 0A 1A 0A */
        if(data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
           data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
        {
            PngState state{data, len, 0};
            return cairo_image_surface_create_from_png_stream(png_read_func, &state);
        }
        return nullptr;
    }

    /* -- host callbacks ------------------------------------------------------ */

    void set_caption(const char* /*caption*/) override
    {
    }

    const std::string& base_url() const
    {
        return m_base_url;
    }

    void on_anchor_click(const char* url, const litehtml::element::ptr& /*el*/) override
    {
        if(m_callbacks && m_callbacks->on_anchor_click && url)
        {
            m_callbacks->on_anchor_click(m_callbacks->user_data, url);
        }
    }

    void on_mouse_event(const litehtml::element::ptr& /*el*/,
                        litehtml::mouse_event /*event*/) override
    {
    }

    void set_cursor(const char* /*cursor*/) override
    {
    }

    void import_css(std::string& /*text*/, const std::string& /*url*/,
                    std::string& /*baseurl*/) override
    {
    }

    /* -- helpers used by eclipse.cpp -------------------------------------------- */

    const eclipse_callbacks_t* callbacks() const
    {
        return m_callbacks;
    }

  private:
    const eclipse_callbacks_t* m_callbacks;
    int                      m_width;
    int                      m_height;
    double                   m_dpi;
    std::string              m_base_url;
    std::map<litehtml::uint_ptr, eclipse_font*> m_fonts;
    cairo_images_cache       m_cache;
};

} // namespace

/* ------------------------------------------------------------------ */
/* C API implementation                                                */
/* ------------------------------------------------------------------ */

struct eclipse_context
{
    std::shared_ptr<eclipse_container> container;
    std::shared_ptr<litehtml::document> document;
    std::string base_url;
    int width  = 0;
    int height = 0;
};

extern "C" {

eclipse_context* eclipse_create(const eclipse_callbacks_t* callbacks,
                                int width, int height)
{
    auto* ctx            = new eclipse_context();
    ctx->width           = width;
    ctx->height          = height;
    ctx->container       = std::make_shared<eclipse_container>(callbacks, width, height);
    return ctx;
}

void eclipse_destroy(eclipse_context* ctx)
{
    if(ctx)
    {
        delete ctx;
    }
}

int eclipse_load_html(eclipse_context* ctx, const char* html, int len, int width)
{
    if(!ctx || !ctx->container || !html || width <= 0)
    {
        return -1;
    }

    std::string src(html, len < 0 ? std::strlen(html) : static_cast<size_t>(len));

    ctx->container->set_viewport(width, ctx->height);
    ctx->document = litehtml::document::createFromString(src, ctx->container.get());
    if(!ctx->document)
    {
        return -1;
    }
    ctx->document->render(width);
    ctx->width = width;
    return 0;
}

int eclipse_resize(eclipse_context* ctx, int width, int height)
{
    if(!ctx || !ctx->document || width <= 0 || height <= 0)
    {
        return -1;
    }
    ctx->container->set_viewport(width, height);
    ctx->width  = width;
    ctx->height = height;
    ctx->document->render(width);
    return 0;
}

int eclipse_render(eclipse_context* ctx, uint32_t* buf, int width, int height,
                   int stride)
{
    if(!ctx || !ctx->document || !buf || width <= 0 || height <= 0 || stride <= 0)
    {
        return 0;
    }

    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(buf), CAIRO_FORMAT_ARGB32, width,
        height, stride);
    cairo_t* cr = cairo_create(surf);

    /* Transparent page background (body paints its own). */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    ctx->document->draw(reinterpret_cast<litehtml::uint_ptr>(cr), 0, 0, nullptr);

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    return ctx->document->height().value();
}

int eclipse_get_width(const eclipse_context* ctx)
{
    if(!ctx || !ctx->document)
    {
        return ctx ? ctx->width : 0;
    }
    return ctx->document->width().value();
}

int eclipse_get_height(const eclipse_context* ctx)
{
    if(!ctx || !ctx->document)
    {
        return 0;
    }
    return ctx->document->height().value();
}

int eclipse_on_click(eclipse_context* ctx, int x, int y)
{
    if(!ctx || !ctx->document)
    {
        return 0;
    }
    return ctx->document->on_lbutton_down(x, y, x, y, nullptr) ? 1 : 0;
}

int eclipse_on_scroll(eclipse_context* ctx, int /*dx*/, int /*dy*/)
{
    if(!ctx || !ctx->document)
    {
        return 0;
    }
    return 1;
}

void eclipse_set_base_url(eclipse_context* ctx, const char* base_url)
{
    if(!ctx || !ctx->container)
    {
        return;
    }
    ctx->base_url = base_url ? base_url : "";
    ctx->container->set_base_url(ctx->base_url.c_str());
}

} // extern "C"