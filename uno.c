#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define MAX_BUFFER 8192

struct termios orig_term;
int width = 80, height = 24;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_term);
    atexit(disable_raw_mode);

    struct termios raw = orig_term;
    raw.c_lflag &= ~(ECHO | ICANON); // Disable echo and canonical mode
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void get_terminal_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
}

void clear_screen() {
    printf("\033[2J\033[H");
}

void move_cursor(int x, int y) {
    printf("\033[%d;%dH", y, x);
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

    int ch;
    while (1) {
        get_terminal_size();
        // Draw UI
        clear_screen();
        printf("\033[107;30m%*sEditing %s%*s\033[0m\n",
               (width - (int)(strlen(argv[1]) + 8)) / 2, "",
               argv[1],
               (width - (int)(strlen(argv[1]) + 8)) / 2, "");
        
        // Print text with line wrapping
        int lines = 1;
        for (size_t i = 0; i < len; i++) {
            putchar(buffer[i]);
            if ((i + 1) % width == 0) lines++;
        }
        if (len % width == 0) lines++; // avoid cursor on next line invisibly
        
        // Position cursor after printed text
        int cursor_row = 2 + (int)(cursor / width);
        int cursor_col = 1 + (int)(cursor % width);
        move_cursor(cursor_col, cursor_row);
        
        fflush(stdout);

        ch = getchar();
        if (ch == 17) break; // Ctrl+Q

        if (ch == 127) { // Backspace
            if (cursor > 0) {
                memmove(&buffer[cursor - 1], &buffer[cursor], len - cursor);
                cursor--;
                len--;
            }
        } else if (ch == 27) { // Escape sequence (arrow keys)
            int seq1 = getchar();
            int seq2 = getchar();
            if (seq1 == '[') {
                if (seq2 == 'C' && cursor < len) cursor++;     // Right
                else if (seq2 == 'D' && cursor > 0) cursor--;  // Left
            }
        } else if (ch >= 32 && ch <= 126) { // Printable characters
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

