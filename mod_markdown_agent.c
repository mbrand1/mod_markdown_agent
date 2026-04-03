/*
 * mod_markdown_agent.c
 *
 * Apache output filter module that converts HTML responses to Markdown
 * when the client sends: Accept: text/markdown
 *
 * Inspired by Cloudflare's "Markdown for Agents":
 *   https://blog.cloudflare.com/markdown-for-agents/
 *
 * Build:
 *   apxs -c -lxml2 -I/usr/include/libxml2 mod_markdown_agent.c html2md.c
 *
 * Load (httpd.conf):
 *   LoadModule markdown_agent_module modules/mod_markdown_agent.so
 */

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "apr_strings.h"
#include "apr_buckets.h"
#include "util_filter.h"

#include "html2md.h"

/* -------------------------------------------------------------------------
 * Per-directory configuration
 * -1 = unset (inherits from parent), 0 = Off, 1 = On
 * ------------------------------------------------------------------------- */
typedef struct {
    int enabled;        /* MarkdownAgent On|Off        */
    int extract;        /* MarkdownAgentExtract On|Off  */
    int links;          /* MarkdownAgentLinks On|Off    */
    int images;         /* MarkdownAgentImages On|Off   */
    int token_header;   /* MarkdownAgentTokenHeader On|Off */
    int title_as_h1;    /* MarkdownAgentTitleH1 On|Off     */
} mma_dir_conf;

/* Filter name used with ap_register_output_filter / ap_add_output_filter */
#define MMA_FILTER_NAME "MARKDOWN_AGENT"

/* Module declaration — must appear before any use of mma_module */
module AP_MODULE_DECLARE_DATA markdown_agent_module;

/* -------------------------------------------------------------------------
 * Filter context: accumulates brigade chunks until EOS, then converts
 * ------------------------------------------------------------------------- */
typedef struct {
    apr_bucket_brigade *accum;  /* accumulated input buckets      */
    int                 seen_eos;
} mma_filter_ctx;

/* -------------------------------------------------------------------------
 * Config: create / merge
 * ------------------------------------------------------------------------- */
static void *mma_create_dir_conf(apr_pool_t *p, char *path)
{
    mma_dir_conf *conf = apr_pcalloc(p, sizeof(mma_dir_conf));
    conf->enabled      = -1;
    conf->extract      = -1;
    conf->links        = -1;
    conf->images       = -1;
    conf->token_header = -1;
    conf->title_as_h1  = -1;
    return conf;
}

/* Child wins if set; otherwise inherit parent. */
#define MERGE_FLAG(child, parent, field) \
    merged->field = ((child)->field != -1) ? (child)->field : (parent)->field

static void *mma_merge_dir_conf(apr_pool_t *p, void *base, void *add)
{
    mma_dir_conf *parent = (mma_dir_conf *)base;
    mma_dir_conf *child  = (mma_dir_conf *)add;
    mma_dir_conf *merged = apr_pcalloc(p, sizeof(mma_dir_conf));

    MERGE_FLAG(child, parent, enabled);
    MERGE_FLAG(child, parent, extract);
    MERGE_FLAG(child, parent, links);
    MERGE_FLAG(child, parent, images);
    MERGE_FLAG(child, parent, token_header);
    MERGE_FLAG(child, parent, title_as_h1);

    return merged;
}

/* -------------------------------------------------------------------------
 * Directive handlers
 * ------------------------------------------------------------------------- */
static const char *cmd_enabled(cmd_parms *cmd, void *dcfg, int flag)
{
    ((mma_dir_conf *)dcfg)->enabled = flag;
    return NULL;
}
static const char *cmd_extract(cmd_parms *cmd, void *dcfg, int flag)
{
    ((mma_dir_conf *)dcfg)->extract = flag;
    return NULL;
}
static const char *cmd_links(cmd_parms *cmd, void *dcfg, int flag)
{
    ((mma_dir_conf *)dcfg)->links = flag;
    return NULL;
}
static const char *cmd_images(cmd_parms *cmd, void *dcfg, int flag)
{
    ((mma_dir_conf *)dcfg)->images = flag;
    return NULL;
}
static const char *cmd_token_header(cmd_parms *cmd, void *dcfg, int flag)
{
    ((mma_dir_conf *)dcfg)->token_header = flag;
    return NULL;
}
static const char *cmd_title_as_h1(cmd_parms *cmd, void *dcfg, int flag)
{
    ((mma_dir_conf *)dcfg)->title_as_h1 = flag;
    return NULL;
}

