/*
 * html2md.c
 *
 * HTML → Markdown converter using libxml2's HTML parser.
 *
 * CURRENT STATE: Stage 1 stub.
 *   - Parses HTML correctly via htmlReadMemory()
 *   - Walks the DOM recursively
 *   - Emits plain text (strips all tags) so the filter pipeline
 *     can be verified end-to-end before the real element handlers
 *     are written.
 *
 * TODO (fill in one section at a time):
 *   [ ] Block elements:  h1-h6, p, blockquote, pre/code, ul/ol/li,
 *                        table, hr, br
 *   [ ] Drop elements:   script, style, noscript, template
 *   [ ] Inline elements: strong/b, em/i, del/s, code, a, img, sub, sup
 *   [ ] Extract mode:    nav, header, footer, aside + class/id patterns
 *   [ ] title_as_h1
 *
 * Build dependency: libxml2
 *   gcc ... -lxml2 -I/usr/include/libxml2
 */

#include <string.h>
#include <ctype.h>

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include "apr_pools.h"
#include "apr_strings.h"

#include "html2md.h"

/* =========================================================================
 * Internal output buffer
 *
 * A simple growable char buffer allocated entirely from the APR pool.
 * We never realloc — instead we keep a linked list of fixed-size chunks
 * and flatten at the end.  This avoids memcpy-heavy doubling inside a
 * recursive walker.
 * ========================================================================= */

#define CHUNK_SIZE (8 * 1024)   /* 8 KiB per chunk */

typedef struct buf_chunk {
    struct buf_chunk *next;
    apr_size_t        used;
    char              data[CHUNK_SIZE];
} buf_chunk;

typedef struct {
    apr_pool_t *pool;
    buf_chunk  *head;
    buf_chunk  *tail;
    apr_size_t  total;
} out_buf;

static void buf_init(out_buf *b, apr_pool_t *pool)
{
    b->pool  = pool;
    b->head  = apr_pcalloc(pool, sizeof(buf_chunk));
    b->tail  = b->head;
    b->total = 0;
}

static void buf_write(out_buf *b, const char *data, apr_size_t len)
{
    while (len > 0) {
        apr_size_t space = CHUNK_SIZE - b->tail->used;
        if (space == 0) {
            buf_chunk *c = apr_pcalloc(b->pool, sizeof(buf_chunk));
            b->tail->next = c;
            b->tail       = c;
            space         = CHUNK_SIZE;
        }
        apr_size_t take = (len < space) ? len : space;
        memcpy(b->tail->data + b->tail->used, data, take);
        b->tail->used += take;
        b->total      += take;
        data          += take;
        len           -= take;
    }
}

static void buf_puts(out_buf *b, const char *s)
{
    if (s) buf_write(b, s, strlen(s));
}

/* Flatten all chunks into a single NUL-terminated pool buffer */
static char *buf_flatten(out_buf *b)
{
    char      *result = apr_palloc(b->pool, b->total + 1);
    char      *p      = result;
    buf_chunk *c;

    for (c = b->head; c; c = c->next) {
        memcpy(p, c->data, c->used);
        p += c->used;
    }
    *p = '\0';
    return result;
}

/* =========================================================================
 * Extract-mode: should this node be dropped entirely?
 * ========================================================================= */

/* Tags that are always dropped (content-free or boilerplate) */
static const char *DROP_TAGS[] = {
    "script", "style", "noscript", "template",
    "head",   /* handled separately for title_as_h1 */
    NULL
};

/* Tags dropped in extract mode */
static const char *EXTRACT_DROP_TAGS[] = {
    "nav", "header", "footer", "aside",
    NULL
};

/* Class/id substrings that signal boilerplate in extract mode */
static const char *EXTRACT_DROP_PATTERNS[] = {
    "nav", "menu", "sidebar", "cookie", "banner", "advertisement",
    "advert", "social", "share-", "share_", "widget", "popup",
    NULL
};

static int tag_in_list(const char *name, const char **list)
{
    for (int i = 0; list[i]; i++)
        if (strcasecmp(name, list[i]) == 0)
            return 1;
    return 0;
}

