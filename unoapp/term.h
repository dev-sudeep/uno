/*
 * term.h - Terminal utility functions
 *
 * Provides:
 *   - Terminal dimensions (width / height)
 *   - Raw mode enable / disable
 *   - True-colour (RGB) text and background colouring
 *   - Screen clear / cursor home shortcuts
 *   - Cursor movement and position query
 *   - Non-blocking key detection
 *   - Full background fill
 *
 * All functions are static inline — simply #include "term.h" in every
 * translation unit that needs it; no separate compilation step is required.
 *
 * All functions return 0 on success and -1 on failure (errno is set).
 * Compatible with Linux, macOS and other POSIX systems.
 */

#ifndef TERM_H
#define TERM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal state  (static — one copy per translation unit, intentionally)
 * ---------------------------------------------------------------------- */

static int            _term_raw_active = 0;
static struct termios _term_saved_attrs;

/* =========================================================================
 * Screen control macros
 * ======================================================================= */

#define TERM_CLEAR_SCREEN  "\033[2J\033[H"  /* clear entire screen + cursor to (0,0) */
#define TERM_CURSOR_HOME   "\033[H"         /* cursor to (0,0) without clearing       */
#define TERM_HIDE_CURSOR   "\033[?25l"      /* hide cursor                            */
#define TERM_SHOW_CURSOR   "\033[?25h"      /* show cursor                            */

/**
 * term_clear - Clear the entire screen and move cursor to (0,0).
 * @return  0 on success, -1 on failure.
 */
static inline int term_clear(void)
{
    return (fputs(TERM_CLEAR_SCREEN, stdout) == EOF) ? -1 : 0;
}

/**
 * term_home - Move cursor to (0,0) without clearing the screen.
 * @return  0 on success, -1 on failure.
 */
static inline int term_home(void)
{
    return (fputs(TERM_CURSOR_HOME, stdout) == EOF) ? -1 : 0;
}

/* =========================================================================
 * Dimensions
 * ======================================================================= */

/**
 * term_get_width - Return the terminal width in columns.
 *
 * Uses ioctl(TIOCGWINSZ), falling back to the COLUMNS environment variable
 * and finally to 80 if neither is available.
 *
 * @return  Width in columns (>= 1).
 */
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

/**
 * term_get_height - Return the terminal height in rows.
 *
 * Uses ioctl(TIOCGWINSZ), falling back to the LINES environment variable
 * and finally to 24 if neither is available.
 *
 * @return  Height in rows (>= 1).
 */
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

/* =========================================================================
 * Raw mode
 * ======================================================================= */

/**
 * term_enable_raw - Switch stdin to raw (non-canonical, no-echo) mode.
 *
 * Saves the previous terminal attributes so they can be fully restored by
 * term_disable_raw().  Calling this more than once without an intervening
 * term_disable_raw() is safe — subsequent calls are no-ops.
 *
 * Note: ISIG is kept enabled so Ctrl-C continues to generate SIGINT.
 * Handle the signal yourself if you need custom exit behaviour.
 *
 * @return  0 on success, -1 on failure (errno set).
 */
static inline int term_enable_raw(void)
{
    if (_term_raw_active) return 0;

    if (!isatty(STDIN_FILENO)) { errno = ENOTTY; return -1; }
    if (tcgetattr(STDIN_FILENO, &_term_saved_attrs) < 0) return -1;

    struct termios raw = _term_saved_attrs;

    /* Input: disable break signal, CR->NL, parity check,
              8th-bit strip, XON/XOFF flow control.        */
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    /* Output: disable post-processing (\n -> \r\n etc.). */
    raw.c_oflag &= (tcflag_t)~(OPOST);

    /* Control: 8-bit characters. */
    raw.c_cflag |= (tcflag_t)(CS8);

    /* Local: no echo, no canonical mode, no extended processing.
              ISIG is intentionally kept so Ctrl-C raises SIGINT. */
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);

    /* read() returns as soon as >= 1 byte is available. */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return -1;

    write(STDOUT_FILENO, "\x1b[?2004h", 8); // enable bracketed paste
    
    _term_raw_active = 1;
    return 0;
}