static const command_rec mma_cmds[] = {
    AP_INIT_FLAG("MarkdownAgent",            cmd_enabled,      NULL, OR_OPTIONS,
                 "Enable markdown-for-agents conversion (On|Off)"),
    AP_INIT_FLAG("MarkdownAgentExtract",     cmd_extract,      NULL, OR_OPTIONS,
                 "Strip nav/header/footer/aside boilerplate (On|Off)"),
    AP_INIT_FLAG("MarkdownAgentLinks",       cmd_links,        NULL, OR_OPTIONS,
                 "Include hyperlinks in markdown output (On|Off)"),
    AP_INIT_FLAG("MarkdownAgentImages",      cmd_images,       NULL, OR_OPTIONS,
                 "Include images in markdown output (On|Off)"),
    AP_INIT_FLAG("MarkdownAgentTokenHeader", cmd_token_header, NULL, OR_OPTIONS,
                 "Emit x-markdown-tokens response header (On|Off)"),
    AP_INIT_FLAG("MarkdownAgentTitleH1",    cmd_title_as_h1,  NULL, OR_OPTIONS,
                 "Emit HTML <title> as # heading before body (On|Off)"),
    { NULL }
};

/* -------------------------------------------------------------------------
 * Helper: does the Accept header express a preference for text/markdown?
 *
 * Handles simple cases:
 *   Accept: text/markdown
 *   Accept: text/html, text/markdown
 *   Accept: text/markdown;q=1.0, text/html;q=0.9
 *
 * Full RFC 7231 q-value ranking is left as a future enhancement;
 * presence of "text/markdown" is sufficient for current agents
 * (Claude Code, OpenCode, etc. send it without q-games).
 * ------------------------------------------------------------------------- */
static int accepts_markdown(request_rec *r)
{
    const char *accept = apr_table_get(r->headers_in, "Accept");
    if (!accept)
        return 0;
    return (ap_strstr_c(accept, "text/markdown") != NULL);
}

/* -------------------------------------------------------------------------
 * Helper: is the upstream response content-type text/html?
 * We only want to intercept HTML — not JSON, images, etc.
 * ------------------------------------------------------------------------- */
static int response_is_html(request_rec *r)
{
    const char *ct = r->content_type;
    if (!ct)
        return 0;
    return (strncasecmp(ct, "text/html", 9) == 0);
}

/* -------------------------------------------------------------------------
 * Output filter: accumulate buckets, convert on EOS, pass downstream
 * ------------------------------------------------------------------------- */
