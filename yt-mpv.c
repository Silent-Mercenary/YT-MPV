#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#define MAX_RESULTS 5
#define MAX_URL 256
#define MAX_TITLE 256
#define INITIAL_BUF_SIZE 16384
#define MAX_VIDEOS 50

struct Video {
    char title[MAX_TITLE];
    char url[MAX_URL];
    char video_id[12];
};

enum Quality { POTATO, LOW, MEDIUM, HIGH, NASA };
static enum Quality current_quality = HIGH;

static struct termios orig_termios;

void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void set_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        exit(EXIT_FAILURE);
    }
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }
}

void parse_html(const char *html, struct Video *videos, int *video_count) {
    int count = 0;
    char seen_ids[MAX_VIDEOS][12] = {0};

    const char *pos = html;
    while (count < MAX_VIDEOS && (pos = strstr(pos, "/watch?v=")) != NULL) {
        char video_id[12] = {0};
        strncpy(video_id, pos + 9, 11);
        video_id[11] = '\0';

        int duplicate = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(seen_ids[i], video_id) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            pos += 11;
            continue;
        }
        strcpy(seen_ids[count], video_id);
        snprintf(videos[count].url, MAX_URL, "https://www.youtube.com/watch?v=%s", video_id);
        strcpy(videos[count].video_id, video_id);

        // Try to extract a title from nearby context.
        const char *title_search_start = pos > html + 4000 ? pos - 4000 : html;
        const char *json_start = strstr(title_search_start, "\"title\":{\"runs\":[{\"text\":\"");
        if (json_start && json_start < pos) {
            json_start += strlen("\"title\":{\"runs\":[{\"text\":\"");
            const char *title_end = strchr(json_start, '"');
            if (title_end && title_end > json_start) {
                int title_len = title_end - json_start;
                if (title_len >= MAX_TITLE)
                    title_len = MAX_TITLE - 1;
                if (title_len > 5) { // some minimal length check
                    strncpy(videos[count].title, json_start, title_len);
                    videos[count].title[title_len] = '\0';
                }
            }
        }
        if (videos[count].title[0] != '\0')
            count++;
        else
            videos[count].url[0] = '\0'; // Invalidate if no title found

        pos += 11;
    }
    *video_count = count;
}

void search_youtube(const char *query, struct Video *videos, int *video_count) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {  // Child: execute curl
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            exit(EXIT_FAILURE);
        }
        close(pipefd[1]);

        char formatted_query[256] = {0};
        int i = 0;
        for (const char *q = query; *q && i < sizeof(formatted_query) - 1; q++) {
            formatted_query[i++] = (*q == ' ') ? '+' : *q;
        }
        formatted_query[i] = '\0';

        char url[512];
        snprintf(url, sizeof(url), "https://www.youtube.com/results?search_query=%s", formatted_query);
        execlp("curl", "curl", "-s", "-A", "Mozilla/5.0", url, NULL);
        perror("execlp curl");
        exit(EXIT_FAILURE);
    } else {
        close(pipefd[1]);
        size_t buf_size = INITIAL_BUF_SIZE;
        char *html = malloc(buf_size);
        if (!html) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        size_t total_read = 0;
        ssize_t bytes_read;
        char buffer[4096];
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            if (total_read + bytes_read + 1 > buf_size) {
                buf_size *= 2;
                char *temp = realloc(html, buf_size);
                if (!temp) {
                    free(html);
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
                html = temp;
            }
            memcpy(html + total_read, buffer, bytes_read);
            total_read += bytes_read;
        }
        html[total_read] = '\0';
        close(pipefd[0]);
        wait(NULL);

        if (total_read == 0) {
            fprintf(stderr, "No data received from YouTube. Check your internet connection or curl installation.\n");
            free(html);
            exit(EXIT_FAILURE);
        }
        parse_html(html, videos, video_count);
        free(html);
    }
}

void play_video(const char *url) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork (play_video)");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        const char *quality_flag = NULL;
        switch (current_quality) {
            case POTATO: quality_flag = "--ytdl-format=worst"; break;
            case LOW:    quality_flag = "--ytdl-format=bestvideo[height<=360]+bestaudio/best"; break;
            case MEDIUM: quality_flag = "--ytdl-format=bestvideo[height<=720]+bestaudio/best"; break;
            case HIGH:   quality_flag = "--ytdl-format=bestvideo[height<=2160]+bestaudio/best"; break;
            case NASA:   quality_flag = "--ytdl-format=bestvideo+bestaudio/best"; break;
            default:     quality_flag = "--ytdl-format=best";
        }
        execlp("mpv", "mpv", "--fs", "--quiet", quality_flag, url, NULL);
        perror("execlp mpv");
        exit(EXIT_FAILURE);
    } else {
        wait(NULL);
    }
}

void display_results(const struct Video *videos, int video_count, int offset) {
    system("clear");
    printf("\nSearch Results (Page %d):\n", (offset / MAX_RESULTS) + 1);
    int end = offset + MAX_RESULTS < video_count ? offset + MAX_RESULTS : video_count;
    for (int i = offset; i < end; i++) {
        printf("%d. %s\n   %s\n\n", (i - offset) + 1, videos[i].title, videos[i].url);
    }
    printf("Current quality: %s\n",
           current_quality == POTATO ? "Potato (lowest)" :
           current_quality == LOW ? "Low (360p)" :
           current_quality == MEDIUM ? "Medium (720p)" :
           current_quality == HIGH ? "High (4K)" : "NASA (max)");
    printf("←: previous page, →: next page, [1-%d]: play video, Q: toggle quality, R: reload, 0: exit\n", MAX_RESULTS);
}

int main(void) {
    char query[256];
    struct Video videos[MAX_VIDEOS] = {0};
    int video_count = 0;
    int offset = 0;

    printf("Enter search query: ");
    if (!fgets(query, sizeof(query), stdin)) {
        fprintf(stderr, "Error reading query\n");
        exit(EXIT_FAILURE);
    }
    query[strcspn(query, "\n")] = '\0';

    search_youtube(query, videos, &video_count);
    if (video_count == 0) {
        fprintf(stderr, "No results found. YouTube may have changed its HTML structure.\n");
        return EXIT_FAILURE;
    }

    set_raw_mode();

    int running = 1;
    while (running) {
        display_results(videos, video_count, offset);
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            continue;

        if (c >= '1' && c <= '9') {
            int choice = c - '0';
            if (choice <= MAX_RESULTS && (offset + choice - 1) < video_count) {
                printf("\nLaunching mpv...\n");
                play_video(videos[offset + choice - 1].url);
            }
        } else if (c == '0') {
            running = 0;
        } else if (c == 'q' || c == 'Q') {
            current_quality = (current_quality + 1) % 5;
        } else if (c == 'r' || c == 'R') {
            printf("\nReloading search results...\n");
            video_count = 0;
            search_youtube(query, videos, &video_count);
            offset = 0;
        } else if (c == 27) {  // Escape sequence
            char seq[2];
            if (read(STDIN_FILENO, seq, 2) == 2) {
                if (seq[0] == '[') {
                    if (seq[1] == 'C') {  // Right arrow
                        if (offset + MAX_RESULTS < video_count)
                            offset += MAX_RESULTS;
                    } else if (seq[1] == 'D') {  // Left arrow
                        if (offset >= MAX_RESULTS)
                            offset -= MAX_RESULTS;
                    }
                }
            }
        }
    }
    return 0;
}