/**
 * term_disable_raw - Restore the terminal to the state before raw mode.
 *
 * If term_enable_raw() has not been called this is a no-op and returns 0.
 *
 * @return  0 on success, -1 on failure (errno set).
 */
static inline int term_disable_raw(void)
{
    if (!_term_raw_active) return 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &_term_saved_attrs) < 0) return -1;

    write(STDOUT_FILENO, "\x1b[?2004l", 8); // disable
    _term_raw_active = 0;
    return 0;
}

/**
 * term_is_raw - Check whether raw mode is currently active.
 *
 * @return  1 if raw mode is active, 0 otherwise.
 */
static inline int term_is_raw(void)
{
    return _term_raw_active;
}

/* =========================================================================
 * Cursor movement
 * ======================================================================= */

/**
 * term_move - Move the cursor to a specific (col, row) position.
 *
 * Coordinates are 0-based; internally converted to ANSI 1-based values.
 *
 * @param col  Column (0-based, left to right).
 * @param row  Row    (0-based, top to bottom).
 * @return     0 on success, -1 on failure.
 */
static inline int term_move(int col, int row)
{
    return (fprintf(stdout, "\033[%d;%dH", row + 1, col + 1) < 0) ? -1 : 0;
}

/**
 * term_move_up - Move the cursor up by n rows.
 * @return  0 on success, -1 on failure.
 */
static inline int term_move_up(int n)
{
    return (fprintf(stdout, "\033[%dA", n) < 0) ? -1 : 0;
}

/**
 * term_move_down - Move the cursor down by n rows.
 * @return  0 on success, -1 on failure.
 */
static inline int term_move_down(int n)
{
    return (fprintf(stdout, "\033[%dB", n) < 0) ? -1 : 0;
}

/**
 * term_move_right - Move the cursor right by n columns.
 * @return  0 on success, -1 on failure.
 */
static inline int term_move_right(int n)
{
    return (fprintf(stdout, "\033[%dC", n) < 0) ? -1 : 0;
}

/**
 * term_move_left - Move the cursor left by n columns.
 * @return  0 on success, -1 on failure.
 */
static inline int term_move_left(int n)
{
    return (fprintf(stdout, "\033[%dD", n) < 0) ? -1 : 0;
}

/**
 * term_get_pos - Query the cursor's current (col, row) position via DSR.
 *
 * Sends the ANSI Device Status Report escape (ESC[6n) and reads back
 * ESC[<row>;<col>R from stdin. Requires raw mode to be active so the
 * response is not echoed or buffered.
 *
 * @param col  Output: current column (0-based).
 * @param row  Output: current row    (0-based).
 * @return     0 on success, -1 on failure.
 */
static inline int term_get_pos(int *col, int *row)
{
    if (!col || !row) { errno = EINVAL; return -1; }

    if (fputs("\033[6n", stdout) == EOF) return -1;
    fflush(stdout);

    /* Response format: ESC [ <row> ; <col> R */
    int r = 0, c = 0;
    if (scanf("\033[%d;%dR", &r, &c) != 2) return -1;

    *row = r - 1;
    *col = c - 1;
    return 0;
}

/* =========================================================================
 * Colour  (24-bit / true-colour ANSI escape sequences)
 * ======================================================================= */

/**
 * term_set_fg - Set the foreground (text) colour to an RGB value.
 *
 * Emits ESC[38;2;<r>;<g>;<b>m to stdout.
 *
 * @param r  Red channel   [0-255].
 * @param g  Green channel [0-255].
 * @param b  Blue channel  [0-255].
 * @return   0 on success, -1 on failure.
 */
static inline int term_set_fg(uint8_t r, uint8_t g, uint8_t b)
{
    return (fprintf(stdout, "\033[38;2;%d;%d;%dm",
                    (int)r, (int)g, (int)b) < 0) ? -1 : 0;
}

/**
 * term_set_bg - Set the background colour to an RGB value.
 *
 * Emits ESC[48;2;<r>;<g>;<b>m to stdout.
 *
 * @param r  Red channel   [0-255].
 * @param g  Green channel [0-255].
 * @param b  Blue channel  [0-255].
 * @return   0 on success, -1 on failure.
 */
