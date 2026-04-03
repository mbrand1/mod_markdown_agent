# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

`mod_markdown_agent` is a native Apache C output filter that converts HTML responses to Markdown on-the-fly when the client sends `Accept: text/markdown`. Inspired by Cloudflare's "Markdown for Agents" but self-hosted as a pure C Apache module with only libxml2 + APR as dependencies.

## Build & Test

```bash
./configure                                                   # auto-detect apxs + libxml2
make                                                          # compile via apxs
sudo make install                                             # install .so + add LoadModule
/usr/local/apache/bin/apachectl configtest                    # verify config
sudo /usr/local/apache/bin/apachectl graceful                 # reload Apache
make test URL=https://scooby.netsville.com/testfile.html      # smoke test with curl
make regression                                               # run all regression tests
make dist                                                     # create source tarball
make clean                                                    # remove build artifacts
make distclean                                                # remove all generated files
```

Configure options: `--with-apxs=PATH`, `--with-libxml2=PATH`.

Build dependencies: libxml2 (`-lxml2`, headers at `/usr/include/libxml2`), APR (bundled with Apache).

## Regression Tests

Test HTML files and their expected markdown output live in `testfiles/`. Each `.html` file has a corresponding `.expected` file. `make regression` curls each test through the live server and diffs against expected output. Default `BASE_URL` is `https://scooby.netsville.com`; override with `make regression BASE_URL=http://...`.

Test files must also be deployed to the docroot (`/usr/local/apache/htdocs/`). After changing parser behavior, regenerate expected output for affected tests and verify by inspection before committing.

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
3. Checks for compressed input (Content-Encoding header + gzip magic bytes) and bails with a log message if detected
4. `apr_brigade_pflatten()` produces a flat HTML buffer
5. `html2md_convert()` parses with libxml2's `htmlReadMemory()` (with base URL for link resolution) and walks the DOM recursively
6. Response headers are rewritten (`Content-Type: text/markdown`, `Vary: Accept`)
7. New brigade with Markdown body is passed downstream

**Key files:**
- `mod_markdown_agent.c` -- Apache module wiring (config directives, filter lifecycle, hooks). Fully complete.
- `html2md.h` -- Public interface: `html2md_opts` struct and `html2md_convert()` signature.
- `html2md.c` -- DOM walker with chunked output buffer. All handlers complete.
- `example.conf` -- Annotated Apache config with all directives.
- `testfiles/` -- Regression test HTML inputs and expected markdown outputs.

## Config Directives

All are `FLAG` type (On/Off), valid in `<Directory>`, `<Location>`, `<VirtualHost>`. Per-dir merge: child wins if set, else inherits. Unset defaults to Off.

| Directive | Description |
|---|---|
| `MarkdownAgent` | Master enable/disable |
| `MarkdownAgentExtract` | Strip nav/header/footer/aside + boilerplate class/id patterns |
| `MarkdownAgentLinks` | Include `[text](url)` links (default On when module enabled) |
| `MarkdownAgentImages` | Include `![alt](src)` images (default On when module enabled) |
| `MarkdownAgentTokenHeader` | Emit `x-markdown-tokens` response header |
| `MarkdownAgentTitleH1` | Emit HTML `<title>` as leading `# heading` before body content |
| `MarkdownAgentContentUsage` | Emit `Content-Usage` header value (IETF aipref, string, e.g. `"bots=y, train-ai=y"`) |

## Design Notes

- Config flags use -1/0/1 (unset/off/on) with `MERGE_FLAG` macro for per-dir inheritance
- `links` and `images` default to On when module is enabled (checked as `!= 0` not `== 1`)
- `emit_text()` collapses whitespace in normal flow but preserves it when `in_pre` is set
- `emit_text()` uses `last_char` to decide whether to trim leading whitespace, preventing inline markup from eating spaces (e.g. "word *text* more")
- `ensure_space_before_inline()` compensates for `emit_text()` trailing-space trimming -- emits a space before inline delimiters when `last_char` is not whitespace
- `ensure_newline()` and `ensure_blank_line()` use `emit()` (not `buf_puts`) so `last_char` stays in sync
- `handle_pre()` peeks at first `<code>` child to detect language and walks that child's children directly to avoid backtick wrapping from `handle_code_inline()`
- `handle_blockquote()` uses a sub-buffer pattern: captures child output, normalizes newlines, trims, then prefixes each line with `> `. Handles nesting naturally.
- `handle_table()` two-pass: first pass renders all cells via sub-buffers and measures column widths, second pass emits aligned GFM pipe table
- `title_as_h1` extracts `<title>` before the main walk (since `<head>` is in `DROP_TAGS`) using `xmlNodeGetContent()`
- Base URL (`scheme://hostname/uri`) is passed to `htmlReadMemory()` for internal link resolution
- Compressed input detection checks `Content-Encoding` header and gzip magic bytes (`1f 8b`); logs and passes through original response if detected
- `normalize_newlines()` collapses 3+ consecutive newlines to 2 as a final pass
- The filter installs speculatively (content-type may not be set at `insert_filter` time for dynamic content) and bails inside `mma_output_filter` if response isn't `text/html`

## Future Enhancements

- Accept header q-value parsing (currently simple `strstr` presence check)
- Full URL rewriting for relative href/src in `handle_a()`/`handle_img()` (libxml2 does not rewrite attributes in the DOM)
- ETag/caching support for markdown responses
- WordPress production deployment considerations: exclude `/wp-admin` and `/wp-login.php`, handle caching plugin interactions