static int has_boilerplate_attr(xmlNode *node)
{
    xmlChar *cls = xmlGetProp(node, BAD_CAST "class");
    xmlChar *id  = xmlGetProp(node, BAD_CAST "id");

    for (int i = 0; EXTRACT_DROP_PATTERNS[i]; i++) {
        if (cls && strcasestr((char *)cls, EXTRACT_DROP_PATTERNS[i])) {
            xmlFree(cls); xmlFree(id);
            return 1;
        }
        if (id  && strcasestr((char *)id,  EXTRACT_DROP_PATTERNS[i])) {
            xmlFree(cls); xmlFree(id);
            return 1;
        }
    }
    xmlFree(cls);
    xmlFree(id);
    return 0;
}

static int should_drop(xmlNode *node, const html2md_opts *opts)
{
    if (node->type != XML_ELEMENT_NODE)
        return 0;

    const char *name = (const char *)node->name;
    if (!name) return 0;

    if (tag_in_list(name, DROP_TAGS))
        return 1;

    if (opts && opts->extract) {
        if (tag_in_list(name, EXTRACT_DROP_TAGS))
            return 1;
        if (has_boilerplate_attr(node))
            return 1;
    }

    return 0;
}

/* =========================================================================
 * Walker context (carries options + output buffer through recursion)
 * ========================================================================= */

typedef struct {
    out_buf           *out;
    const html2md_opts *opts;
    int                list_depth;    /* nesting depth for ul/ol */
    int                ordered;       /* current list is ordered? */
    int                list_index;    /* counter for ordered lists */
    int                in_pre;        /* inside <pre> — preserve whitespace */
    int                last_was_nl;   /* avoid double blank lines */
    char               last_char;     /* last character written to output, 0 at start */
} walk_ctx;

/* =========================================================================
 * Forward declaration
 * ========================================================================= */
static void walk_node(walk_ctx *ctx, xmlNode *node);
static void walk_children(walk_ctx *ctx, xmlNode *node);
static void emit(walk_ctx *ctx, const char *s);

/* =========================================================================
 * Output helpers
 * ========================================================================= */

 /* Collapse 3+ consecutive newlines down to 2 (one blank line max) */
static char *normalize_newlines(apr_pool_t *pool, const char *s)
{
    apr_size_t len = strlen(s);
    char *out = apr_palloc(pool, len + 1);
    char *dst = out;
    int nl_count = 0;

    for (const char *src = s; *src; src++) {
        if (*src == '\n') {
            nl_count++;
            if (nl_count <= 2) *dst++ = '\n';
        } else {
            nl_count = 0;
            *dst++ = *src;
        }
    }
    *dst = '\0';
    return out;
}

/* Ensure output ends with at least one newline before emitting more */
static void ensure_newline(walk_ctx *ctx)
{
    if (!ctx->last_was_nl)
        emit(ctx, "\n");
}

/* Ensure output ends with a blank line (paragraph separator) */
static void ensure_blank_line(walk_ctx *ctx)
{
    ensure_newline(ctx);
    emit(ctx, "\n");
}

/* Write a string, tracking whether we ended on a newline */
static void emit(walk_ctx *ctx, const char *s)
{
    if (!s || !*s) return;
    buf_puts(ctx->out, s);
    apr_size_t len   = strlen(s);
    ctx->last_char   = s[len - 1];
    ctx->last_was_nl = (ctx->last_char == '\n');
}

/* Emit a space if needed before an inline delimiter — compensates for
 * emit_text() trimming trailing spaces before inline elements */
static void ensure_space_before_inline(walk_ctx *ctx)
{
    if (ctx->last_char != '\0' && !isspace((unsigned char)ctx->last_char))
        emit(ctx, " ");
}

/* =========================================================================
 * Text node emission
 *
 * In normal flow: collapse whitespace runs to a single space,
 * trim leading/trailing whitespace at block boundaries.
 * In <pre>: emit verbatim.
 * ========================================================================= */

