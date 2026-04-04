# mod_markdown_agent

A native Apache output filter module that converts HTML responses to Markdown
on-the-fly for AI agents using HTTP content negotiation.

When a client sends `Accept: text/markdown`, the module intercepts the HTML
response, converts it to Markdown, and returns it with
`Content-Type: text/markdown` and `Vary: Accept`. Normal browser requests
pass through completely untouched.

Inspired by [Cloudflare's Markdown for Agents](https://blog.cloudflare.com/markdown-for-agents/),
built for self-hosted Apache.

## Why?

Wrote an early version of this and decided to let Claude take a whack at it and
flesh it out completely so here you go...  It's something I'm using in-house that
sits in front of our own websites.  **But I still consider it in the alpha stage!**

## What's it for?

AI agents (Claude Code, OpenCode, Cursor, etc.) consume web content as part of
their workflow. Markdown is ~80-85% fewer tokens than equivalent HTML, meaning
lower cost and less context window consumption. The emerging standard is for
agents to request `text/markdown` via the `Accept` header and for servers to
respond accordingly.

## Dependencies

- Apache 2.4+ with `apxs`
- libxml2 (development headers)
- APR (bundled with Apache)

Pure C. No npm, no runtime, no interpreter.

## Build & Install

```bash
./configure
make
sudo make install
sudo apachectl graceful
```

`configure` auto-detects `apxs` and `libxml2`. If they're in
non-standard locations:

```bash
./configure --with-apxs=/usr/local/apache/bin/apxs \
            --with-libxml2=/usr/local/include/libxml2
```

To create a source tarball for distribution:

```bash
make dist    # produces mod_markdown_agent-VERSION.tar.gz
```

## Configuration

The module provides seven directives valid in `<Directory>`, `<Location>`,
and `<VirtualHost>` contexts:

| Directive | Default | Description |
|---|---|---|
| `MarkdownAgent` | Off | Master enable/disable |
| `MarkdownAgentExtract` | Off | Strip nav/header/footer/aside + boilerplate patterns |
| `MarkdownAgentLinks` | On | Include `[text](url)` links |
| `MarkdownAgentImages` | On | Include `![alt](src)` images |
| `MarkdownAgentTokenHeader` | Off | Emit `x-markdown-tokens` response header |
| `MarkdownAgentTitleH1` | Off | Emit HTML `<title>` as leading `# heading` |
| `MarkdownAgentDebug` | Off | Enable debug logging for filter operations |
| `MarkdownAgentContentUsage` | *(unset)* | Emit `Content-Usage` header ([IETF aipref](https://datatracker.ietf.org/wg/aipref/about/)) |

Example:

```apache
LoadModule markdown_agent_module modules/mod_markdown_agent.so

<Directory "/var/www/docs">
    MarkdownAgent            On
    MarkdownAgentExtract     On
    MarkdownAgentLinks       On
    MarkdownAgentImages      Off
    MarkdownAgentTokenHeader On
    MarkdownAgentContentUsage "bots=y, train-ai=y, ai-output=y, search=y"
</Directory>
```

See `example.conf` for a full annotated configuration including SSL VirtualHost.

## Testing

```bash
# Smoke test — should return Content-Type: text/markdown and Vary: Accept
make test URL=http://localhost/yourpage.html

# Or manually:
curl -s -H "Accept: text/markdown" http://localhost/yourpage.html

# Regression tests — compare live output against expected markdown
make regression BASE_URL=https://yourserver.com
```

## How It Works

The module registers an Apache output filter (`AP_FTYPE_RESOURCE`) that:

1. Checks if the client sent `Accept: text/markdown`
2. Accumulates the full HTML response body
3. Parses it with libxml2's `htmlReadMemory()` (handles malformed HTML)
4. Walks the DOM tree, converting elements to Markdown syntax
5. Rewrites response headers and sends the Markdown body

Because it's a filter (not a handler), it works with any content source —
static files, CGI, proxied backends, PHP, etc.

## Element Coverage

### Block Elements

| HTML | Markdown |
|---|---|
| `<h1>`..`<h6>` | `# ` .. `###### ` (ATX style) |
| `<p>` | Paragraph with surrounding blank lines |
| `<pre><code>` | Fenced code block with language detection |
| `<ul>` / `<ol>` / `<li>` | `- ` / `1. ` with nested indentation |
| `<hr>` | `---` |
| `<br>` | Two trailing spaces + newline |
| `<blockquote>` | `> ` prefixed lines (nested supported) |
| `<table>` | GFM pipe table with aligned columns |

### Inline Elements

| HTML | Markdown |
|---|---|
| `<strong>`, `<b>` | `**text**` |
| `<em>`, `<i>` | `*text*` |
| `<del>`, `<s>`, `<strike>` | `~~text~~` |
| `<code>` (inline) | `` `text` `` |
| `<a href="...">` | `[text](url)` |
| `<img>` | `![alt](src)` |
| `<sub>` | `~text~` |
| `<sup>` | `^text^` |

### Extract Mode

When `MarkdownAgentExtract On`, the module strips boilerplate before
converting: `<nav>`, `<header>`, `<footer>`, `<aside>`, and elements
whose class/id contains common patterns like `sidebar`, `cookie`,
`banner`, `advertisement`, `widget`, `popup`, etc.

`<script>`, `<style>`, `<noscript>`, and `<template>` are always dropped.

## License

MIT License. See [LICENSE](LICENSE) for details.
