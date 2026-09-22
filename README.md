# OpenWeb

OpenWeb is the web browser of CodeOS. This repository mirrors the current
in-tree implementation, which has three targets:

```
src/        Host C HTTP service (litehtml renderer → PNG). Builds with
            CMake/Make on any POSIX host; serves the UI in `static/`.
console/    CodeOS console renderer (C, `ow_html` layout engine). This is
            the userspace browser UI for the kernel console
            (`pkgs/core/openweb/src` in the CodeOS tree).
native/     Native Rust stack — HTTP backend + HTML renderer, compiled to a
            static lib (`libow_http.a`, crate `ow_http`) for the CodeOS
            kernel and Zircon (`kernel/kernel/rust_ow/` in the CodeOS tree).
```

## Host C service (`src/`)

A small C HTTP service that provides the browser shell in `static/`. It
serves the UI, creates Google or DuckDuckGo search URLs, and renders the
generated HTML through litehtml onto a Cairo PNG surface. The default render
surface is a 1024x768 CodeOS-style canvas with a centered document card.

Navigation mirrors the CodeOS kernel's `rust_ow` flow
(`kernel/kernel/rust_ow/` in the CodeOS tree):

- a bare address is classified as a URL when it has a known scheme
  (`http`, `https`, `file`, `data`), starts with `localhost`, or contains a
  dot; otherwise the query becomes a search;
- URL-shaped input without a scheme is prefixed with `http://` (never `https`);
- search queries go to `http://www.google.com/search?q=...` (or DuckDuckGo)
  with RFC 3986 percent-encoding;
- HTTP redirects (`3xx` with a `Location` header, absolute, scheme-relative,
  or path-relative) are followed with the same bounded loop of at most
  `OW_MAX_REDIRECTS` (8) that the kernel tab uses;
- failures surface as the same status strings the kernel shows: `IP needed:`,
  `No data:`, `HTTP error N`, `Too many redirects`, `Redirect without Location`.

Every fetched page is rendered to a PNG by litehtml from the HTML body; on
fetch failure the tile shows a styled error card with the same status.

Run it with:

```bash
make
make run
```

The first build downloads litehtml from GitHub and requires CMake, a C++17
compiler, Cairo, and Pango development packages.

Open `http://127.0.0.1:3000` in a browser on the host.

## Console renderer (`console/`)

The kernel console variant (`openweb.c`, `openweb.h`, `ow_html.c`,
`ow_html.h`). It renders pages as text lines on the CodeOS console using the
`ow_html` layout engine (bounded `OW_MAX_IMAGES`/`OW_MAX_LINKS` document
model, forms and link taps), with scroll and status handling. This is the
implementation packaged as `openweb-1.0.0.xora` in `pkgs/xora/` of the
CodeOS tree. It is compiled against the CodeOS userspace libc, not a POSIX
libc, so it builds inside the CodeOS userspace toolchain.

## Native Rust stack (`native/`)

The primary in-tree browser: an HTTP backend plus HTML renderer written in
Rust and compiled `no_std` for `x86_64-unknown-none` as a static library
(`ow_http`, `libow_http.a`). It provides the tab model used by the CodeOS
kernel and Zircon — `ow_navigate`, `ow_get_tabs`, per-tab URL/status/links,
image and form state — with the renderer in `src/ow_render.rs`.

Build (requires Rust with the `x86_64-unknown-none` target):

```bash
cd native
cargo build --release --target x86_64-unknown-none
# → target/x86_64-unknown-none/release/libow_http.a
```

The CodeOS kernel links this library (with `rust_ow` symbols referenced from
C shims) when building `zircond`.

`native/` carries the full `rust_ow` git history (merged with the `ours`
strategy, so the working tree is exactly the mirrored sources): the commits
`2d39549`, `3f39fa0`, and `74de3d1` are reachable from this repository, and
the CodeOS tree's `kernel/kernel/rust_ow` gitlink points at `74de3d1`.
Fetch this repo, then `git checkout 74de3d1` to reproduce that exact state.

## CodeOS-it status

The public CodeOS repositories currently provide a prebuilt kernel image and do
not define a user-space ABI, C library, filesystem API, or networking API. The
host C service is therefore written against standard POSIX sockets and file
calls so it can be ported when those interfaces are available, but it cannot be
linked into the bare CodeOS-it kernel from this repository alone.

For a native CodeOS-it build, the kernel needs to expose at least:

- a C entry point and process or application loading;
- memory allocation and filesystem reads for `static/`;
- TCP sockets (`socket`, `bind`, `listen`, `accept`, `recv`, and `send`);
- a way to launch or replace the optional external fetcher used by a full browser.

Until that ABI is published, build the C console renderer with the CodeOS
userspace toolchain and the host service with a POSIX-compatible C toolchain.

## License

GPL-3.0 (see LICENCE).