static void emit_text(walk_ctx *ctx, const xmlChar *raw)
{
    if (!raw) return;
    const char *s = (const char *)raw;

    if (ctx->in_pre) {
        emit(ctx, s);
        return;
    }

    /* Collapse whitespace */
    char *buf = strdup(s);       /* temp — not pool alloc, freed below */
    char *dst = buf;

    /* Only trim leading whitespace if previous output ended with whitespace
     * (or we're at the very start). Otherwise preserve the leading space
     * so "word <em>text</em> more" doesn't become "word**text**more". */
    int  was_ws = (ctx->last_char == '\0' || ctx->last_char == '\n'
                || ctx->last_char == ' ');

    for (const char *src = s; *src; src++) {
        if (isspace((unsigned char)*src)) {
            if (!was_ws) *dst++ = ' ';
            was_ws = 1;
        } else {
            *dst++ = *src;
            was_ws = 0;
        }
    }
    *dst = '\0';
    /* Trim trailing space */
    while (dst > buf && dst[-1] == ' ')
        *--dst = '\0';

    if (*buf)
        emit(ctx, buf);

    free(buf);
}

/* =========================================================================
 * Block element handlers
 *
 * Each handler is responsible for:
 *   1. Ensuring correct leading blank line
 *   2. Emitting its markdown prefix
 *   3. Recursing into children
 *   4. Ensuring correct trailing blank line
 *
 * TODO: implement these.  The stubs below just recurse so the text
 * content comes through without any markdown formatting.
 * ========================================================================= */

static void handle_heading(walk_ctx *ctx, xmlNode *node, int level)
{
    /* emit "## " prefix (level hashes + space), walk children, newline */
    ensure_blank_line(ctx);
    for (int i = 0; i < level; i++)
        emit(ctx, "#");
    emit(ctx, " ");          /* now last_char = ' ', last_was_nl = 0 */

    /* emit the text */
    walk_children(ctx, node);
    ensure_blank_line(ctx);
}

static void handle_p(walk_ctx *ctx, xmlNode *node)
{
    ensure_blank_line(ctx);
    walk_children(ctx, node);
    ensure_blank_line(ctx);
}

static void handle_blockquote(walk_ctx *ctx, xmlNode *node)
{
    ensure_blank_line(ctx);

    /* Capture child output into a temporary buffer */
    out_buf  tmp;
    buf_init(&tmp, ctx->out->pool);

    out_buf *saved_out = ctx->out;

    ctx->out         = &tmp;
    ctx->last_was_nl = 1;
    ctx->last_char   = '\0';

    walk_children(ctx, node);

    /* Restore original buffer (but not last_char/last_was_nl —
     * those should reflect what we're about to emit) */
    ctx->out = saved_out;

    /* Flatten, normalize, and trim leading/trailing newlines */
    char *text = normalize_newlines(ctx->out->pool, buf_flatten(&tmp));
    while (*text == '\n') text++;
    char *end = text + strlen(text);
    while (end > text && end[-1] == '\n') end--;
    *end = '\0';

    /* Prefix each line with "> " */
    char *line = text;
    for (char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            emit(ctx, "> ");
            emit(ctx, line);
            emit(ctx, "\n");
            if (saved == '\0')
                break;
            line = p + 1;
        }
    }
}

static void handle_pre(walk_ctx *ctx, xmlNode *node)
{
    ensure_blank_line(ctx);

    /* Detect language from <pre><code class="language-xxx"> */
    const char *lang = NULL;
    xmlNode *code_child = NULL;

    for (xmlNode *c = node->children; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE
            && c->name && strcasecmp((const char *)c->name, "code") == 0) {
            code_child = c;
            break;
        }
    }

    if (code_child) {
        xmlChar *cls = xmlGetProp(code_child, BAD_CAST "class");
        if (cls) {
            /* Look for "language-" prefix in class list */
            const char *p = strcasestr((char *)cls, "language-");
            if (p) {
                p += 9;   /* skip "language-" */
                /* Extract until space or end of string */
                const char *end = p;
                while (*end && !isspace((unsigned char)*end))
                    end++;
                if (end > p)
                    lang = apr_pstrmemdup(ctx->out->pool, p, end - p);
            }
            xmlFree(cls);
        }
    }

    emit(ctx, "```");
    if (lang)
        emit(ctx, lang);
    emit(ctx, "\n");

    ctx->in_pre = 1;
    /* If we found a <code> child, walk its children directly
     * to avoid the inline code handler wrapping it in backticks */
    if (code_child)
        walk_children(ctx, code_child);
    else
        walk_children(ctx, node);
    ctx->in_pre = 0;

    ensure_newline(ctx);
    emit(ctx, "```\n");
}

