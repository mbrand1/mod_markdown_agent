# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

`mod_markdown_agent` is a native Apache C output filter that converts HTML responses to Markdown on-the-fly when the client sends `Accept: text/markdown`. Inspired by Cloudflare's "Markdown for Agents" but self-hosted as a pure C Apache module with only libxml2 + APR as dependencies.

## Build & Test

```bash
make                                                          # compile via apxs
sudo make install                                             # install .so + add LoadModule
/usr/local/apache/bin/apachectl configtest                    # verify config
sudo /usr/local/apache/bin/apachectl graceful                 # reload Apache
make test URL=https://scooby.netsville.com/testfile.html      # smoke test with curl
make clean                                                    # remove build artifacts
```

Build dependencies: libxml2 (`-lxml2`, headers at `/usr/include/libxml2`), APR (bundled with Apache).

## Environment

- Custom-compiled Apache at `/usr/local/apache/` (not distro packages)
- `apxs` is at `/usr/local/apache/bin/apxs`
- `apachectl` is at `/usr/local/apache/bin/apachectl` (not the system one)
- Per-domain configs: `conf/websites/domain.com.conf` via `IncludeOptional`
- Logs: `/usr/local/apache/logs/domain-{access,error}_log`
- SSL certs: `/usr/local/apache/ssl/domain.{crt,key}`
- No `a2ensite`/`a2enmod` -- all manual config editing
- VirtualHosts use explicit server IP (e.g. `69.55.64.29:443`)
- Test docroot: `/usr/local/apache/htdocs/` (scooby.netsville.com)
- SSL VirtualHosts must include the hardening directives shown in `example.conf`

## Architecture

The module is an **output filter** (`AP_FTYPE_RESOURCE`), not a handler. It intercepts HTML responses from any origin (static files, CGI, proxy, PHP) and converts them to Markdown.

**Request flow:**
1. `mma_insert_filter` hook checks if `MarkdownAgent On` and `Accept: text/markdown` is present; if so, installs the `MARKDOWN_AGENT` output filter
2. `mma_output_filter` accumulates the full response body (bucket brigade) until EOS
3. `apr_brigade_pflatten()` produces a flat HTML buffer
4. `html2md_convert()` parses with libxml2's `htmlReadMemory()` and walks the DOM recursively
5. Response headers are rewritten (`Content-Type: text/markdown`, `Vary: Accept`)
6. New brigade with Markdown body is passed downstream

**Key files:**
- `mod_markdown_agent.c` -- Apache module wiring (config directives, filter lifecycle, hooks). Fully complete.
- `html2md.h` -- Public interface: `html2md_opts` struct and `html2md_convert()` signature.
- `html2md.c` -- DOM walker with chunked output buffer.
- `example.conf` -- Annotated Apache config with all directives.

## Config Directives

All are `FLAG` type (On/Off), valid in `<Directory>`, `<Location>`, `<VirtualHost>`. Per-dir merge: child wins if set, else inherits. Unset defaults to Off.

| Directive | Description |
|---|---|
| `MarkdownAgent` | Master enable/disable |
| `MarkdownAgentExtract` | Strip nav/header/footer/aside + boilerplate class/id patterns |
| `MarkdownAgentLinks` | Include `[text](url)` links (default On when module enabled) |
| `MarkdownAgentImages` | Include `![alt](src)` images (default On when module enabled) |
| `MarkdownAgentTokenHeader` | Emit `x-markdown-tokens` response header |

## Current Development Status

**Complete and working:**
- All Apache module wiring (`mod_markdown_agent.c`) -- config, filter, hooks
- Output buffer (chunked linked-list, 8KB chunks, APR pool allocated)
- `normalize_newlines()` -- collapses 3+ consecutive newlines to 2, applied as final pass
- `last_char` tracking in `walk_ctx` + `emit()` for whitespace-aware text emission
- `should_drop()` filtering (always-drop tags + extract-mode class/id patterns)
- All inline handlers: strong/b, em/i, del/s/strike, code (inline), a, img, sub, sup
- `handle_heading()` -- ATX-style `#` prefix using `emit()` for proper `last_char` tracking
- `handle_p()` -- blank line wrapping with `last_was_nl` reset
- `handle_hr()`, `handle_br()`
- `handle_li()` with depth/index tracking and prefix emission
- `handle_ul()`, `handle_ol()` with save/restore of list state
- `handle_pre()` -- fenced code blocks with language detection from `<code class="language-xxx">`

**Block handlers still stubbed (recurse correctly but lack markdown formatting):**
1. `handle_blockquote()` -- needs sub-buffer approach to prefix lines with `> `
2. `handle_table()` -- needs GFM pipe table output (most complex)

**Not yet implemented:**
- `title_as_h1` option (walk `<head>` for `<title>`)
- Accept header q-value parsing (currently simple `strstr`)
- Base URL for relative links (`htmlReadMemory()` base URL param is `NULL`)
- Regression test suite: a set of HTML test files in the docroot with known-good expected Markdown output, to run after any parser change

## Design Notes

- Config flags use -1/0/1 (unset/off/on) with `MERGE_FLAG` macro for per-dir inheritance
- `links` and `images` default to On when module is enabled (checked as `!= 0` not `== 1`)
- `emit_text()` collapses whitespace in normal flow but preserves it when `in_pre` is set
- `emit_text()` uses `last_char` to decide whether to trim leading whitespace, preventing inline markup from eating spaces (e.g. "word *text* more")
- `handle_pre()` peeks at first `<code>` child to detect language and walks that child's children directly to avoid backtick wrapping from `handle_code_inline()`
- `handle_blockquote()` should use a sub-buffer pattern: save current `out_buf`, walk children into fresh buffer, restore original, prefix each line with `> `
- `handle_table()` should collect cell text first to measure column widths before emitting
- The filter installs speculatively (content-type may not be set at `insert_filter` time for dynamic content) and bails inside `mma_output_filter` if response isn't `text/html`
