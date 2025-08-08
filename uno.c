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

typedef enum {
    FILE_TYPE_TEXT,
    FILE_TYPE_C,
    FILE_TYPE_PYTHON,
    FILE_TYPE_BASH,
    FILE_TYPE_UNKNOWN
} FileType;

FileType detect_file_type(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return FILE_TYPE_TEXT;
    
    ext++; // Skip the dot
    if (strcasecmp(ext, "c") == 0 || strcasecmp(ext, "h") == 0 ||
        strcasecmp(ext, "cpp") == 0 || strcasecmp(ext, "hpp") == 0)
        return FILE_TYPE_C;
    else if (strcasecmp(ext, "py") == 0)
        return FILE_TYPE_PYTHON;
    else if (strcasecmp(ext, "sh") == 0 || strcasecmp(ext, "bash") == 0)
        return FILE_TYPE_BASH;
    
    // Check if file is binary
    FILE* fp = fopen(filename, "rb");
    if (fp) {
        unsigned char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == 0) return FILE_TYPE_UNKNOWN;
        }
    }
    
    return FILE_TYPE_TEXT;
}

void print_plain_text(const char* line, size_t len) {
    printf("%.*s", (int)len, line);
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

void print_with_syntax(const char* line, size_t len, FileType type) {
    if (type == FILE_TYPE_TEXT || type == FILE_TYPE_UNKNOWN) {
        print_plain_text(line, len);
        return;
    }
    
    size_t i = 0;
    while (i < len) {
        if (isalpha(line[i]) || line[i] == '#' || line[i] == '_') {
            // Find word boundary
            size_t word_end = i;
            while (word_end < len && (isalnum(line[word_end]) || line[word_end] == '_' || line[word_end] == '#'))
                word_end++;
            
            // Extract and print complete word
            char word[64] = {0};
            size_t word_len = word_end - i;
            if (word_len < sizeof(word) - 1) {
                strncpy(word, &line[i], word_len);
                word[word_len] = '\0';
                if (is_keyword(word))
                    printf("\033[1;34m%s\033[0m", word);
                else
                    printf("%s", word);
            }
            i = word_end;
        } else if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i++];
            printf("\033[0;32m%c", quote);
            size_t str_start = i;
            while (i < len && line[i] != quote) {
                i++;
            }
            printf("%.*s", (int)(i - str_start), &line[str_start]);
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

    FileType file_type = detect_file_type(argv[1]);

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
            print_with_syntax(&buffer[i], line_len, file_type);
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
                if (cursor > len) cursor = len;  // Safety check
                memmove(&buffer[cursor + 1], &buffer[cursor], len - cursor);
                buffer[cursor++] = ch;
                len++;
                buffer[len] = '\0';  // Ensure null termination
            }
        }

        if (cursor > len) cursor = len;  // Maintain cursor bounds
        rewind(file);
        fwrite(buffer, 1, len, file);
        fflush(file);
        ftruncate(fileno(file), (off_t)len);
    }

    fclose(file);
    clear_screen();
    return 0;
}