static void handle_hr(walk_ctx *ctx)
{
    ensure_blank_line(ctx);
    emit(ctx, "---\n");
    ctx->last_was_nl = 1;
}

static void handle_br(walk_ctx *ctx)
{
    emit(ctx, "  \n");   /* two trailing spaces = hard line break in markdown */
}

static void handle_ul(walk_ctx *ctx, xmlNode *node)
{
    /* TODO: recurse with list_depth++, ordered=0 */
    ctx->list_depth++;
    walk_children(ctx, node);
    ctx->list_depth--;
    ensure_newline(ctx);
}

static void handle_ol(walk_ctx *ctx, xmlNode *node)
{
    /* TODO: recurse with list_depth++, ordered=1, reset list_index */
    int saved_ordered = ctx->ordered;
    int saved_index   = ctx->list_index;
    ctx->list_depth++;
    ctx->ordered    = 1;
    ctx->list_index = 0;
    walk_children(ctx, node);
    ctx->list_depth--;
    ctx->ordered    = saved_ordered;
    ctx->list_index = saved_index;
    ensure_newline(ctx);
}

static void handle_li(walk_ctx *ctx, xmlNode *node)
{
    /* TODO: emit "  " * (depth-1) + "- " or "N. " then walk children */
    ensure_newline(ctx);
    for (int i = 1; i < ctx->list_depth; i++)
        emit(ctx, "  ");
    if (ctx->ordered) {
        ctx->list_index++;
        emit(ctx, apr_psprintf(ctx->out->pool, "%d. ", ctx->list_index));
    } else {
        emit(ctx, "- ");
    }
    walk_children(ctx, node);
    ensure_newline(ctx);
}

/* ---- Table helpers ---- */

#define MAX_TABLE_COLS 64
#define MAX_TABLE_ROWS 512

/* Render a cell's children into a flat string via sub-buffer */
static char *render_cell(walk_ctx *ctx, xmlNode *cell)
{
    out_buf tmp;
    buf_init(&tmp, ctx->out->pool);

    out_buf *saved_out = ctx->out;
    int      saved_nl  = ctx->last_was_nl;
    char     saved_lc  = ctx->last_char;

    ctx->out         = &tmp;
    ctx->last_was_nl = 1;
    ctx->last_char   = '\0';

    walk_children(ctx, cell);

    ctx->out         = saved_out;
    ctx->last_was_nl = saved_nl;
    ctx->last_char   = saved_lc;

    /* Flatten and strip leading/trailing whitespace */
    char *text = buf_flatten(&tmp);
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';

    /* Replace any newlines with spaces (cell text must be single-line) */
    for (char *p = text; *p; p++)
        if (*p == '\n') *p = ' ';

    return text;
}

/* Collect <tr> elements from a container (table, thead, tbody, tfoot) */
static int collect_rows(xmlNode *parent, xmlNode **rows, int max_rows)
{
    int count = 0;
    for (xmlNode *child = parent->children; child && count < max_rows;
         child = child->next) {
        if (child->type != XML_ELEMENT_NODE || !child->name)
            continue;
        const char *tag = (const char *)child->name;
        if (strcasecmp(tag, "tr") == 0)
            rows[count++] = child;
        else if (strcasecmp(tag, "thead") == 0 ||
                 strcasecmp(tag, "tbody") == 0 ||
                 strcasecmp(tag, "tfoot") == 0)
            count += collect_rows(child, rows + count, max_rows - count);
    }
    return count;
}

