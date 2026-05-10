/* 
   uno.c - A lightweight terminal editor
   Compile: gcc uno.c -o uno
                                         */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>

/* =========================================================================
 * Embedded terminal helpers 
 * ======================================================================= */

static int            _term_raw_active = 0;
static struct termios _term_saved_attrs;

#define TERM_MOUSE_ENABLE   "\033[?1000h\033[?1002h\033[?1006h"
#define TERM_MOUSE_DISABLE  "\033[?1006l\033[?1002l\033[?1000l"

static inline int term_get_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;

    const char *env = getenv("COLUMNS");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) return v;
    }
    return 80;
}

static inline int term_get_height(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return (int)ws.ws_row;

    const char *env = getenv("LINES");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) return v;
    }
    return 24;
}

static inline int term_enable_raw(void)
{
    if (_term_raw_active) return 0;
    if (!isatty(STDIN_FILENO)) { errno = ENOTTY; return -1; }
    if (tcgetattr(STDIN_FILENO, &_term_saved_attrs) < 0) return -1;

    struct termios raw = _term_saved_attrs;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= (tcflag_t)(CS8);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return -1;
    _term_raw_active = 1;
    return 0;
}

static inline int term_disable_raw(void)
{
    if (!_term_raw_active) return 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &_term_saved_attrs) < 0) return -1;
    _term_raw_active = 0;
    return 0;
}

static inline int term_set_bg(uint8_t r, uint8_t g, uint8_t b)
{
    return (fprintf(stdout, "\033[48;2;%d;%d;%dm",
                    (int)r, (int)g, (int)b) < 0) ? -1 : 0;
}

static inline int term_get_bg(int *r, int *g, int *b)
{
    if (!r || !g || !b) { errno = EINVAL; return -1; }

    if (fputs("\033]11;?\033\\", stdout) == EOF) return -1;
    fflush(stdout);

    char buf[64];
    int i = 0;
    while (i < (int)(sizeof(buf) - 1)) {
        int ch = getchar();
        if (ch == EOF) return -1;
        buf[i++] = (char)ch;
        if (i >= 2 && buf[i - 2] == '\033' && buf[i - 1] == '\\') break;
        if (buf[i - 1] == '\a') break;
    }
    buf[i] = '\0';

    unsigned int rv = 0, gv = 0, bv = 0;
    char *p = buf;
    while (*p && *p != 'r') p++;
    if (sscanf(p, "rgb:%4x/%4x/%4x", &rv, &gv, &bv) != 3) return -1;

    *r = (int)(rv >> 8);
    *g = (int)(gv >> 8);
    *b = (int)(bv >> 8);
    return 0;
}

static inline int term_mouse_enable(void)
{
    return (fputs(TERM_MOUSE_ENABLE, stdout) == EOF) ? -1 : 0;
}

static inline int term_mouse_disable(void)
{
    return (fputs(TERM_MOUSE_DISABLE, stdout) == EOF) ? -1 : 0;
}

int r = 0, g = 0, b = 0;

/* =========================================================================
 * File helpers
 * ======================================================================= */

static inline int file_size(FILE *fp)
{
    if (!fp) { errno = EINVAL; return -1; }
    long original = ftell(fp);
    if (original < 0) return -1;
    if (fseek(fp, 0, SEEK_END) < 0) return -1;
    long size = ftell(fp);
    if (size < 0) return -1;
    if (fseek(fp, original, SEEK_SET) < 0) return -1;
    return (int)size;
}

static inline int file_read(FILE *fp, char *buf)
{
    if (!fp || !buf) { errno = EINVAL; return -1; }
    int size = file_size(fp);
    if (size < 0) return -1;
    if (fseek(fp, 0, SEEK_SET) < 0) return -1;
    size_t n = fread(buf, 1, (size_t)size, fp);
    if (n != (size_t)size && ferror(fp)) return -1;
    buf[n] = '\0';
    return 0;
}

static inline int file_write(FILE *fp, const char *buf)
{
    if (!fp || !buf) { errno = EINVAL; return -1; }
    if (fseek(fp, 0, SEEK_SET) < 0) return -1;
    if (ftruncate(fileno(fp), 0) < 0) return -1;
    size_t len = strlen(buf);
    if (fwrite(buf, 1, len, fp) != len) return -1;
    fflush(fp);
    return 0;
}

/* =========================================================================
 * Double-buffered renderer
 * ======================================================================= */

typedef struct {
    char    ch;
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t bold;
} RCell;

static int    render_w     = 0;
static int    render_h     = 0;
static RCell *render_front = NULL;  /* what is on screen now  */
static RCell *render_back  = NULL;  /* what we want on screen */

/* Output accumulation buffer — everything goes here, then one write(). */
static char  *_rbuf     = NULL;
static size_t _rbuf_len = 0;
static size_t _rbuf_cap = 0;

static void _rb_reserve(size_t extra)
{
    while (_rbuf_len + extra + 1 >= _rbuf_cap) {
        _rbuf_cap = _rbuf_cap ? _rbuf_cap * 2 : 65536;
        _rbuf = realloc(_rbuf, _rbuf_cap);
    }
}

static void _rb_append(const char *s, size_t n)
{
    _rb_reserve(n);
    memcpy(_rbuf + _rbuf_len, s, n);
    _rbuf_len += n;
}

static void _rb_str(const char *s) { _rb_append(s, strlen(s)); }
static void _rb_ch(char c)         { _rb_reserve(1); _rbuf[_rbuf_len++] = c; }

static void _rb_move(int col, int row)  /* 1-based terminal coords */
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "\033[%d;%dH", row, col);
    _rb_append(tmp, (size_t)n);
}

static void _rb_fg(uint8_t fr, uint8_t fg, uint8_t fb)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "\033[38;2;%d;%d;%dm", fr, fg, fb);
    _rb_append(tmp, (size_t)n);
}

