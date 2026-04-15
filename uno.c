#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include "term.h"

#define TERM_CLEAR_SCREEN  "\033[2J\033[H"
#define TERM_CURSOR_HOME   "\033[H"
#define FPS       30
#define FRAME_US  (1000000 / FPS)

static volatile int running = 1;

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

void cleanup(void) {
    term_disable_raw();
    fputs("\033[?25h", stdout);  /* restore cursor */
    fflush(stdout);
}

void clear(void) {
    fputs(TERM_CLEAR_SCREEN, stdout);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        printf("Usage: %s <FILENAME>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror("Unable to open file");
        return 1;
    }

    atexit(cleanup);
    signal(SIGINT, handle_sigint);
    term_enable_raw();
    fputs("\033[?25l", stdout);  /* hide cursor */

    struct timespec t0, t1;

    char heading[4096];
    snprintf(heading, sizeof(heading), "Editing %s", argv[1]);

    char s[4096] = "";

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &t0);

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
        printf("\n%s", s);

        /* Fake cursor */
        term_set_bg(TERM_WHITE);
        printf(" ");

        fflush(stdout);

        int a = getc(stdin);

        if (a == 127 || a == '\b') {  /* Backspace */
            size_t len = strlen(s);
            if (len > 0) {
                s[len - 1] = '\0';
            }
        }
        else if (a == '\r' || a == '\n') {  /* Enter */
            size_t len = strlen(s);
            if (len < sizeof(s) - 1) {
                s[len] = '\n';
                s[len + 1] = '\0';
            }
        }
        else if (isprint(a)) {  /* Normal characters */
            size_t len = strlen(s);
            if (len < sizeof(s) - 1) {
                s[len] = (char)a;
                s[len + 1] = '\0';
            }
        }
        else if (a == 17) {  /* Ctrl+Q */
            running = 0;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_us = (t1.tv_sec  - t0.tv_sec)  * 1000000L
                        + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        long remaining  = FRAME_US - elapsed_us;

        if (remaining > 0)
            usleep((useconds_t)remaining);
    }

    fclose(fp);
    return 0;
}
