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

void showstr(char* str) {
    int count = 1;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') count++;
    }

    term_move(0, 1);
    term_set_bg(64, 64, 64);
    term_set_fg(229, 229, 229);
    int d = countDigits(count) + 1;
    int x = d, y = 1;
    printf("%d%*c", y, d - y, ' ');
    term_move(x, y);
    term_set_fg(TERM_WHITE);

    for (int i = 0; str[i] != '\0'; i++) {
        term_set_bg(100, 100, 100);
        term_set_fg(TERM_WHITE);
        term_move(x, y);
        if (isprint(str[i])) {
            putc(str[i], stdout);
            x++;
            term_move(x, y);
        } else if (str[i] == '\n' || str[i] == '\r') {
            x = d;
            y++;
            if ((str[i] == '\n' && str[i+1] == '\r') ||
                (str[i] == '\r' && str[i+1] == '\n')) {
                i++;
            }
            term_move(0, y);
            term_set_bg(64, 64, 64);
            term_set_fg(229, 229, 229);
            printf("%d%*c", y, d - countDigits(y), ' ');
            term_move(x, y);
        }
    }

    int w = x;
    int l = y;
    for (; y < term_get_height(); y++) {
        term_move(0, y + 1);
        term_set_bg(64, 64, 64);
        term_set_fg(229, 229, 229);
        printf("%*c", d, ' ');
    }
    term_move(w, l);
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
    if (-term_get_bg(&r, &b, &g)) {
        fprintf(stderr, "Failed to get terminal background color\n");
        term_enable_raw();
        printf("press any key to continue. Note that on exit terminal background color will automatically be set to black.");
        getc(stdin);
    }
    fputs("\033[?25l", stdout);  /* hide cursor */

    char heading[4096];
    snprintf(heading, sizeof(heading), "Editing %s", argv[1]);

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

        /* Print buffer */
        showstr(s);

        /* Fake cursor */
        term_set_bg(TERM_WHITE);
        printf(" ");

        fflush(stdout);

        int a = getc(stdin);

        if (a == 127 || a == '\b') {  /* Backspace */
            size_t len = strlen(s);
            if (len > 0) s[len - 1] = '\0';
        }
        else if (a == '\r' || a == '\n') {  /* Enter */
            if (ENSURE_CAP(s, sc, 2)) return 1;
            size_t len = strlen(s);
            s[len] = '\n'; s[len + 1] = '\0';
        }
        else if (isprint(a)) {  /* Normal characters */
            if (ENSURE_CAP(s, sc, 2)) return 1;
            size_t len = strlen(s);
            s[len] = (char)a; s[len + 1] = '\0';
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
            /* Read 5 bytes to check for bracketed paste start: \033[200~ */
            char seq[5];
            for (int i = 0; i < 5; i++)
                seq[i] = getc(stdin);

            if (seq[0]=='[' && seq[1]=='2' && seq[2]=='0' && seq[3]=='0' && seq[4]=='~') {
                /*
                 * Bracketed paste — read until end sequence \033[201~
                 *
                 * Use a 5-byte sliding window so the entire end sequence
                 * fits in the window before any of it gets flushed to the
                 * buffer. p[0] is always the oldest (next-to-flush) byte.
                 */
                int p[5] = {0, 0, 0, 0, 0};
                int c;
                while ((c = getc(stdin)) != EOF) {
                    /* End sequence: window holds \033[201 and current byte is ~ */
                    if (p[0]=='\x1b' && p[1]=='[' && p[2]=='2' && p[3]=='0' && p[4]=='1' && c=='~')
                        break;

                    /* p[0] is safe to flush — can't be start of end sequence */
                    if (p[0] != 0) {
                        if (ENSURE_CAP(s, sc, 2)) return 1;
                        size_t len = strlen(s);
                        if (p[0] == '\r') {
                            /* Normalise CR / CR+LF to LF */
                            if (p[1] != '\n') { s[len] = '\n'; s[len+1] = '\0'; }
                        } else {
                            s[len] = (char)p[0]; s[len+1] = '\0';
                        }
                    }

                    /* Shift window */
                    p[0]=p[1]; p[1]=p[2]; p[2]=p[3]; p[3]=p[4]; p[4]=c;
                }

                /* Flush remaining window bytes — none are part of end sequence */
                for (int i = 0; i < 5; i++) {
                    if (p[i] == 0) continue;
                    if (ENSURE_CAP(s, sc, 2)) return 1;
                    size_t len = strlen(s);
                    if (p[i] == '\r') {
                        if (i+1 < 5 && p[i+1] != '\n') { s[len] = '\n'; s[len+1] = '\0'; }
                    } else {
                        s[len] = (char)p[i]; s[len+1] = '\0';
                    }
                }
            } else {
                /* Not a bracketed paste — drain remaining buffered bytes */
                struct timeval tv = {0, 10000};
                fd_set fds;
                FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
                while (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) > 0) {
                    getc(stdin);
                    FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
                    tv.tv_usec = 1000;
                }
            }
        }

        fflush(stdout);
    }

    free(s);
    if (isfile) fclose(fp);
    return 0;
}