static void _rb_bg(uint8_t br, uint8_t bg, uint8_t bb)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "\033[48;2;%d;%d;%dm", br, bg, bb);
    _rb_append(tmp, (size_t)n);
}

static void _rb_bold(int bold)
{
    _rb_str(bold ? "\033[1m" : "\033[22m");
}

/* Allocate cell grids for the given terminal dimensions. Call once. */
static void render_init(int w, int h)
{
    render_w     = w;
    render_h     = h;
    size_t n     = (size_t)(w * h);
    render_front = calloc(n, sizeof(RCell));
    render_back  = calloc(n, sizeof(RCell));
}

/* Reallocate after a terminal resize; forces a full redraw next flip. */
static void render_resize(int w, int h)
{
    render_w = w;
    render_h = h;
    size_t n = (size_t)(w * h);
    free(render_front);
    free(render_back);
    render_front = calloc(n, sizeof(RCell));
    render_back  = calloc(n, sizeof(RCell));
}

/* Fill the back-buffer with blank cells before composing a new frame. */
static void render_clear_back(uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                               uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    RCell blank = { ' ', fg_r, fg_g, fg_b, bg_r, bg_g, bg_b, 0 };
    for (int i = 0; i < render_w * render_h; i++)
        render_back[i] = blank;
}

/* Write one cell into the back-buffer (col/row are 0-based). */
static void render_put_style(int col, int row, char ch,
                             uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                             uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                             int bold)
{
    if (col < 0 || col >= render_w || row < 0 || row >= render_h) return;
    RCell *c = &render_back[row * render_w + col];
    c->ch   = ch;
    c->fg_r = fg_r; c->fg_g = fg_g; c->fg_b = fg_b;
    c->bg_r = bg_r; c->bg_g = bg_g; c->bg_b = bg_b;
    c->bold = bold ? 1 : 0;
}

static void render_put(int col, int row, char ch,
                       uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                       uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    render_put_style(col, row, ch, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b, 0);
}

/*
 * render_flip — diff back vs front, emit only changed cells, then issue a
 * single write() to stdout.  Parks the (hidden) cursor at (cursor_col,
 * cursor_row) (0-based) for IME / accessibility.
 */
static void render_flip(int cursor_col, int cursor_row)
{
    _rbuf_len = 0;

    int cur_fg_r = -1, cur_fg_g = -1, cur_fg_b = -1;
    int cur_bg_r = -1, cur_bg_g = -1, cur_bg_b = -1;
    int cur_bold = -1;
    int last_col = -2, last_row = -2;

    for (int row = 0; row < render_h; row++) {
        for (int col = 0; col < render_w; col++) {
            RCell *bk = &render_back [row * render_w + col];
            RCell *fr = &render_front[row * render_w + col];

            /* Skip cells that are already correct on screen. */
            if (bk->ch    == fr->ch   &&
                bk->fg_r  == fr->fg_r && bk->fg_g == fr->fg_g && bk->fg_b == fr->fg_b &&
                bk->bg_r  == fr->bg_r && bk->bg_g == fr->bg_g && bk->bg_b == fr->bg_b &&
                bk->bold  == fr->bold)
                continue;

            /* Emit a cursor-move only when not continuing a run. */
            if (col != last_col + 1 || row != last_row)
                _rb_move(col + 1, row + 1);

            if (bk->fg_r != cur_fg_r || bk->fg_g != cur_fg_g || bk->fg_b != cur_fg_b) {
                _rb_fg(bk->fg_r, bk->fg_g, bk->fg_b);
                cur_fg_r = bk->fg_r; cur_fg_g = bk->fg_g; cur_fg_b = bk->fg_b;
            }
            if (bk->bg_r != cur_bg_r || bk->bg_g != cur_bg_g || bk->bg_b != cur_bg_b) {
                _rb_bg(bk->bg_r, bk->bg_g, bk->bg_b);
                cur_bg_r = bk->bg_r; cur_bg_g = bk->bg_g; cur_bg_b = bk->bg_b;
            }
            if (bk->bold != cur_bold) {
                _rb_bold(bk->bold);
                cur_bold = bk->bold;
            }

            _rb_ch(bk->ch);
            *fr = *bk;

            last_col = col;
            last_row = row;
        }
    }

    _rb_str("\033[0m");
    _rb_move(cursor_col + 1, cursor_row + 1);

    if (_rbuf_len > 0)
        (void)write(STDOUT_FILENO, _rbuf, _rbuf_len);
}

/* Force a full redraw on the next flip (e.g. after SIGWINCH). */
static void render_invalidate(void)
{
    memset(render_front, 0xff, (size_t)(render_w * render_h) * sizeof(RCell));
}

static void render_free(void)
{
    free(render_front); render_front = NULL;
    free(render_back);  render_back  = NULL;
    free(_rbuf);        _rbuf = NULL;
    _rbuf_len = _rbuf_cap = 0;
}

/* =========================================================================
 * Text buffer helpers
 * ======================================================================= */

static int ensure_cap(char **s, int *sc, size_t extra)
{
    if (!s || !*s || !sc) { errno = EINVAL; return -1; }
    while (strlen(*s) + extra + 1 >= (size_t)(*sc)) {
        *sc *= 2;
        char *tmp = realloc(*s, (size_t)(*sc));
        if (!tmp) { perror("realloc"); return -1; }
        *s = tmp;
    }
    return 0;
}

static int count_lines(const char *s)
{
    int lines = 1;
    for (size_t i = 0; s && s[i] != '\0'; i++)
        if (s[i] == '\n') lines++;
    return lines;
}

static void index_to_line_col(const char *s, size_t idx, int *line, int *col)
{
    int l = 0, c = 0;
    for (size_t i = 0; s && s[i] != '\0' && i < idx; i++) {
        if (s[i] == '\n') { l++; c = 0; }
        else c++;
    }
    if (line) *line = l;
    if (col)  *col  = c;
}

