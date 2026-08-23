# OpenWeb

# Description
OpenWeb is a small C HTTP service that provides the browser shell in `static/`.
It serves the UI, creates Google or DuckDuckGo search URLs, and renders the
generated HTML through litehtml onto a Cairo PNG surface. The default render
surface is a 1024x768 CodeOS-style canvas with a centered document card.

Run it with:
```bash
make
make run
```

The first build downloads litehtml from GitHub and requires CMake, a C++17
compiler, Cairo, and Pango development packages.

Open `http://127.0.0.1:3000` in a browser on the host.

## CodeOS-it status

The public CodeOS repositories currently provide a prebuilt kernel image and do
not define a user-space ABI, C library, filesystem API, or networking API. The
server is therefore written against standard POSIX sockets and file calls so it
can be ported when those interfaces are available, but it cannot be linked into
the bare CodeOS-it kernel from this repository alone.

For a native CodeOS-it build, the kernel needs to expose at least:

- a C entry point and process or application loading;
- memory allocation and filesystem reads for `static/`;
- TCP sockets (`socket`, `bind`, `listen`, `accept`, `recv`, and `send`);
- a way to launch or replace the optional external fetcher used by a full browser.

Until that ABI is published, build this C version with a POSIX-compatible C
toolchain and treat it as the application layer awaiting CodeOS-it adapters.