static void handle_table(walk_ctx *ctx, xmlNode *node)
{
    ensure_blank_line(ctx);

    /* Collect all <tr> elements */
    xmlNode *rows[MAX_TABLE_ROWS];
    int nrows = collect_rows(node, rows, MAX_TABLE_ROWS);
    if (nrows == 0) {
        walk_children(ctx, node);
        ensure_blank_line(ctx);
        return;
    }

    /* First pass: render all cells, find column count and widths */
    int ncols = 0;
    char **cells[MAX_TABLE_ROWS];
    int    row_ncols[MAX_TABLE_ROWS];
    int    col_width[MAX_TABLE_COLS];

    memset(col_width, 0, sizeof(col_width));

    for (int r = 0; r < nrows; r++) {
        int c = 0;
        /* Count cells in this row */
        for (xmlNode *cell = rows[r]->children; cell && c < MAX_TABLE_COLS;
             cell = cell->next) {
            if (cell->type != XML_ELEMENT_NODE || !cell->name)
                continue;
            const char *tag = (const char *)cell->name;
            if (strcasecmp(tag, "th") == 0 || strcasecmp(tag, "td") == 0)
                c++;
        }
        row_ncols[r] = c;
        if (c > ncols) ncols = c;

        cells[r] = apr_palloc(ctx->out->pool, c * sizeof(char *));

        /* Render each cell */
        c = 0;
        for (xmlNode *cell = rows[r]->children; cell && c < MAX_TABLE_COLS;
             cell = cell->next) {
            if (cell->type != XML_ELEMENT_NODE || !cell->name)
                continue;
            const char *tag = (const char *)cell->name;
            if (strcasecmp(tag, "th") != 0 && strcasecmp(tag, "td") != 0)
                continue;

            cells[r][c] = render_cell(ctx, cell);
            int len = (int)strlen(cells[r][c]);
            if (len > col_width[c])
                col_width[c] = len;
            c++;
        }
    }

    /* Ensure minimum column width of 3 (for "---" separator) */
    for (int c = 0; c < ncols; c++)
        if (col_width[c] < 3) col_width[c] = 3;

    /* Emit header row (first row) */
    emit(ctx, "|");
    for (int c = 0; c < ncols; c++) {
        emit(ctx, " ");
        const char *text = (c < row_ncols[0]) ? cells[0][c] : "";
        emit(ctx, text);
        /* Pad to column width */
        int pad = col_width[c] - (int)strlen(text);
        for (int i = 0; i < pad; i++) emit(ctx, " ");
        emit(ctx, " |");
    }
    emit(ctx, "\n");

    /* Emit separator row */
    emit(ctx, "|");
    for (int c = 0; c < ncols; c++) {
        emit(ctx, " ");
        for (int i = 0; i < col_width[c]; i++) emit(ctx, "-");
        emit(ctx, " |");
    }
    emit(ctx, "\n");

    /* Emit data rows (starting from row 1) */
    for (int r = 1; r < nrows; r++) {
        emit(ctx, "|");
        for (int c = 0; c < ncols; c++) {
            emit(ctx, " ");
            const char *text = (c < row_ncols[r]) ? cells[r][c] : "";
            emit(ctx, text);
            int pad = col_width[c] - (int)strlen(text);
            for (int i = 0; i < pad; i++) emit(ctx, " ");
            emit(ctx, " |");
        }
        emit(ctx, "\n");
    }
}

/* =========================================================================
 * Inline element handlers
 * TODO: wrap child content with markdown delimiters
 * ========================================================================= */

static void handle_strong(walk_ctx *ctx, xmlNode *node)
{
    ensure_space_before_inline(ctx);
    emit(ctx, "**");
    walk_children(ctx, node);
    emit(ctx, "**");
}

static void handle_em(walk_ctx *ctx, xmlNode *node)
{
    ensure_space_before_inline(ctx);
    emit(ctx, "*");
    walk_children(ctx, node);
    emit(ctx, "*");
}

static void handle_del(walk_ctx *ctx, xmlNode *node)
{
    ensure_space_before_inline(ctx);
    emit(ctx, "~~");
    walk_children(ctx, node);
    emit(ctx, "~~");
}