static size_t line_col_to_index(const char *s, int target_line, int target_col)
{
    if (!s) return 0;
    if (target_line < 0) target_line = 0;
    if (target_col  < 0) target_col  = 0;
    int line = 0, col = 0;
    size_t i = 0;
    while (s[i] != '\0') {
        if (line == target_line) break;
        if (s[i] == '\n') line++;
        i++;
    }
    if (line < target_line) return strlen(s);
    while (s[i] != '\0' && s[i] != '\n' && col < target_col) { i++; col++; }
    return i;
}

static void ensure_cursor_visible(const char *s, size_t cursor, int *view_top, int rows)
{
    int total   = count_lines(s);
    int max_top = total - rows;
    if (max_top < 0) max_top = 0;

    int line = 0, col = 0;
    index_to_line_col(s, cursor, &line, &col);
    (void)col;

    if (*view_top > max_top) *view_top = max_top;
    if (*view_top < 0)       *view_top = 0;
    if (line < *view_top)               *view_top = line;
    if (line >= *view_top + rows)       *view_top = line - rows + 1;
    if (*view_top > max_top) *view_top = max_top;
    if (*view_top < 0)       *view_top = 0;
}

static int insert_at_cursor(char **s, int *sc, size_t *cursor, char ch)
{
    if (ensure_cap(s, sc, 1) < 0) return -1;
    size_t len = strlen(*s);
    memmove((*s) + *cursor + 1, (*s) + *cursor, len - *cursor + 1);
    (*s)[*cursor] = ch;
    (*cursor)++;
    return 0;
}

static int count_digits(int n)
{
    if (n == 0) return 1;
    int count = 0;
    if (n < 0) n = -n;
    while (n != 0) { n /= 10; count++; }
    return count;
}

/*
 * read_escape_seq — read bytes after the ESC that was already consumed.
 *
 * Terminal escape sequences end with either:
 *   • an alphabetic byte  (e.g. ESC [ A  for cursor-up), or
 *   • a tilde '~'         (e.g. ESC [ 2 0 0 ~  for bracketed-paste start).
 *
 * Returns the number of bytes stored in seq (NUL-terminated), or -1 if the
 * buffer overflowed (seq is still NUL-terminated at cap-1).
 */
static int read_escape_seq(char *seq, int cap)
{
    int n = 0;
    if (!seq || cap < 2) return -1;
    while (n < cap - 1) {
        int ch = getc(stdin);
        if (ch == EOF || ch < 0) { seq[n] = '\0'; return -1; }
        seq[n++] = (char)ch;
        if (isalpha((unsigned char)ch) || ch == '~') {
            seq[n] = '\0';
            return n;
        }
    }
    /* Buffer full — drain until a terminator so nothing leaks into text. */
    for (;;) {
        int ch = getc(stdin);
        if (ch == EOF || ch < 0) break;
        if (isalpha((unsigned char)ch) || ch == '~') break;
    }
    seq[cap - 1] = '\0';
    return -1;
}

/*
 * read_bracketed_paste — called after the opening ESC[200~ has been
 * recognised.  Reads raw bytes until the closing ESC[201~ marker and
 * bulk-inserts everything into the text buffer at the cursor position.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int read_bracketed_paste(char **s, int *sc, size_t *cursor)
{
    /* We accumulate the paste payload here before inserting, so that a
       single ensure_cap call covers the whole block. */
    int    pcap = 4096;
    int    plen = 0;
    char  *pbuf = malloc((size_t)pcap);
    if (!pbuf) { perror("malloc"); return -1; }

    /* State machine: watch for ESC [ 2 0 1 ~ */
    enum { S_NORMAL, S_ESC, S_LBRACKET, S_2, S_20, S_201 } state = S_NORMAL;

    for (;;) {
        int ch = getc(stdin);
        if (ch == EOF || ch < 0) break;

        switch (state) {
        case S_NORMAL:
            if (ch == '\x1b') { state = S_ESC; break; }
            goto store;
        case S_ESC:
            if (ch == '[')  { state = S_LBRACKET; break; }
            /* False alarm — store the ESC we held back, then this byte. */
            if (plen + 1 >= pcap) { pcap *= 2; pbuf = realloc(pbuf, (size_t)pcap); }
            pbuf[plen++] = '\x1b';
            state = S_NORMAL;
            if (ch == '\x1b') { state = S_ESC; break; }
            goto store;
        case S_LBRACKET:
            if (ch == '2')  { state = S_2;        break; }
            /* False alarm — store ESC [ then this byte. */
            if (plen + 2 >= pcap) { pcap *= 2; pbuf = realloc(pbuf, (size_t)pcap); }
            pbuf[plen++] = '\x1b';
            pbuf[plen++] = '[';
            state = S_NORMAL;
            if (ch == '\x1b') { state = S_ESC; break; }
            goto store;
        case S_2:
            if (ch == '0')  { state = S_20;       break; }
            if (plen + 3 >= pcap) { pcap *= 2; pbuf = realloc(pbuf, (size_t)pcap); }
            pbuf[plen++] = '\x1b'; pbuf[plen++] = '['; pbuf[plen++] = '2';
            state = S_NORMAL;
            if (ch == '\x1b') { state = S_ESC; break; }
            goto store;
        case S_20:
            if (ch == '1')  { state = S_201;      break; }
            if (plen + 4 >= pcap) { pcap *= 2; pbuf = realloc(pbuf, (size_t)pcap); }
            pbuf[plen++] = '\x1b'; pbuf[plen++] = '[';
            pbuf[plen++] = '2';    pbuf[plen++] = '0';
            state = S_NORMAL;
            if (ch == '\x1b') { state = S_ESC; break; }
            goto store;
        case S_201:
            if (ch == '~')  goto done;   /* closing marker consumed */
            if (plen + 5 >= pcap) { pcap *= 2; pbuf = realloc(pbuf, (size_t)pcap); }
            pbuf[plen++] = '\x1b'; pbuf[plen++] = '[';
            pbuf[plen++] = '2';    pbuf[plen++] = '0'; pbuf[plen++] = '1';
            state = S_NORMAL;
            if (ch == '\x1b') { state = S_ESC; break; }
            goto store;
        }
        continue;
    store:
        if (plen + 1 >= pcap) { pcap *= 2; pbuf = realloc(pbuf, (size_t)pcap); }
        pbuf[plen++] = (char)ch;
    }
