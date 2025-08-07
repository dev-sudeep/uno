#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>

#define MAX_BUFFER 16384
#define MAX_LINE_LEN 1024

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

int is_keyword(const char* word) {
    const char* keywords[] = {
        "int", "return", "if", "else", "while", "for", "void", "char",
        "float", "double", "struct", "break", "continue", "switch", "case",
        "#include", "#define", "const", "static", "sizeof", NULL
    };
    for (int i = 0; keywords[i]; i++) {
        if (strcmp(word, keywords[i]) == 0) return 1;
    }
    return 0;
}

void print_with_syntax(const char* line, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (isalpha(line[i]) || line[i] == '#' || line[i] == '_') {
            char word[64] = {0};
            size_t j = 0;
            while ((isalnum(line[i]) || line[i] == '_' || line[i] == '#') && j < sizeof(word) - 1)
                word[j++] = line[i++];
            word[j] = '\0';
            if (is_keyword(word))
                printf("\033[1;34m%s\033[0m", word);  // Blue keywords
            else
                printf("%s", word);
        } else if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i++];
            printf("\033[0;32m%c", quote);  // Green strings
            while (i < len && line[i] != quote) {
                putchar(line[i++]);
            }
            if (i < len) printf("%c\033[0m", line[i++]);
        } else {
            putchar(line[i++]);
        }
    }
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
    int logical_x = 0, logical_y = 0;

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

        size_t line_start = 0;
        int row = 2, col = 1;
        for (size_t i = 0; i < len;) {
            size_t line_len = 0;
            while (i + line_len < len && buffer[i + line_len] != '\n') line_len++;
            move_cursor(1, row++);
            print_with_syntax(&buffer[i], line_len);
            i += line_len;
            if (i < len && buffer[i] == '\n') {
                i++;
                putchar('\n');
            }
        }

        // Calculate cursor position
        size_t cx = 0, cy = 2;
        for (size_t i = 0; i < cursor; i++) {
            if (buffer[i] == '\n') {
                cy++;
                cx = 0;
            } else {
                cx++;
                if (cx >= (size_t)width) {
                    cx = 0;
                    cy++;
                }
            }
        }
        move_cursor(cx + 1, cy);
        fflush(stdout);

        ch = getchar();
        if (ch == 17) break; // Ctrl+Q

        if (ch == 127) { // Backspace
            if (cursor > 0) {
                memmove(&buffer[cursor - 1], &buffer[cursor], len - cursor);
                cursor--;
                len--;
            }
        } else if (ch == 10) { // Enter
            if (len < MAX_BUFFER - 1) {
                memmove(&buffer[cursor + 1], &buffer[cursor], len - cursor);
                buffer[cursor++] = '\n';
                len++;
            }
        } else if (ch == 27) { // Arrow keys
            int seq1 = getchar();
            int seq2 = getchar();
            if (seq1 == '[') {
                if (seq2 == 'C' && cursor < len) cursor++;       // Right
                else if (seq2 == 'D' && cursor > 0) cursor--;    // Left
                else if (seq2 == 'A') { // Up
                    size_t temp = cursor;
                    while (temp > 0 && buffer[temp - 1] != '\n') temp--;
                    if (temp > 0) {
                        size_t prev_line_start = temp - 1;
                        while (prev_line_start > 0 && buffer[prev_line_start - 1] != '\n') prev_line_start--;
                        size_t offset = cursor - temp;
                        cursor = prev_line_start;
                        while (cursor < temp - 1 && offset--) cursor++;
                    }
                } else if (seq2 == 'B') { // Down
                    size_t temp = cursor;
                    while (temp < len && buffer[temp] != '\n') temp++;
                    if (temp < len) {
                        size_t next_line_start = temp + 1;
                        size_t offset = cursor - (cursor > 0 && buffer[cursor - 1] == '\n' ? cursor - 1 : cursor);
                        cursor = next_line_start;
                        while (cursor < len && buffer[cursor] != '\n' && offset--) cursor++;
                    }
                }
            }
        } else if (ch >= 32 && ch <= 126) { // Printable characters
            if (len < MAX_BUFFER - 1) {
                memmove(&buffer[cursor + 1], &buffer[cursor], len - cursor);
                buffer[cursor++] = ch;
                len++;
            }
        }

        rewind(file);
        fwrite(buffer, 1, len, file);
        fflush(file);
        ftruncate(fileno(file), (off_t)len);
    }

    fclose(file);
    clear_screen();
    return 0;
}

