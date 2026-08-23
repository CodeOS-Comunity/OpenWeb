#ifndef OPENWEB_LITEHTML_RENDERER_H
#define OPENWEB_LITEHTML_RENDERER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int render_html_to_png(const char *html, size_t html_length,
                      const char *output_path);

#ifdef __cplusplus
}
#endif

#endif