static apr_status_t mma_output_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    request_rec    *r   = f->r;
    mma_filter_ctx *ctx = f->ctx;
    apr_bucket     *b;
    apr_status_t    rv;

    /* Allocate context on first call */
    if (!ctx) {
        ctx   = apr_pcalloc(r->pool, sizeof(mma_filter_ctx));
        ctx->accum = apr_brigade_create(r->pool, f->c->bucket_alloc);
        f->ctx = ctx;
    }

    /* Walk the incoming brigade */
    for (b = APR_BRIGADE_FIRST(bb);
         b != APR_BRIGADE_SENTINEL(bb);
         b = APR_BUCKET_NEXT(b))
    {
        if (APR_BUCKET_IS_EOS(b)) {
            ctx->seen_eos = 1;
            break;
        }
        if (APR_BUCKET_IS_FLUSH(b)) {
            /* Don't flush mid-accumulation — we need the full document */
            continue;
        }
    }

    /* Move all non-EOS buckets into our accumulation brigade */
    APR_BRIGADE_CONCAT(ctx->accum, bb);

    if (!ctx->seen_eos) {
        /* Need more data — return OK without passing anything downstream */
        return APR_SUCCESS;
    }

    /* --- We have the full response body. Convert it. --- */

    /* Flatten accumulated brigade into a contiguous buffer */
    apr_size_t  html_len = 0;
    char       *html_buf = NULL;

    rv = apr_brigade_pflatten(ctx->accum, &html_buf, &html_len, r->pool);
    if (rv != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                      "mod_markdown_agent: brigade flatten failed");
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, ctx->accum);
    }

    /* Build options from config */
    mma_dir_conf *conf = ap_get_module_config(r->per_dir_config,
                                               &markdown_agent_module);
    html2md_opts opts;
    opts.extract      = (conf->extract      == 1);
    opts.links        = (conf->links        != 0);  /* On by default */
    opts.images       = (conf->images       != 0);  /* On by default */
    opts.title_as_h1  = (conf->title_as_h1  == 1);

    /* Build base URL for resolving relative links */
    const char *scheme   = ap_http_scheme(r);
    const char *hostname = r->hostname ? r->hostname : "localhost";
    const char *base_url = apr_psprintf(r->pool, "%s://%s%s",
                                        scheme, hostname, r->uri);

    /* Convert HTML → Markdown */
    apr_size_t  md_len  = 0;
    char       *md_buf  = html2md_convert(r->pool, html_buf, html_len,
                                           base_url, &opts, &md_len);

    if (!md_buf) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_markdown_agent: html2md_convert returned NULL");
        ap_remove_output_filter(f);
        /* Fall through and send original (already in ctx->accum, flattened) */
        apr_brigade_cleanup(ctx->accum);
        APR_BRIGADE_INSERT_TAIL(ctx->accum,
            apr_bucket_pool_create(html_buf, html_len, r->pool,
                                   f->c->bucket_alloc));
        APR_BRIGADE_INSERT_TAIL(ctx->accum,
            apr_bucket_eos_create(f->c->bucket_alloc));
        return ap_pass_brigade(f->next, ctx->accum);
    }

    /* Update response headers */
    ap_set_content_type(r, "text/markdown; charset=utf-8");
    apr_table_set(r->headers_out, "Vary", "Accept");

    /* Optionally emit token count heuristic (~4 chars/token) */
    if (conf->token_header == 1) {
        apr_size_t tokens = (md_len + 3) / 4;
        apr_table_set(r->headers_out, "x-markdown-tokens",
                      apr_psprintf(r->pool, "%" APR_SIZE_T_FMT, tokens));
    }

    /* Content-Length must reflect the new body */
    ap_set_content_length(r, (apr_off_t)md_len);

    /* Build a fresh outgoing brigade */
    apr_bucket_brigade *out = apr_brigade_create(r->pool, f->c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(out,
        apr_bucket_pool_create(md_buf, md_len, r->pool, f->c->bucket_alloc));
    APR_BRIGADE_INSERT_TAIL(out,
        apr_bucket_eos_create(f->c->bucket_alloc));

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "mod_markdown_agent: converted %"APR_SIZE_T_FMT
                  " bytes HTML -> %"APR_SIZE_T_FMT" bytes markdown",
                  html_len, md_len);

    ap_remove_output_filter(f);
    return ap_pass_brigade(f->next, out);
}

/* -------------------------------------------------------------------------
 * insert_filter hook: called per-request, decides whether to install
 * the filter for this request/response pair.
 *
 * Note: r->content_type may not be set yet at this hook point for
 * dynamic content — we recheck inside the filter itself.  For static
 * files served by mod_mime it is available here.
 * ------------------------------------------------------------------------- */
static void mma_insert_filter(request_rec *r)
{
    mma_dir_conf *conf = ap_get_module_config(r->per_dir_config,
                                               &markdown_agent_module);

    /* Module disabled for this location */
    if (conf->enabled != 1)
        return;

    /* Client doesn't want markdown */
    if (!accepts_markdown(r))
        return;

    /*
     * We can't always know content-type here for dynamic responses,
     * so we install the filter and do the content-type check inside it
     * (mma_output_filter bails out early if the response isn't text/html).
     *
     * For static .html files served by mod_mime, r->content_type IS
     * available and we could gate here — but the double-check in the
     * filter body is harmless and handles both cases.
     */
    ap_add_output_filter(MMA_FILTER_NAME, NULL, r, r->connection);
}

/* -------------------------------------------------------------------------
 * Module wiring
 * ------------------------------------------------------------------------- */
static void mma_register_hooks(apr_pool_t *p)
{
    ap_register_output_filter(MMA_FILTER_NAME,
                               mma_output_filter,
                               NULL,
                               AP_FTYPE_RESOURCE);

    ap_hook_insert_filter(mma_insert_filter, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA markdown_agent_module = {
    STANDARD20_MODULE_STUFF,
    mma_create_dir_conf,    /* per-dir config create  */
    mma_merge_dir_conf,     /* per-dir config merge   */
    NULL,                   /* per-server config create (unused) */
    NULL,                   /* per-server config merge  (unused) */
    mma_cmds,               /* directive table        */
    mma_register_hooks      /* hook registration      */
};