static inline int term_set_bg(uint8_t r, uint8_t g, uint8_t b)
{
    return (fprintf(stdout, "\033[48;2;%d;%d;%dm",
                    (int)r, (int)g, (int)b) < 0) ? -1 : 0;
}


int term_get_bg(int* r, int* g, int* b) {
    char response[64];
    int i = 0;

    term_enable_raw();

    // Send the OSC 11 query
    // \033]11;?\a  (Query background color)
    write(STDOUT_FILENO, "\033]11;?\a", 7);

    // 3. Read the response from stdin
    // Response format: \033]11;rgb:rrrr/gggg/bbbb\a
    // We read until the BEL (\a) or a timeout occurs
    while (i < sizeof(response) - 1) {
        if (read(STDIN_FILENO, &response[i], 1) <= 0) break;
        if (response[i] == '\a' || response[i] == '\\') break; 
        i++;
    }
    response[i] = '\0';

    

    // 5. Parse the RGB values
    // Looking for "rgb:RRRR/GGGG/BBBB"
    char* rgb_start = strstr(response, "rgb:");
    if (rgb_start && sscanf(rgb_start, "rgb:%x/%x/%x", r, g, b) == 3) {
        // Most terminals return 16-bit values (0-65535)
        // Scale down to 8-bit (0-255) if needed
        *r /= 256; *g /= 256; *b /= 256;
        return 0; // Success
    }

    return -1; // Failure
}


/**
 * term_reset_color - Reset foreground and background to terminal defaults.
 *
 * Emits ESC[0m to stdout.
 *
 * @return  0 on success, -1 on failure.
 */
static inline int term_reset_color(void)
{
    return (fputs("\033[0m", stdout) == EOF) ? -1 : 0;
}

/**
 * term_fill_bg - Fill the entire terminal background with an RGB colour.
 *
 * Sets the background colour and overwrites every cell with spaces, then
 * resets colour and moves the cursor back to the top-left.
 *
 * @param r  Red channel   [0-255].
 * @param g  Green channel [0-255].
 * @param b  Blue channel  [0-255].
 * @return   0 on success, -1 on failure.
 */
static inline int term_fill_bg(uint8_t r, uint8_t g, uint8_t b)
{
    int w = term_get_width();
    int h = term_get_height();

    if (term_set_bg(r, g, b) < 0) return -1;

    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            if (fputc(' ', stdout) == EOF) return -1;

    if (term_reset_color() < 0) return -1;
    return (fputs(TERM_CURSOR_HOME, stdout) == EOF) ? -1 : 0;
}

/* -------------------------------------------------------------------------
 * Predefined RGB colour helpers  (expand to r,g,b — pass to term_set_fg/bg)
 *
 * Usage:  term_set_fg(TERM_RED);
 *         term_set_bg(TERM_BLUE);
 *         term_fill_bg(TERM_BLACK);
 * ---------------------------------------------------------------------- */
#define TERM_BLACK     0,   0,   0
#define TERM_WHITE   255, 255, 255
#define TERM_RED     220,  50,  47
#define TERM_GREEN    42, 161, 152
#define TERM_YELLOW  181, 137,   0
#define TERM_BLUE     38, 139, 210
#define TERM_MAGENTA 211,  54, 130
#define TERM_CYAN      0, 168, 168

/* =========================================================================
 * Input
 * ======================================================================= */

/**
 * term_is_key - Non-blocking check if a specific key was pressed on stdin.
 *
 * Uses select() to poll stdin without blocking. If a byte is available but
 * does not match c, it is pushed back via ungetc() so future reads see it.
 * Intended to be called once per frame inside a render loop.
 *
 * Requires raw mode to be active (term_enable_raw) so bytes are available
 * immediately without waiting for Enter.
 *
 * @param c  The character to check for (e.g. 0x03 for Ctrl+C, 'q' to quit).
 * @return   1 if the key was pressed, 0 otherwise.
 */
static inline int term_is_key(char c)
{
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        int byte = getchar();
        if (byte != EOF) {
            if ((char)byte == c)
                return 1;
            ungetc(byte, stdin);  /* put it back if it doesn't match */
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* TERM_H */
