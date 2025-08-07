#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_BUFFER 8192

volatile sig_atomic_t window_resized = 0;
struct termios orig_term;
int width = 80, height = 24;

void handle_winch(int sig) {
    window_resized = 1;
}

void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
}

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_term);
    atexit(disable_raw_mode);

    struct termios raw = orig_term;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void get_terminal_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
}

void clear_screen(void) {
    printf("\033[2J\033[H");
}

void move_cursor(int x, int y) {
    printf("\033[%d;%dH", y, x);
}

int line_offset[MAX_BUFFER]; // stores start index of each line in buffer

void recalculate_lines(char* buffer, size_t len, int* total_lines) {
    *total_lines = 0;
    line_offset[0] = 0;
    for (size_t i = 0; i < len; i++) {
        if (buffer[i] == '\n') {
            (*total_lines)++;
            line_offset[*total_lines] = i + 1;
        }
    }
    (*total_lines)++;
}

int get_line_from_cursor(size_t cursor, int total_lines) {
    for (int i = 0; i < total_lines; i++) {
        if (cursor < line_offset[i + 1] || i + 1 == total_lines)
            return i;
    }
    return total_lines - 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: uno <filename>\n");
        return 1;
    }

    enable_raw_mode();
    get_terminal_size();
    signal(SIGWINCH, handle_winch);

    FILE* file = fopen(argv[1], "r+");
    if (!file) file = fopen(argv[1], "w+");
    if (!file) {
        perror("Failed to open file");
        return 1;
    }

    char buffer[MAX_BUFFER] = {0};
    size_t len = fread(buffer, 1, MAX_BUFFER - 1, file);
    size_t cursor = len;
    int total_lines = 0;
    int column = 0;

    recalculate_lines(buffer, len, &total_lines);

    int ch;
    while (1) {
        if (window_resized) {
            get_terminal_size();
            window_resized = 0;
        }

        clear_screen();
        printf("\033[107;30m%*sEditing %s%*s\033[0m\n",
               (width - (int)(strlen(argv[1]) + 8)) / 2, "",
               argv[1],
               (width - (int)(strlen(argv[1]) + 8)) / 2, "");

        // Print buffer content
        for (size_t i = 0; i < len; i++) {
            putchar(buffer[i]);
        }

        // Determine cursor position
        int line = get_line_from_cursor(cursor, total_lines);
        int col = cursor - line_offset[line];
        move_cursor(col + 1, line + 2);  // +2 to skip title

        fflush(stdout);

        ch = getchar();
        if (ch == 17) break; // Ctrl+Q

        if (ch == 127) { // Backspace
            if (cursor > 0) {
                memmove(&buffer[cursor - 1], &buffer[cursor], len - cursor);
                cursor--;
                len--;
            }
        } else if (ch == 27) { // Arrow keys
            int seq1 = getchar();
            int seq2 = getchar();
            if (seq1 == '[') {
                int line = get_line_from_cursor(cursor, total_lines);
                int col = cursor - line_offset[line];

                if (seq2 == 'D' && cursor > 0) cursor--;               // Left
                else if (seq2 == 'C' && cursor < len) cursor++;        // Right
                else if (seq2 == 'A' && line > 0) {                    // Up
                    int prev_len = line_offset[line] - line_offset[line - 1];
                    int target = line_offset[line - 1] + col;
                    if (target >= line_offset[line]) target = line_offset[line] - 1;
                    cursor = target;
                } else if (seq2 == 'B' && line < total_lines - 1) {   // Down
                    int next_len = line_offset[line + 1] - line_offset[line];
                    int target = line_offset[line + 1] + col;
                    if (line + 2 < total_lines && target >= line_offset[line + 2])
                        target = line_offset[line + 2] - 1;
                    else if (target > len) target = len;
                    cursor = target;
                }
            }
        } else if ((ch >= 32 && ch <= 126) || ch == 10) { // Printable or newline
            if (len < MAX_BUFFER - 1) {
                memmove(&buffer[cursor + 1], &buffer[cursor], len - cursor);
                buffer[cursor] = ch;
                cursor++;
                len++;
            }
        }

        recalculate_lines(buffer, len, &total_lines);

        // Write changes
        rewind(file);
        fwrite(buffer, 1, len, file);
        fflush(file);
        ftruncate(fileno(file), (off_t)len);
    }

    fclose(file);
    clear_screen();
    return 0;
}

