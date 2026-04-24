#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <sys/select.h>
#include "term.h"

int r=0, g=0, b=0;


/* =========================================================================
 * File size
 * ======================================================================= */

/**
 * file_size - Return the size of an open file in bytes.
 *
 * Seeks to the end of the file to measure its size, then seeks back to
 * wherever the caller left the position.
 *
 * @param fp  Open FILE* in any mode.
 * @return    Size in bytes (>= 0), or -1 on failure (errno set).
 */
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

/* =========================================================================
 * File read
 * ======================================================================= */

/**
 * file_read - Read the entire contents of a file into a caller-provided buffer
 *             as a null-terminated string.
 *
 * The buffer must be at least file_size(fp) + 1 bytes large to hold the
 * contents plus the null terminator. The file position is reset to the
 * beginning before reading and left at the end after.
 *
 * @param fp   Open FILE* (any mode that allows reading, e.g. "r+").
 * @param buf  Caller-allocated buffer to write into.
 * @return     0 on success, -1 on failure (errno set).
 */
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

/**
 * file_write - Overwrite the entire contents of a file with a string.
 *
 * Truncates the file to zero length, seeks to the beginning, then writes
 * the null-terminated string. The file must be open in a mode that allows
 * writing (e.g. "r+", "w+").
 *
 * @param fp   Open FILE* in a writable mode.
 * @param buf  Null-terminated string to write.
 * @return     0 on success, -1 on failure (errno set).
 */
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

#define TERM_CLEAR_SCREEN  "\033[2J\033[H"
#define TERM_CURSOR_HOME   "\033[H"

#define ENSURE_CAP(s, sc, n)                                        \
    __extension__({                                                  \
        int _fail = 0;                                               \
        while (strlen(s) + (size_t)(n) + 1 >= (size_t)(sc)) {       \
            (sc) *= 2;                                               \
            char *_tmp = realloc((s), (sc));                         \
            if (!_tmp) { perror("realloc"); _fail = 1; break; }      \
            (s) = _tmp;                                              \
        }                                                            \
        _fail;                                                       \
    })

static volatile int running = 1;

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

void cleanup(void) {
    term_mouse_disable();
    term_disable_raw();
    term_set_bg(r, g, b);
    fputs("\033[?25h" TERM_CLEAR_SCREEN, stdout);  /* restore cursor and clear screen */
    fflush(stdout);
}

void clear(void) {
    fputs(TERM_CURSOR_HOME, stdout);
    fflush(stdout);
}

int countDigits(int n) {
    if (n == 0) return 1;
    int count = 0;
    if (n < 0) n = -n;
    while (n != 0) { n /= 10; count++; }
    return count;
}

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
    for (size_t i = 0; s && s[i] != '\0'; i++) {
        if (s[i] == '\n') lines++;
    }
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
    if (col) *col = c;
}

static size_t line_col_to_index(const char *s, int target_line, int target_col)
{
    if (!s) return 0;
    if (target_line < 0) target_line = 0;
    if (target_col < 0) target_col = 0;

    int line = 0, col = 0;
    size_t i = 0;
    while (s[i] != '\0') {
        if (line == target_line) break;
        if (s[i] == '\n') line++;
        i++;
    }
    if (line < target_line) return strlen(s);

    while (s[i] != '\0' && s[i] != '\n' && col < target_col) {
        i++;
        col++;
    }
    return i;
}

