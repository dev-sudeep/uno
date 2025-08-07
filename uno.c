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
    raw.c_lflag &= ~(ECHO | ICANON); // Disable echo and canonical mode
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

// Draws buffer and computes screen position of the cursor
void draw_buffer(const char *buffer, size_t len, const char *filename, size_t cursor_index) {
    clear_screen();
    printf("\033[107;30m%*sEditing %s%*s\033[0m\n",
           (width - (int)(strlen(filename) + 8)) / 2, "",
           filename,
           (width - (int)(strlen(filename) + 8)) / 2, "");

    size_t visual_row = 2;
    size_t visual_col = 1;
    size_t cursor_row = 2, cursor_col = 1;

    for (size_t i = 0; i < len; ++i) {
        if (i == cursor_index) {
            cursor_row = visual_row;
            cursor_col = visual_col;
        }

        char c = buffer[i];
        if (c == '\n') {
            putchar('\n');
            visual_row++;
            visual_col = 1;
        } else {
            putchar(c);
            visual_col++;
            if (visual_col > width) {
                visual_col = 1;
                visual_row++;
            }
        }
    }

    if (cursor_index == len) {
        cursor_row = visual_row;
        cursor_col = visual_col;
    }

    move_cursor(cursor_col, cursor_row);
    fflush(stdout);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: uno <filename>\n");
        return 1;
    }

    enable_raw_mode();
    get_terminal_size();

    FILE* file = fopen(argv[1], "r+");
    if (!file) file = fopen(argv[1], "w+");
    if (!file) {
        perror("Failed to open file");
        return 1;
    }

    char buffer[MAX_BUFFER] = {0};
    size_t len = fread(buffer, 1, MAX_BUFFER - 1, file);
    size_t cursor = len;

    signal(SIGWINCH, handle_winch);

    int ch;
    while (1) {
        if (window_resized) {
            get_terminal_size();
            window_resized = 0;
        }

        draw_buffer(buffer, len, argv[1], cursor);

        ch = getchar();
        if (ch == 17) break; // Ctrl+Q

        if (ch == 127 || ch == 8) { // Backspace
            if (cursor > 0) {
                memmove(&buffer[cursor - 1], &buffer[cursor], len - cursor);
                cursor--;
                len--;
            }
        } else if (ch == 27) { // Arrow keys
            int seq1 = getchar();
            int seq2 = getchar();
            if (seq1 == '[') {
                if (seq2 == 'C' && cursor < len) {
                    cursor++; // Right
                } else if (seq2 == 'D' && cursor > 0) {
                    cursor--; // Left
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

        // Write buffer to file
        rewind(file);
        fwrite(buffer, 1, len, file);
        fflush(file);
        ftruncate(fileno(file), (off_t)len);
    }

    fclose(file);
    clear_screen();
    return 0;
}