done:;
    /* Bulk-insert the collected paste payload. */
    if (plen > 0) {
        if (ensure_cap(s, sc, (size_t)plen) < 0) { free(pbuf); return -1; }
        size_t len = strlen(*s);
        memmove((*s) + *cursor + plen, (*s) + *cursor, len - *cursor + 1);
        memcpy((*s) + *cursor, pbuf, (size_t)plen);
        *cursor += (size_t)plen;
    }
    free(pbuf);
    return 0;
}

/* =========================================================================
 * Colour palette macros
 * ======================================================================= */

#define C_GUTTER_FG  229, 229, 229
#define C_GUTTER_BG   64,  64,  64
#define C_TEXT_FG    229, 229, 229
#define C_TEXT_BG    100, 100, 100
#define C_BAR_FG       0,   0,   0
#define C_BAR_BG     220, 220, 220
#define C_CURSOR_FG    0,   0,   0
#define C_CURSOR_BG  255, 255, 255
#define C_SYNTAX_STRING    130, 255, 130   /* high-contrast green      */
#define C_SYNTAX_FUNCTION  255, 235, 120   /* high-contrast yellow     */
#define C_SYNTAX_DATATYPE   80, 130, 230   /* brighter dark-blue tone  */
#define C_SYNTAX_COMMENT   205, 205, 205   /* high-contrast gray       */
#define C_SYNTAX_OPERATOR  150, 225, 255   /* high-contrast light blue */
#define C_SYNTAX_NUMBER    255, 214, 102

static int is_ident_start(unsigned char ch)
{
    return isalpha(ch) || ch == '_';
}

static int is_ident_char(unsigned char ch)
{
    return isalnum(ch) || ch == '_';
}

typedef enum {
    LANG_GENERIC = 0,
    LANG_PYTHON,
    LANG_SHELL,
    LANG_SQL,
    LANG_HTML,
    LANG_CSS,
    LANG_MARKDOWN
} SyntaxLang;

static int is_css_ident_char(unsigned char ch)
{
    return isalnum(ch) || ch == '_' || ch == '-';
}

static int is_operator_char(unsigned char ch)
{
    return strchr("+-*/%=!<>|&^~?:.,;()[]{}", ch) != NULL;
}

static int is_word_in_list(const char *s, size_t len,
                           const char *const *words, size_t word_count)
{
    for (size_t i = 0; i < word_count; i++) {
        size_t wlen = strlen(words[i]);
        if (wlen == len && strncmp(s, words[i], len) == 0) return 1;
    }
    return 0;
}

static int token_is_keyword(const char *s, size_t len)
{
    static const char *kw[] = {
        "if", "else", "for", "while", "do", "switch", "case", "default",
        "break", "continue", "return", "try", "catch", "finally", "throw",
        "throws", "class", "interface", "enum", "struct", "union", "typedef",
        "namespace", "using", "public", "private", "protected", "static",
        "const", "let", "var", "new", "delete", "import", "export", "from",
        "package", "module", "in", "of", "with", "as", "await", "async",
        "yield", "lambda", "fn", "func", "function", "def", "match", "where",
        "impl", "extends", "implements", "override", "virtual", "final"
    };
    return is_word_in_list(s, len, kw, sizeof(kw) / sizeof(kw[0]));
}

static int token_is_datatype(const char *s, size_t len, SyntaxLang lang)
{
    static const char *common_types[] = {
        "bool", "boolean", "byte", "char", "double", "float", "int", "long",
        "short", "void", "signed", "unsigned", "size_t", "ssize_t", "string",
        "str", "number", "object", "array", "list", "dict", "map", "set",
        "tuple", "null", "NULL", "true", "false", "any", "unknown", "never",
        "auto", "var", "let", "const", "class", "interface", "enum", "struct"
    };
    static const char *html_types[] = {
        "html", "head", "body", "title", "meta", "link", "script", "style",
        "div", "span", "p", "a", "img", "ul", "ol", "li", "table", "tr", "td",
        "th", "thead", "tbody", "footer", "header", "main", "section", "nav",
        "article", "aside", "form", "input", "button", "label", "select",
        "option", "textarea", "canvas", "svg", "path", "h1", "h2", "h3", "h4",
        "h5", "h6", "br", "hr", "pre", "code", "blockquote", "doctype"
    };
    static const char *css_types[] = {
        "color", "background", "display", "position", "margin", "padding",
        "border", "font", "width", "height", "min-width", "max-width",
        "min-height", "max-height", "grid", "flex", "align-items",
        "justify-content", "transform", "transition", "animation"
    };

    if (is_word_in_list(s, len, common_types,
                        sizeof(common_types) / sizeof(common_types[0])))
        return 1;
    if (lang == LANG_HTML &&
        is_word_in_list(s, len, html_types, sizeof(html_types) / sizeof(html_types[0])))
        return 1;
    if (lang == LANG_CSS &&
        is_word_in_list(s, len, css_types, sizeof(css_types) / sizeof(css_types[0])))
        return 1;
    return 0;
}

static int token_is_function(const char *s, size_t end)
{
    size_t j = end;
    while (s[j] == ' ' || s[j] == '\t') j++;
    return s[j] == '(';
}

static int ext_eq(const char *ext, const char *value)
{
    while (*ext && *value) {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*value)) return 0;
        ext++;
        value++;
    }
    return *ext == '\0' && *value == '\0';
}