static void handle_code_inline(walk_ctx *ctx, xmlNode *node)
{
    ensure_space_before_inline(ctx);
    emit(ctx, "`");
    ctx->in_pre = 1;   /* preserve whitespace inside inline code */
    walk_children(ctx, node);
    ctx->in_pre = 0;
    emit(ctx, "`");
}

static void handle_sub(walk_ctx *ctx, xmlNode *node)
{
    ensure_space_before_inline(ctx);
    emit(ctx, "~");
    walk_children(ctx, node);
    emit(ctx, "~");
}

static void handle_sup(walk_ctx *ctx, xmlNode *node)
{
    ensure_space_before_inline(ctx);
    emit(ctx, "^");
    walk_children(ctx, node);
    emit(ctx, "^");
}

static void handle_a(walk_ctx *ctx, xmlNode *node)
{
    if (!ctx->opts || !ctx->opts->links) {
        walk_children(ctx, node);   /* drop href, keep text */
        return;
    }
    xmlChar *href = xmlGetProp(node, BAD_CAST "href");
    if (!href) {
        walk_children(ctx, node);
        return;
    }
    ensure_space_before_inline(ctx);
    emit(ctx, "[");
    walk_children(ctx, node);
    emit(ctx, "](");
    emit(ctx, (char *)href);
    emit(ctx, ")");
    xmlFree(href);
}

static void handle_img(walk_ctx *ctx, xmlNode *node)
{
    if (!ctx->opts || !ctx->opts->images)
        return;   /* drop entirely */

    xmlChar *src = xmlGetProp(node, BAD_CAST "src");
    xmlChar *alt = xmlGetProp(node, BAD_CAST "alt");

    ensure_space_before_inline(ctx);
    emit(ctx, "![");
    if (alt) emit(ctx, (char *)alt);
    emit(ctx, "](");
    if (src) emit(ctx, (char *)src);
    emit(ctx, ")");

    xmlFree(src);
    xmlFree(alt);
}

/* =========================================================================
 * Main walker: dispatch on node type / tag name
 * ========================================================================= */

static void walk_node(walk_ctx *ctx, xmlNode *node)
{
    if (!node) return;

    /* Text node */
    if (node->type == XML_TEXT_NODE) {
        emit_text(ctx, node->content);
        return;
    }

    /* CDATA — treat as text */
    if (node->type == XML_CDATA_SECTION_NODE) {
        if (node->content)
            emit(ctx, (char *)node->content);
        return;
    }

    /* We only handle elements from here */
    if (node->type != XML_ELEMENT_NODE) {
        walk_children(ctx, node);
        return;
    }

    if (should_drop(node, ctx->opts))
        return;

    const char *tag = (const char *)node->name;
    if (!tag) return;

    /* --- Block elements --- */
    if      (strcasecmp(tag, "h1") == 0) handle_heading(ctx, node, 1);
    else if (strcasecmp(tag, "h2") == 0) handle_heading(ctx, node, 2);
    else if (strcasecmp(tag, "h3") == 0) handle_heading(ctx, node, 3);
    else if (strcasecmp(tag, "h4") == 0) handle_heading(ctx, node, 4);
    else if (strcasecmp(tag, "h5") == 0) handle_heading(ctx, node, 5);
    else if (strcasecmp(tag, "h6") == 0) handle_heading(ctx, node, 6);
    else if (strcasecmp(tag, "p")  == 0) handle_p(ctx, node);
    else if (strcasecmp(tag, "blockquote") == 0) handle_blockquote(ctx, node);
    else if (strcasecmp(tag, "pre") == 0) handle_pre(ctx, node);
    else if (strcasecmp(tag, "hr") == 0) handle_hr(ctx);
    else if (strcasecmp(tag, "br") == 0) handle_br(ctx);
    else if (strcasecmp(tag, "ul") == 0) handle_ul(ctx, node);
    else if (strcasecmp(tag, "ol") == 0) handle_ol(ctx, node);
    else if (strcasecmp(tag, "li") == 0) handle_li(ctx, node);
    else if (strcasecmp(tag, "table") == 0) handle_table(ctx, node);

    /* --- Inline elements --- */
    else if (strcasecmp(tag, "strong") == 0 ||
             strcasecmp(tag, "b")      == 0) handle_strong(ctx, node);
    else if (strcasecmp(tag, "em")     == 0 ||
             strcasecmp(tag, "i")      == 0) handle_em(ctx, node);
    else if (strcasecmp(tag, "del")    == 0 ||
             strcasecmp(tag, "s")      == 0 ||
             strcasecmp(tag, "strike") == 0) handle_del(ctx, node);
    else if (strcasecmp(tag, "code")   == 0 &&
             !ctx->in_pre)                   handle_code_inline(ctx, node);
    else if (strcasecmp(tag, "a")      == 0) handle_a(ctx, node);
    else if (strcasecmp(tag, "img")    == 0) handle_img(ctx, node);
    else if (strcasecmp(tag, "sub")    == 0) handle_sub(ctx, node);
    else if (strcasecmp(tag, "sup")    == 0) handle_sup(ctx, node);

    /* --- div, span, body, html, section, article, main, etc.
           Just recurse — they carry no markdown meaning themselves --- */
    else {
        walk_children(ctx, node);
    }
}

