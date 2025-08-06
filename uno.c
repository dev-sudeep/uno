#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

void clear(){
	printf("\033[2J\033[H");
}

// Function to fetch terminal dimensions via `stty size`
// Returns 0 on success, non-zero on error
int get_terminal_size(int* rows, int* cols) {
    if (!rows || !cols) {
        fprintf(stderr, "[Error] Null pointer passed to get_terminal_size()\n");
        return 1;
    }

    *rows = 0;
    *cols = 0;

    FILE* sizefetch = popen("stty size", "r");
    if (!sizefetch) {
        perror("[Error] Failed to run 'stty size' using popen");
        fprintf(stderr,
            "[Hint] Make sure you're running this inside a terminal (TTY environment)\n"
            "[Hint] Ensure the 'stty' command is available and not removed\n"
        );
        return 2;
    }

    // Try parsing the two integers
    if (fscanf(sizefetch, "%d %d", rows, cols) != 2) {
        fprintf(stderr,
            "[Error] Failed to parse output from 'stty size'\n"
            "[Hint] Expected format: '<rows> <cols>' but parsing failed\n"
            "[Hint] Actual output: ");

        // Rewind and print actual output for debugging
        rewind(sizefetch);
        int ch;
        while ((ch = fgetc(sizefetch)) != EOF) {
            fputc(ch, stderr);
        }
        fputc('\n', stderr);

        pclose(sizefetch);
        return 3;
    }

    pclose(sizefetch);

    if (*rows <= 0 || *cols <= 0) {
        fprintf(stderr,
            "[Error] Parsed terminal size is invalid: %d rows, %d cols\n"
            "[Hint] This might happen if stdin/stdout is not connected to a real terminal\n",
            *rows, *cols
        );
        return 4;
    }

    return 0;
}


int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: uno (filename)\n");
        return 0;
    }

    FILE* f = fopen(argv[1], "r");
    if (!f) {
        printf("File not found. Exiting...\n");
        return 1;
    }
    fclose(f);

    int height = 0, width = 0;

    if (get_terminal_size(&height, &width) != 0) {
        fprintf(stderr, "[Fatal] Unable to determine terminal size. Exiting.\n");
        return 1;
    }
    

    clear();

    printf("\033[107;30m%*sEditing %s%*s\033[0m\n", (width -(strlen(argv[1]) +(strlen("Editing "))))/2, "\x20",argv[1], (width -(strlen(argv[1]) + (strlen("Editing "))))/2, "\x20");

        struct termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);        // save old settings
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);      // disable canonical mode and echo
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
        FILE *out = fopen("output.txt", "w");
        if (!out) {
            perror("file");
            return 1;
        }
    
        int ch;
        while ((ch = getchar()) != EOF) {
            fputc(ch, out);
            fflush(out);
            if (ch == '\x11') break;  // exit on 'ctrl + q'
        }
    
        fclose(out);
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // restore settings
    

    return 0;
}