static void ensure_cursor_visible(const char *s, size_t cursor, int *view_top)
{
    int h = term_get_height();
    int rows = (h > 1) ? (h - 1) : 1;
    int total = count_lines(s);
    int max_top = total - rows;
    if (max_top < 0) max_top = 0;

    int line = 0, col = 0;
    index_to_line_col(s, cursor, &line, &col);
    (void)col;

    if (*view_top > max_top) *view_top = max_top;
    if (*view_top < 0) *view_top = 0;
    if (line < *view_top) *view_top = line;
    if (line >= *view_top + rows) *view_top = line - rows + 1;
    if (*view_top > max_top) *view_top = max_top;
    if (*view_top < 0) *view_top = 0;
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

/* Read bytes after ESC using blocking getc() until an alphabetic final byte. */
static int read_escape_alpha_seq(char *seq, int cap)
{
    int n = 0;
    if (!seq || cap < 2) return -1;

    while (n < cap - 1) {
        int ch = getc(stdin);
        if (ch == EOF || ch < 0) return -1;
        seq[n++] = (char)ch;
        if (isalpha((unsigned char)ch)) {
            seq[n] = '\0';
            return n;
        }
    }

    /* Drain until a final alphabetic byte so leftovers don't leak to text input. */
    for (;;) {
        int ch = getc(stdin);
        if (ch == EOF || ch < 0) break;
        if (isalpha((unsigned char)ch)) break;
    }

    seq[n] = '\0';
    return -1;
}

void showstr(const char *str, size_t cursor, int view_top, int *cursor_col_out, int *cursor_row_out, int *gutter_out) {
    int total_lines = count_lines(str);
    int d = countDigits(total_lines) + 1;
    int h = term_get_height();
    int w = term_get_width();
    int rows = (h > 1) ? (h - 1) : 1;
    int max_top = total_lines - rows;
    if (max_top < 0) max_top = 0;
    if (view_top < 0) view_top = 0;
    if (view_top > max_top) view_top = max_top;

    size_t i = 0;
    int line = 0;
    while (str[i] != '\0' && line < view_top) {
        if (str[i] == '\n') line++;
        i++;
    }

    for (int rline = 0; rline < rows; rline++) {
        int lnum = view_top + rline;

        term_move(0, rline + 1);
        term_set_bg(64, 64, 64);
        term_set_fg(229, 229, 229);
        if (lnum < total_lines) {
            printf("%d%*c", lnum + 1, d - countDigits(lnum + 1), ' ');
        } else {
            printf("%*c", d, ' ');
        }

        term_set_bg(100, 100, 100);
        term_set_fg(TERM_WHITE);
        int x = d;
        if (lnum < total_lines) {
            while (str[i] != '\0' && str[i] != '\n') {
                if (x < w) {
                    term_move(x, rline + 1);
                    putc(isprint((unsigned char)str[i]) ? str[i] : ' ', stdout);
                }
                x++;
                i++;
            }
            if (str[i] == '\n') i++;
        }
    }

    int cl = 0, cc = 0;
    index_to_line_col(str, cursor, &cl, &cc);
    int c_row = (cl - view_top) + 1;
    int c_col = d + cc;

    if (c_row < 1) c_row = 1;
    if (c_row > rows) c_row = rows;
    if (c_col < d) c_col = d;
    if (c_col >= w) c_col = w - 1;

    if (cursor_col_out) *cursor_col_out = c_col;
    if (cursor_row_out) *cursor_row_out = c_row;
    if (gutter_out) *gutter_out = d;
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        printf("Usage: %s <FILENAME>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "r+");
    char* s;
    int sc, isfile;
    if (!fp) {
        sc = 16;
        s  = (char*)malloc(sc);
        if (!s) { perror("malloc"); return 1; }
        s[0] = '\0';
        isfile = 0;
    } else {
        int sz = file_size(fp);
        if (sz < 0) { perror("file_size"); fclose(fp); return 1; }
        sc = sz + 16;
        s  = (char*)malloc(sc);
        if (!s) { perror("malloc"); fclose(fp); return 1; }
        file_read(fp, s);
        isfile = 1;
    }

    atexit(cleanup);
    signal(SIGINT, handle_sigint);
    term_enable_raw();
    term_mouse_enable();
    if (-term_get_bg(&r, &b, &g)) {
        fprintf(stderr, "Failed to get terminal background color\n");
        term_enable_raw();
        printf("press any key to continue. Note that on exit terminal background color will automatically be set to black.");
        getc(stdin);
    }
    fputs("\033[?25l", stdout);  /* hide cursor */

    char heading[4096];
    snprintf(heading, sizeof(heading), "Editing %s", argv[1]);
    size_t cursor = strlen(s);
    int view_top = 0;
    int keep_cursor_visible = 1;
    int preferred_col = -1;

    while (running) {
        clear();

        int w = term_get_width();
        int x = (w / 2) - (int)strlen(heading) / 2;
        if (x < 0) x = 0;

        term_fill_bg(100, 100, 100);
        term_set_fg(TERM_BLACK);
        term_set_bg(TERM_WHITE);

        printf("\033[?25l");
        printf("%*c%s%*c", x, ' ', heading, x, ' ');

        term_set_bg(100, 100, 100);

        if (keep_cursor_visible) ensure_cursor_visible(s, cursor, &view_top);

        /* Print buffer */
        int ccol = 0, crow = 1, gutter = 2;
        showstr(s, cursor, view_top, &ccol, &crow, &gutter);

        /* Fake cursor */
        term_move(ccol, crow);
        term_set_bg(TERM_WHITE);
        printf(" ");

        fflush(stdout);

        int a = getc(stdin);

        if (a == 127 || a == '\b') {  /* Backspace */
            size_t len = strlen(s);
            if (cursor > 0 && len > 0) {
                memmove(s + cursor - 1, s + cursor, len - cursor + 1);
                cursor--;
                keep_cursor_visible = 1;
                preferred_col = -1;
            }
        }
        else if (a == '\r' || a == '\n') {  /* Enter */
            if (insert_at_cursor(&s, &sc, &cursor, '\n') < 0) return 1;
            keep_cursor_visible = 1;
            preferred_col = -1;
        }
        else if (isprint(a)) {  /* Normal characters */
            if (insert_at_cursor(&s, &sc, &cursor, (char)a) < 0) return 1;
            keep_cursor_visible = 1;
            preferred_col = -1;
        }
        else if (a == 17) {  /* Ctrl+Q */
            running = 0;
        }
        else if (a == 19) {  /* Ctrl+S */
            if (!isfile) {
                fp = fopen(argv[1], "w+");
                isfile = 1;
            }
            file_write(fp, s);
        }
        else if (a == '\x1b') {
            char seq[128];
            int n = read_escape_alpha_seq(seq, sizeof(seq));
            if (n < 0) continue;

            if (strcmp(seq, "[D") == 0) {  /* ESC [ D : Left */
                if (cursor > 0) cursor--;
                keep_cursor_visible = 1;
                preferred_col = -1;
            }
            else if (strcmp(seq, "[C") == 0) {  /* ESC [ C : Right */
                if (cursor < strlen(s)) cursor++;
                keep_cursor_visible = 1;
                preferred_col = -1;
            }
            else if (strcmp(seq, "[A") == 0) {  /* ESC [ A : Up */
                int line = 0, col = 0;
                index_to_line_col(s, cursor, &line, &col);
                if (preferred_col < 0) preferred_col = col;
                if (line > 0) cursor = line_col_to_index(s, line - 1, preferred_col);
                keep_cursor_visible = 1;
            }
            else if (strcmp(seq, "[B") == 0) {  /* ESC [ B : Down */
                int line = 0, col = 0;
                index_to_line_col(s, cursor, &line, &col);
                if (preferred_col < 0) preferred_col = col;
                cursor = line_col_to_index(s, line + 1, preferred_col);
                keep_cursor_visible = 1;
            }
            else if (strncmp(seq, "[<", 2) == 0) {  /* ESC [ < ... M mouse */
                int btn = 0, p1 = 0, p2 = 0;
                char final = 0;
                if (sscanf(seq + 2, "%d;%d;%d%c", &btn, &p1, &p2, &final) == 4) {
                    int term_h = term_get_height();
                    int rows = (term_h > 1) ? term_h - 1 : 1;
                    int total = count_lines(s);
                    int max_top = total - rows;
                    if (max_top < 0) max_top = 0;

                    if (btn == 64 && final == 'M') { /* ESC [ < 64 ; col ; row M */
                        if (view_top > 0) view_top--;
                        keep_cursor_visible = 0;
                    }
                    else if (btn == 65 && final == 'M') { /* ESC [ < 65 ; col ; row M */
                        if (view_top < max_top) view_top++;
                        keep_cursor_visible = 0;
                    }
                    else if (((btn & 3) == 0) && final == 'M') { /* Left click: ESC [ < 0 ; col ; row M */
                        int col = p1;
                        int row = p2;
                        int target_line = view_top + (row - 2);
                        int target_col = col - 1 - gutter;
                        cursor = line_col_to_index(s, target_line, target_col);
                        keep_cursor_visible = 1;
                        preferred_col = -1;
                    }
                }
            }
        }

        fflush(stdout);
    }

    free(s);
    if (isfile) fclose(fp);
    return 0;
}
