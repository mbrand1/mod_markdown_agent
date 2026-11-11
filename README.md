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

## Triggering by User-Agent

The module triggers on `Accept: text/markdown`. As of mid-2026, most AI
fetchers don't send that header — user-triggered fetchers like `Claude-User`,
`ChatGPT-User`, and `Perplexity-User` typically request with `Accept: */*`. Until
content negotiation is widely adopted, you can opt those agents in by injecting
the header for matching User-Agents, using stock `mod_setenvif` + `mod_headers`
(no module change required):

```apache
SetEnvIfNoCase User-Agent "(Claude-User|ChatGPT-User|Perplexity-User)" ai_fetcher
RequestHeader merge Accept "text/markdown" env=ai_fetcher
```

Use `merge`, not `append` — it adds `text/markdown` only if not already in the
`Accept` list, avoiding a duplicated value.

**Caching:** once User-Agent decides the format, the response varies by
User-Agent, but the module only sends `Vary: Accept`, and only on the Markdown
response. A shared cache keys on the client's *original* headers, so it could
store the Markdown served to `Claude-User` (sent `Accept: */*`) and hand it to
any other client sending `*/*`. Mark both variants, unconditionally — not with
`env=ai_fetcher`, or the HTML variant goes unmarked:

```apache
Header merge Vary Accept
Header merge Vary User-Agent
```

Use two separate `merge` lines; a single `"Accept, User-Agent"` value is
compared as one token and would duplicate `Accept`. Note that
`Vary: User-Agent` fragments shared caches heavily — if a CDN or page cache sits
in front of the site, consider bypassing it for these agents instead.

Scope this to **user-triggered fetchers** (an agent acting for a live human on a
specific URL). Do **not** match search/index crawlers such as `Googlebot`,
`bingbot`, `GPTBot`, or `ClaudeBot`: serving them a different format than
browsers is a cloaking risk. Same-URL, same-content format switching for an
on-demand fetcher is just standard HTTP content negotiation.

## Logging what was served as Markdown

The converted response carries `Content-Type: text/markdown`, so a custom log
format that records the response content-type tells you exactly which requests
were served as Markdown vs. HTML:

```apache
LogFormat "%h %l %u %t \"%r\" %>s %b \"%{Referer}i\" \"%{User-Agent}i\" \"%{Accept}i\" \"%{Content-Type}o\"" md_combined
CustomLog logs/access_log md_combined
```

The last two fields are the request's `Accept` and the response `Content-Type`:

```bash
# requests served as Markdown
grep '"text/markdown"$' logs/access_log

# requests that ASKED for Markdown but got HTML back (conversion bailed:
# compressed input, redirect, error page, etc.)
grep -E '"[^"]*text/markdown[^"]*" "text/html[^"]*"$' logs/access_log
```

`%{Accept}i` logs the header *after* any `RequestHeader` rewrite, so
User-Agent-injected requests show up as `*/*, text/markdown` — the second
pattern matches `text/markdown` anywhere in the field for that reason. Requests
where the response was some other non-HTML type (JSON, XML, images) are passed
through untouched and won't appear in either list.

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
