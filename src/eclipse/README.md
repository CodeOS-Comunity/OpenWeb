# eclipse — CodeOS rendering engine

Skeleton engine: **litehtml** parses HTML/CSS and lays out the page;
**cairo** does the 2D drawing.  A JavaScript engine is planned to sit
alongside eclipse and turn it into a full rendering engine.

## Layout

```
include/eclipse/eclipse.h   public C API (eclipse_*)
src/eclipse.cpp             engine: document_container on top of cairo
demo/demo.c                 renders an HTML page to out.png
litehtml/                   HTML/CSS layout core (vendored, CMake subproject)
cairo/                      cairo graphics source tree (vendored)
```

## Build

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./eclipse_demo        # writes out.png
```

Uses the system cairo (`pkg-config cairo`); the vendored `cairo/` tree is
for the future CodeOS cross-build.

## Status (skeleton)

Working:
- `eclipse_load_html` — parse + layout HTML/CSS via litehtml
- `eclipse_render` — draw into a host ARGB32 buffer via cairo
- `eclipse_get_width/height` — laid-out content size
- `eclipse_resize` — relayout on viewport change
- `eclipse_on_click` — anchor clicks reach the host callback
- Text drawn with cairo's toy font API (no pango/GTK dependency)

Not yet implemented:
- Image loading is callback-driven and currently returns nothing useful
- No text shaping (ASCII/Latin via toy fonts only)
- No scrolling/JS
- No cairo cross-build for the CodeOS kernel
- The kernel-side integration (`kernel/kernel/codeos_litehtml.*`) is still
  the old placeholder and must be replaced by this engine

## Next steps

1. Replace `codeos_litehtml` in the CodeOS kernel with `libeclipse`.
2. Wire `eclipse_render` into the kernel framebuffer / OpenWeb browser window.
3. Add a freetype/harfbuzz text shaper (replace toy-font path).
4. Add a JS engine alongside eclipse for a full rendering engine.