static SyntaxLang detect_language(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || dot[1] == '\0') return LANG_GENERIC;
    const char *ext = dot + 1;

    if (ext_eq(ext, "md") || ext_eq(ext, "markdown") || ext_eq(ext, "mdx"))
        return LANG_MARKDOWN;
    if (ext_eq(ext, "html") || ext_eq(ext, "htm") || ext_eq(ext, "xhtml") ||
        ext_eq(ext, "xml") || ext_eq(ext, "svg"))
        return LANG_HTML;
    if (ext_eq(ext, "css") || ext_eq(ext, "scss") || ext_eq(ext, "less"))
        return LANG_CSS;
    if (ext_eq(ext, "py") || ext_eq(ext, "pyw"))
        return LANG_PYTHON;
    if (ext_eq(ext, "sh") || ext_eq(ext, "bash") || ext_eq(ext, "zsh") || ext_eq(ext, "fish"))
        return LANG_SHELL;
    if (ext_eq(ext, "sql"))
        return LANG_SQL;

    return LANG_GENERIC;
}

static size_t number_token_len(const char *s, size_t idx)
{
    size_t j = idx;

    if (s[j] == '0' && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
        j += 2;
        while (isxdigit((unsigned char)s[j]) || s[j] == '\'') j++;
        while (strchr("uUlL", s[j])) j++;
        return j - idx;
    }

    if (s[j] == '0' && (s[j + 1] == 'b' || s[j + 1] == 'B')) {
        j += 2;
        while (s[j] == '0' || s[j] == '1' || s[j] == '\'') j++;
        while (strchr("uUlL", s[j])) j++;
        return j - idx;
    }

    int seen_dot = 0, seen_exp = 0;
    while (s[j] != '\0') {
        unsigned char ch = (unsigned char)s[j];
        if (isdigit(ch) || ch == '\'') {
            j++;
            continue;
        }
        if (ch == '.' && !seen_dot && !seen_exp) {
            seen_dot = 1;
            j++;
            continue;
        }
        if ((ch == 'e' || ch == 'E' || ch == 'p' || ch == 'P') && !seen_exp) {
            seen_exp = 1;
            j++;
            if (s[j] == '+' || s[j] == '-') j++;
            continue;
        }
        break;
    }

    while (strchr("uUlLfF", s[j])) j++;
    return j - idx;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static int text_rows_for_height(int term_h, int prompt_active)
{
    int reserved_bottom = prompt_active ? 2 : 1;
    int rows = term_h - 1 - reserved_bottom; /* -heading -bottom bars */
    if (rows < 1) rows = 1;
    return rows;
}

static const char *newline_style(const char *s)
{
    return strstr(s, "\r\n") ? "DOS" : "UNIX";
}

/* =========================================================================
 * Signal / cleanup
 * ======================================================================= */

static volatile int running = 1;
static volatile int resized = 0;

static void handle_sigint(int sig)   { (void)sig; running = 0; }
static void handle_sigwinch(int sig) { (void)sig; resized = 1; }

static void cleanup(void)
{
    render_free();
    term_mouse_disable();
    term_disable_raw();
    term_set_bg(r, g, b);
    write(STDOUT_FILENO, "\033[?2004l"    /* bracketed paste off */
                         "\033[?25h"     /* restore cursor      */
                         "\033[2J\033[H", /* clear screen        */
                         21);
}

/* =========================================================================
 * Frame composition — fills the render back-buffer for one frame.
 *
 * Returns (via out-params) the 0-based cursor cell position for render_flip,
 * and the gutter width for mouse-click hit testing.
 * ======================================================================= */

static void draw_frame(const char *s, size_t cursor, int view_top,
                        const char *heading, SyntaxLang lang,
                        const char *status_left, const char *status_right,
                        const char *prompt_line, int prompt_active,
                        int *out_cursor_col, int *out_cursor_row,
                        int *out_gutter)
{
    int w = render_w;
    int h = render_h;

    /* --- Status bar (screen row 0) ------------------------------------- */
    int hlen = (int)strlen(heading);
    int hx   = (w / 2) - hlen / 2;
    if (hx < 0) hx = 0;
    for (int col = 0; col < w; col++) {
        char ch = (col >= hx && col < hx + hlen) ? heading[col - hx] : ' ';
        render_put(col, 0, ch, C_BAR_FG, C_BAR_BG);
    }

    /* --- Text area (screen rows 1 .. h-1) ------------------------------ */
    int total_lines = count_lines(s);
    int rows        = text_rows_for_height(h, prompt_active);
    int gutter_w    = count_digits(total_lines) + 1;
    int status_row  = prompt_active ? (h - 2) : (h - 1);
    int prompt_row  = h - 1;
    if (out_gutter) *out_gutter = gutter_w;

    /* Seek to the first byte of view_top. */
    size_t i    = 0;
    int    line = 0;
    while (s[i] != '\0' && line < view_top) {
        if (s[i] == '\n') line++;
        i++;
    }

    int in_block_comment = 0;
    int block_comment_is_html = 0;
    int in_md_fence = 0;

    for (int rline = 0; rline < rows; rline++) {
        int lnum       = view_top + rline;
        int screen_row = rline + 1;

        /* Gutter: right-aligned line number. */
        {
            char gstr[32];
            if (lnum < total_lines)
                snprintf(gstr, sizeof(gstr), "%d", lnum + 1);
            else
                gstr[0] = '\0';
            int glen = (int)strlen(gstr);
            int pad  = gutter_w - glen;
            for (int col = 0; col < gutter_w; col++) {
                char ch = (col < pad) ? ' ' : gstr[col - pad];
                render_put(col, screen_row, ch, C_GUTTER_FG, C_GUTTER_BG);
            }
        }

        /* Text: one character per cell, then pad to edge. */
        if (lnum < total_lines) {
            int    col               = gutter_w;
            size_t line_start        = i;
            int    in_line_comment   = 0;
            int    in_string         = 0;
            int    in_md_inline_code = 0;
            int    escaped           = 0;
            char   string_delim      = '\0';
            size_t probe             = i;
            int    preprocessor_line = 0;
            int    md_heading_line   = 0;
            int    md_fence_line     = 0;

            while (s[probe] == ' ' || s[probe] == '\t') probe++;
            if (lang == LANG_GENERIC && s[probe] == '#') preprocessor_line = 1;

            if (lang == LANG_MARKDOWN) {
                if (strncmp(s + probe, "```", 3) == 0 || strncmp(s + probe, "~~~", 3) == 0)
                    md_fence_line = 1;
                if (!in_md_fence && s[probe] == '#')
                    md_heading_line = 1;
            }

            while (s[i] != '\0' && s[i] != '\n') {
                unsigned char uch = (unsigned char)s[i];
                char draw_ch = isprint(uch) ? (char)uch : ' ';
                int code_like = (lang != LANG_MARKDOWN) || in_md_fence;

                if (in_line_comment) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (in_block_comment) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    if (!block_comment_is_html && s[i] == '*' && s[i + 1] == '/') {
                        i++;
                        col++;
                        if (s[i] != '\0' && s[i] != '\n') {
                            char slash = isprint((unsigned char)s[i]) ? s[i] : ' ';
                            render_put_style(col, screen_row, slash, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                            col++;
                            i++;
                        }
                        in_block_comment = 0;
                    } else if (block_comment_is_html &&
                               s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
                        for (int k = 0; k < 2; k++) {
                            i++;
                            col++;
                            if (s[i] == '\0' || s[i] == '\n') break;
                            char c2 = isprint((unsigned char)s[i]) ? s[i] : ' ';
                            render_put_style(col, screen_row, c2, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                        }
                        i++;
                        col++;
                        if (s[i] != '\0' && s[i] != '\n') {
                            char c3 = isprint((unsigned char)s[i]) ? s[i] : ' ';
                            render_put_style(col, screen_row, c3, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                        }
                        in_block_comment = 0;
                    } else {
                        col++;
                        i++;
                    }
                    continue;
                }

                if (lang == LANG_MARKDOWN && !in_md_fence && md_heading_line) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_DATATYPE, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (lang == LANG_MARKDOWN && md_fence_line) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_DATATYPE, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (lang == LANG_MARKDOWN && !in_md_fence && s[i] == '`') {
                    in_md_inline_code = !in_md_inline_code;
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_OPERATOR, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (in_md_inline_code) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_STRING, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (in_string) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_STRING, C_TEXT_BG, 1);
                    if (!escaped && s[i] == string_delim) in_string = 0;
                    if (!escaped && s[i] == '\\') escaped = 1;
                    else escaped = 0;
                    col++;
                    i++;
                    continue;
                }

                if (code_like && s[i] == '/' && s[i + 1] == '/') {
                    in_line_comment = 1;
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (lang == LANG_SQL && s[i] == '-' && s[i + 1] == '-') {
                    in_line_comment = 1;
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if ((lang == LANG_PYTHON || lang == LANG_SHELL) && s[i] == '#') {
                    in_line_comment = 1;
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (code_like && s[i] == '/' && s[i + 1] == '*') {
                    in_block_comment = 1;
                    block_comment_is_html = 0;
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if ((lang == LANG_HTML || lang == LANG_MARKDOWN) &&
                    s[i] == '<' && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                    in_block_comment = 1;
                    block_comment_is_html = 1;
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_COMMENT, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if ((code_like || lang == LANG_HTML) && (s[i] == '"' || s[i] == '\'')) {
                    in_string = 1;
                    escaped = 0;
                    string_delim = s[i];
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_STRING, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (preprocessor_line) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_DATATYPE, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                if (is_ident_start(uch) || (lang == LANG_CSS && (uch == '-' || uch == '_'))) {
                    size_t j = i + 1;
                    while ((lang == LANG_CSS && is_css_ident_char((unsigned char)s[j])) ||
                           (lang != LANG_CSS && is_ident_char((unsigned char)s[j])))
                        j++;
                    int is_kw = token_is_keyword(s + i, j - i);
                    int is_dt = token_is_datatype(s + i, j - i, lang);
                    int is_fn = token_is_function(s, j) && !is_kw && !is_dt;
                    for (size_t k = i; k < j; k++) {
                        char token_ch = isprint((unsigned char)s[k]) ? s[k] : ' ';
                        if (is_dt || is_kw)
                            render_put_style(col, screen_row, token_ch, C_SYNTAX_DATATYPE, C_TEXT_BG, 1);
                        else if (is_fn)
                            render_put_style(col, screen_row, token_ch, C_SYNTAX_FUNCTION, C_TEXT_BG, 1);
                        else
                            render_put(col, screen_row, token_ch, C_TEXT_FG, C_TEXT_BG);
                        col++;
                    }
                    i = j;
                    continue;
                }

                if ((isdigit(uch) || (s[i] == '.' && isdigit((unsigned char)s[i + 1]))) &&
                    (i == line_start || !is_ident_char((unsigned char)s[i - 1]))) {
                    size_t nlen = number_token_len(s, i);
                    if (nlen == 0) nlen = 1;
                    for (size_t k = 0; k < nlen && s[i + k] != '\0' && s[i + k] != '\n'; k++) {
                        char num_ch = isprint((unsigned char)s[i + k]) ? s[i + k] : ' ';
                        render_put_style(col, screen_row, num_ch, C_SYNTAX_NUMBER, C_TEXT_BG, 1);
                        col++;
                    }
                    i += nlen;
                    continue;
                }

                if (is_operator_char(uch) ||
                    (lang == LANG_MARKDOWN && strchr("#*_[]()!`>-", s[i]) != NULL)) {
                    render_put_style(col, screen_row, draw_ch, C_SYNTAX_OPERATOR, C_TEXT_BG, 1);
                    col++;
                    i++;
                    continue;
                }

                render_put(col, screen_row, draw_ch, C_TEXT_FG, C_TEXT_BG);
                col++;
                i++;
            }
            if (lang == LANG_MARKDOWN && md_fence_line) in_md_fence = !in_md_fence;
            while (col < w) {
                render_put(col, screen_row, ' ', C_TEXT_FG, C_TEXT_BG);
                col++;
            }
            if (s[i] == '\n') i++;
        }
        /* Lines beyond EOF are left as the blank from render_clear_back(). */
    }

    if (status_row >= 1 && status_row < h) {
        for (int col = 0; col < w; col++)
            render_put(col, status_row, ' ', C_BAR_FG, C_BAR_BG);

        if (status_left && *status_left) {
            int llen = (int)strlen(status_left);
            if (llen > w) llen = w;
            for (int col = 0; col < llen; col++)
                render_put(col, status_row, status_left[col], C_BAR_FG, C_BAR_BG);
        }

        if (status_right && *status_right) {
            int rlen = (int)strlen(status_right);
            if (rlen > w) rlen = w;
            int start = w - rlen;
            if (start < 0) start = 0;
            for (int col = 0; col < rlen && start + col < w; col++)
                render_put(start + col, status_row, status_right[col], C_BAR_FG, C_BAR_BG);
        }
    }

    if (prompt_active && prompt_row >= 1 && prompt_row < h) {
        for (int col = 0; col < w; col++)
            render_put(col, prompt_row, ' ', C_TEXT_FG, C_TEXT_BG);
        if (prompt_line && *prompt_line) {
            int plen = (int)strlen(prompt_line);
            if (plen > w) plen = w;
            for (int col = 0; col < plen; col++)
                render_put(col, prompt_row, prompt_line[col], C_TEXT_FG, C_TEXT_BG);
        }
    }

    /* --- Cursor cell (inverted colours) -------------------------------- */
    int cl = 0, cc = 0;
    index_to_line_col(s, cursor, &cl, &cc);
    int c_row = (cl - view_top) + 1;
    int c_col = gutter_w + cc;

    if (c_row < 1)    c_row = 1;
    if (c_row > rows) c_row = rows;
    if (c_col < gutter_w) c_col = gutter_w;
    if (c_col >= w)       c_col = w - 1;

    {
        char ch = s[cursor];
        if (!ch || ch == '\n') ch = ' ';
        if (!isprint((unsigned char)ch)) ch = ' ';
        render_put(c_col, c_row, ch, C_CURSOR_FG, C_CURSOR_BG);
    }

    if (out_cursor_col) *out_cursor_col = c_col;
    if (out_cursor_row) *out_cursor_row = c_row;
}

/* =========================================================================
 * main
 * ======================================================================= */

int main(int argc, char *argv[])
{
    if (argc == 1) {
        printf("Usage: %s <FILENAME>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "r+");
    char *s;
    int   sc, isfile;
    if (!fp) {
        sc = 16;
        s  = malloc(sc);
        if (!s) { perror("malloc"); return 1; }
        s[0] = '\0';
        isfile = 0;
    } else {
        int sz = file_size(fp);
        if (sz < 0) { perror("file_size"); fclose(fp); return 1; }
        sc = sz + 16;
        s  = malloc(sc);
        if (!s) { perror("malloc"); fclose(fp); return 1; }
        file_read(fp, s);
        isfile = 1;
    }

    atexit(cleanup);
    signal(SIGINT,   handle_sigint);
    signal(SIGWINCH, handle_sigwinch);

    term_enable_raw();
    term_mouse_enable();

    if (term_get_bg(&r, &g, &b) < 0) {
        fprintf(stderr, "Failed to get terminal background color\n");
        term_enable_raw();
        printf("press any key to continue. "
               "On exit terminal background will be set to black.");
        getc(stdin);
    }

    write(STDOUT_FILENO, "\033[?25l"      /* hide cursor        */
                         "\033[?2004h",  /* bracketed paste on */
                         14);

    render_init(term_get_width(), term_get_height());

    char heading[4096];
    snprintf(heading, sizeof(heading), "Editing %s", argv[1]);
    SyntaxLang lang = detect_language(argv[1]);
    char status_msg[256] = "";
    long long status_until_ms = 0;
    int dirty = 0;
    int quit_prompt = 0;
    char quit_input[32] = "";
    int quit_input_len = 0;

    size_t cursor          = strlen(s);
    int    view_top        = 0;
    int    keep_cursor_vis = 1;
    int    preferred_col   = -1;

    while (running) {

        if (resized) {
            resized = 0;
            render_resize(term_get_width(), term_get_height());
            render_invalidate();
        }

        int text_rows = text_rows_for_height(term_get_height(), quit_prompt);

        if (keep_cursor_vis)
            ensure_cursor_visible(s, cursor, &view_top, text_rows);

        if (status_msg[0] != '\0' && now_ms() >= status_until_ms)
            status_msg[0] = '\0';

        int cur_line = 0, cur_col = 0;
        index_to_line_col(s, cursor, &cur_line, &cur_col);
        char status_right[64];
        snprintf(status_right, sizeof(status_right), "%d, %d | %s",
                 cur_line + 1, cur_col + 1, newline_style(s));

        char prompt_line[256] = "";
        if (quit_prompt) {
            snprintf(prompt_line, sizeof(prompt_line),
                     "Save file? (yes, no, esc) %s", quit_input);
        }

        render_clear_back(C_TEXT_FG, C_TEXT_BG);

        int ccol = 0, crow = 1, gutter = 2;
        draw_frame(s, cursor, view_top, heading, lang,
                   status_msg, status_right, prompt_line, quit_prompt,
                   &ccol, &crow, &gutter);

        render_flip(ccol, crow);

        /* ---- Input ---- */
        int a = getc(stdin);

        if (quit_prompt) {
            if (a == '\x1b') { /* Esc */
                quit_prompt = 0;
                quit_input[0] = '\0';
                quit_input_len = 0;
                continue;
            }
            if (a == 127 || a == '\b') {
                if (quit_input_len > 0) {
                    quit_input[--quit_input_len] = '\0';
                }
                continue;
            }
            if (a == '\r' || a == '\n') {
                if (strcmp(quit_input, "yes") == 0 || strcmp(quit_input, "y") == 0) {
                    if (!isfile) {
                        fp = fopen(argv[1], "w+");
                        if (!fp) { perror("fopen"); return 1; }
                        isfile = 1;
                    }
                    if (file_write(fp, s) < 0) { perror("file_write"); return 1; }
                    dirty = 0;
                    running = 0;
                    continue;
                }
                if (strcmp(quit_input, "no") == 0 || strcmp(quit_input, "n") == 0) {
                    running = 0;
                    continue;
                }
                if (strcmp(quit_input, "esc") == 0) {
                    quit_prompt = 0;
                    quit_input[0] = '\0';
                    quit_input_len = 0;
                    continue;
                }
                snprintf(status_msg, sizeof(status_msg), "Type yes, no, or esc");
                status_until_ms = now_ms() + 3000;
                continue;
            }
            if (a != EOF && tolower((unsigned char)a) == 'y' && quit_input_len == 0) {
                strcpy(quit_input, "yes");
                quit_input_len = 3;
                continue;
            }
            if (a != EOF && tolower((unsigned char)a) == 'n' && quit_input_len == 0) {
                strcpy(quit_input, "no");
                quit_input_len = 2;
                continue;
            }
            if (isprint(a) && quit_input_len < (int)sizeof(quit_input) - 1) {
                quit_input[quit_input_len++] = (char)tolower(a);
                quit_input[quit_input_len] = '\0';
            }
            continue;
        }

        if (a == 127 || a == '\b') {        /* Backspace */
            size_t len = strlen(s);
            if (cursor > 0 && len > 0) {
                memmove(s + cursor - 1, s + cursor, len - cursor + 1);
                cursor--;
                keep_cursor_vis = 1;
                preferred_col   = -1;
                dirty = 1;
            }
        }
        else if (a == '\r' || a == '\n') {  /* Enter */
            if (insert_at_cursor(&s, &sc, &cursor, '\n') < 0) return 1;
            keep_cursor_vis = 1;
            preferred_col   = -1;
            dirty = 1;
        }
        else if (isprint(a)) {              /* Normal characters */
            if (insert_at_cursor(&s, &sc, &cursor, (char)a) < 0) return 1;
            keep_cursor_vis = 1;
            preferred_col   = -1;
            dirty = 1;
        }
        else if (a == 17) {                 /* Ctrl+Q — quit */
            if (dirty) {
                quit_prompt = 1;
                quit_input[0] = '\0';
                quit_input_len = 0;
                keep_cursor_vis = 1;
            } else {
                running = 0;
            }
        }
        else if (a == 19) {                 /* Ctrl+S — save */
            if (!isfile) {
                fp = fopen(argv[1], "w+");
                if (!fp) { perror("fopen"); return 1; }
                isfile = 1;
            }
            if (file_write(fp, s) < 0) { perror("file_write"); return 1; }
            dirty = 0;
            snprintf(status_msg, sizeof(status_msg), "Saved to file %s", argv[1]);
            status_until_ms = now_ms() + 3000;
        }
        else if (a == '\x1b') {
            char seq[128];
            int n = read_escape_seq(seq, sizeof(seq));
            if (n < 0) continue;

            if (strcmp(seq, "[200~") == 0) {        /* Bracketed paste start */
                if (read_bracketed_paste(&s, &sc, &cursor) < 0) return 1;
                keep_cursor_vis = 1;
                preferred_col   = -1;
                dirty = 1;
                snprintf(status_msg, sizeof(status_msg), "Pasted clipboard");
                status_until_ms = now_ms() + 3000;
            }
            else if (strcmp(seq, "[D") == 0) {      /* Left */
                if (cursor > 0) cursor--;
                keep_cursor_vis = 1; preferred_col = -1;
            }
            else if (strcmp(seq, "[C") == 0) {      /* Right */
                if (cursor < strlen(s)) cursor++;
                keep_cursor_vis = 1; preferred_col = -1;
            }
            else if (strcmp(seq, "[A") == 0) {      /* Up */
                int ln = 0, col = 0;
                index_to_line_col(s, cursor, &ln, &col);
                if (preferred_col < 0) preferred_col = col;
                if (ln > 0) cursor = line_col_to_index(s, ln - 1, preferred_col);
                keep_cursor_vis = 1;
            }
            else if (strcmp(seq, "[B") == 0) {      /* Down */
                int ln = 0, col = 0;
                index_to_line_col(s, cursor, &ln, &col);
                if (preferred_col < 0) preferred_col = col;
                cursor = line_col_to_index(s, ln + 1, preferred_col);
                keep_cursor_vis = 1;
            }
            else if (strncmp(seq, "[<", 2) == 0) {  /* Mouse */
                int btn = 0, p1 = 0, p2 = 0;
                char final = 0;
                if (sscanf(seq + 2, "%d;%d;%d%c",
                            &btn, &p1, &p2, &final) == 4) {
                    int term_h  = term_get_height();
                    int rows    = text_rows_for_height(term_h, quit_prompt);
                    int total   = count_lines(s);
                    int max_top = total - rows;
                    if (max_top < 0) max_top = 0;

                    if (btn == 64 && final == 'M') {           /* Scroll up */
                        if (view_top > 0) view_top -= 2;
                        keep_cursor_vis = 0;
                    }
                    else if (btn == 65 && final == 'M') {      /* Scroll down */
                        if (view_top < max_top) view_top += 2;
                        keep_cursor_vis = 0;
                    }
                    else if (((btn & 3) == 0) && final == 'M') { /* Left click */
                        if (p2 < 2 || p2 > rows + 1) continue;
                        int target_line = view_top + (p2 - 2);
                        int target_col  = p1 - 1 - gutter;
                        cursor = line_col_to_index(s, target_line, target_col);
                        keep_cursor_vis = 1;
                        preferred_col   = -1;
                    }
                }
            }
        }
    }

    free(s);
    if (isfile) fclose(fp);
    return 0;
}
// Hello from uno!