static void walk_children(walk_ctx *ctx, xmlNode *node)
{
    for (xmlNode *child = node->children; child; child = child->next)
        walk_node(ctx, child);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void html2md_init(void)
{
    xmlInitParser();
}

void html2md_cleanup(void)
{
    xmlCleanupParser();
}

char *html2md_convert(apr_pool_t         *pool,
                      const char         *html,
                      apr_size_t          html_len,
                      const char         *base_url,
                      const html2md_opts *opts,
                      apr_size_t         *out_len)
{
    if (!html || html_len == 0) {
        if (out_len) *out_len = 0;
        return apr_pstrdup(pool, "");
    }

    /* Parse — HTML_PARSE_RECOVER handles real-world malformed markup.
     * HTML_PARSE_NOERROR / NOWARNING suppress libxml2's stderr spam. */
    htmlDocPtr doc = htmlReadMemory(html, (int)html_len,
                                    base_url, /* URL (base) for relative links */
                                    NULL,     /* encoding — let libxml2 detect */
                                    HTML_PARSE_RECOVER  |
                                    HTML_PARSE_NOERROR  |
                                    HTML_PARSE_NOWARNING);
    if (!doc) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    xmlNode *root = xmlDocGetRootElement(doc);

    out_buf  buf;
    buf_init(&buf, pool);

    walk_ctx ctx = {
        .out         = &buf,
        .opts        = opts,
        .list_depth  = 0,
        .ordered     = 0,
        .list_index  = 0,
        .in_pre      = 0,
        .last_was_nl = 1,   /* treat start-of-output as "after newline" */
    };

    /* If title_as_h1 is set, find <title> in <head> and emit as # heading
     * before walking the body. We do this here because <head> is in
     * DROP_TAGS and will be skipped by the normal walk. */
    if (opts && opts->title_as_h1) {
        /* Search for <head> -> <title> */
        for (xmlNode *child = root->children; child; child = child->next) {
            if (child->type == XML_ELEMENT_NODE && child->name
                && strcasecmp((const char *)child->name, "head") == 0) {
                for (xmlNode *hc = child->children; hc; hc = hc->next) {
                    if (hc->type == XML_ELEMENT_NODE && hc->name
                        && strcasecmp((const char *)hc->name, "title") == 0) {
                        xmlChar *title = xmlNodeGetContent(hc);
                        if (title && *title) {
                            emit(&ctx, "# ");
                            emit(&ctx, (char *)title);
                            emit(&ctx, "\n\n");
                        }
                        xmlFree(title);
                        break;
                    }
                }
                break;
            }
        }
    }

    walk_node(&ctx, root);

    xmlFreeDoc(doc);

    char *result = normalize_newlines(pool, buf_flatten(&buf));
    if (out_len) *out_len = strlen(result);
    return result;
}
