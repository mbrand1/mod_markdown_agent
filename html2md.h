/*
 * html2md.h
 *
 * HTML → Markdown converter interface.
 * Implemented in html2md.c using libxml2's HTML parser.
 *
 * The caller owns the returned buffer — it is allocated from the
 * provided APR pool and must not be freed manually.
 */

#ifndef HTML2MD_H
#define HTML2MD_H

#include "apr_pools.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Conversion options
 *
 * All flags default to 0 (off) — caller sets what it wants.
 * The Apache module maps its config directives directly to this struct.
 * ------------------------------------------------------------------------- */
typedef struct {
    /*
     * extract: strip boilerplate before converting.
     * Removes: <nav>, <header>, <footer>, <aside>, <script>, <style>,
     * <noscript>, <template>, plus elements whose class/id contains
     * common ad/cookie/sidebar patterns.
     */
    int extract;

    /*
     * links: include hyperlinks as [text](url).
     * When 0: href is dropped, link text is preserved.
     */
    int links;

    /*
     * images: include images as ![alt](src).
     * When 0: <img> elements are dropped entirely.
     */
    int images;

    /*
     * title_as_h1: if <title> is found in <head>, emit it as a leading
     * # Heading before the body content.
     * Default: 0 (off) — most agents don't need it.
     */
    int title_as_h1;
} html2md_opts;

/* -------------------------------------------------------------------------
 * html2md_convert
 *
 * Convert an HTML buffer to Markdown.
 *
 * @param pool      APR pool for all allocations (output buffer lives here)
 * @param html      Input HTML bytes (need not be NUL-terminated)
 * @param html_len  Length of html in bytes
 * @param base_url  Base URL for resolving relative href/src (may be NULL)
 * @param opts      Conversion options (may be NULL → all defaults/off)
 * @param out_len   Output: length of returned markdown buffer in bytes
 *
 * @return  NUL-terminated markdown string allocated from pool,
 *          or NULL on unrecoverable parse failure.
 *          *out_len is set to strlen(return value) on success.
 *
 * Thread safety: libxml2 is thread-safe when its global init has been
 * called once (we do this in html2md_init below).  Each call to
 * html2md_convert uses its own parser context — no shared mutable state.
 * ------------------------------------------------------------------------- */
char *html2md_convert(apr_pool_t   *pool,
                      const char   *html,
                      apr_size_t    html_len,
                      const char   *base_url,
                      const html2md_opts *opts,
                      apr_size_t   *out_len);

/*
 * html2md_init / html2md_cleanup
 *
 * Call html2md_init once at module load time (child_init hook).
 * Call html2md_cleanup once at module unload / process exit.
 * These wrap xmlInitParser() / xmlCleanupParser().
 *
 * Not strictly required for correct operation (libxml2 auto-inits)
 * but good practice in a multi-threaded server.
 */
void html2md_init(void);
void html2md_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* HTML2MD_H */
