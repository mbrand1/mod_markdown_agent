# Deployment Notes

Deployment guidance and gotchas for `mod_markdown_agent`. See `README.md` for
build/install steps and `CLAUDE.md` for architecture.

## General

- Build on the target host (`./configure && make && sudo make install`) rather
  than copying a `.so` between servers — APR/Apache builds must match exactly.
- For non-www → www redirects, put `MarkdownAgent On` on the **www** vhost (the
  one returning `200`); the `301` has no body to convert.

## WordPress / WooCommerce sites

### Scope the filter away from stateful pages

Do **not** convert WooCommerce cart / checkout / my-account (or `/wp-admin`,
`/wp-login.php`). They are per-user/session, useless-to-harmful as Markdown for
agents, and waste conversion work on every hit:

```apache
# disable on WooCommerce stateful pages
<LocationMatch "^/(cart|checkout|my-account)(/|$)">
    MarkdownAgent Off
</LocationMatch>

# and on WordPress admin/login
<LocationMatch "^/(wp-admin|wp-login\.php)">
    MarkdownAgent Off
</LocationMatch>
```

Adjust paths to the site's actual permalinks.

### Guard page caches against Markdown leaking to browsers

The module sends `Vary: Accept`, but WordPress page-cache plugins and CDNs
**often ignore `Vary`**. If a full-page cache is enabled, an AI bot that primes
the `.md` variant can cause that cached Markdown to be served to a normal
browser (first-writer-wins). PHP OPcache is **not** a concern here — it caches
bytecode only and can never serve a `.md` body.

Before enabling any page cache (WP Super Cache, W3TC, LiteSpeed, WP Rocket, or a
"cache everything" CDN rule), configure it to either:

- **exclude** requests carrying `Accept: text/markdown` from caching, or
- **vary the cache key** on the `Accept` header.
