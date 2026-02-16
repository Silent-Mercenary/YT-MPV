#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>

#define MAX_RESULTS 5
#define MAX_URL 256
#define MAX_TITLE 256
#define INITIAL_BUF_SIZE 16384
#define MAX_VIDEOS 50

#define HISTORY_FILE ".ytplayer_history"
#define LOG_FILE ".ytplayer.log"

struct Video {
    char title[MAX_TITLE];
    char url[MAX_URL];
    char video_id[12];
};

enum Quality { POTATO, LOW, MEDIUM, HIGH, NASA };
static enum Quality current_quality = HIGH;

static struct termios orig_termios;

/* Logging helper functions */
void log_to_file(const char *level, const char *msg) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    fprintf(f, "[%s] %s: %s\n", ctime(&now), level, msg);
    fclose(f);
}

void log_error(const char *msg) {
    fprintf(stderr, "[ERROR] %s: %s\n", msg, strerror(errno));
    log_to_file("ERROR", msg);
}

void log_info(const char *msg) {
    printf("[INFO] %s\n", msg);
    log_to_file("INFO", msg);
}

/* Append search query to history file */
void append_search_history(const char *query) {
    FILE *f = fopen(HISTORY_FILE, "a");
    if (!f) {
        log_error("Failed to open history file");
        return;
    }
    time_t now = time(NULL);
    // Remove the newline from ctime's result
    char *time_str = ctime(&now);
    if(time_str) {
        size_t len = strlen(time_str);
        if (len > 0 && time_str[len-1] == '\n')
            time_str[len-1] = '\0';
    }
    fprintf(f, "[%s] %s\n", time_str, query);
    fclose(f);
}

/* Restore terminal settings on exit. */
void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

/* Set terminal to raw mode for immediate key capture. */
void set_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        log_error("tcgetattr failed");
        exit(EXIT_FAILURE);
    }
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        log_error("tcsetattr failed");
        exit(EXIT_FAILURE);
    }
}

/* Check if an external command is available. */
void check_command(const char *cmd) {
    char command[128];
    snprintf(command, sizeof(command), "command -v %s > /dev/null 2>&1", cmd);
    if (system(command) != 0) {
        fprintf(stderr, "[ERROR] %s is not installed or not in PATH.\n", cmd);
        exit(EXIT_FAILURE);
    }
}

/* Parses HTML to extract video info. */
void parse_html(const char *html, struct Video *videos, int *video_count) {
    int count = 0;
    char seen_ids[MAX_VIDEOS][12] = {0};
    const char *pos = html;

    while (count < MAX_VIDEOS && (pos = strstr(pos, "/watch?v=")) != NULL) {
        if ((pos + 9 + 11) > html + strlen(html)) break;
        char video_id[12] = {0};
        strncpy(video_id, pos + 9, 11);
        video_id[11] = '\0';

        // Check for duplicate video_id.
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
        strncpy(videos[count].video_id, video_id, sizeof(videos[count].video_id) - 1);
        videos[count].video_id[sizeof(videos[count].video_id)-1] = '\0';

        // Look backwards up to 4000 characters for a title marker.
        const char *search_start = pos - 4000;
        if (search_start < html) search_start = html;
        const char *marker = "\"title\":{\"runs\":[{\"text\":\"";
        const char *marker_pos = strstr(search_start, marker);
        if (marker_pos && marker_pos < pos) {
            marker_pos += strlen(marker);
            const char *title_end = strchr(marker_pos, '"');
            if (title_end && title_end > marker_pos) {
                int title_len = title_end - marker_pos;
                if (title_len >= MAX_TITLE)
                    title_len = MAX_TITLE - 1;
                if (title_len > 5) {  // Minimal length check.
                    strncpy(videos[count].title, marker_pos, title_len);
                    videos[count].title[title_len] = '\0';
                    count++;
                }
            }
        }
        pos += 11;
    }
    *video_count = count;
}

/* Fetch YouTube search results via curl. */
void search_youtube(const char *query, struct Video *videos, int *video_count) {
    check_command("curl");

    char info_msg[512];
    snprintf(info_msg, sizeof(info_msg), "Fetching results for \"%s\" ...", query);
    log_info(info_msg);
    printf("%s\n", info_msg);
    fflush(stdout);

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("pipe creation failed");
        exit(EXIT_FAILURE);
    }
    pid_t pid = fork();
    if (pid == -1) {
        log_error("fork failed");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {  // Child: Execute curl.
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            log_error("dup2 failed");
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
            log_error("malloc failed");
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
                    log_error("realloc failed");
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
            fprintf(stderr, "[ERROR] No data received from YouTube. Check your connection or curl installation.\n");
            free(html);
            exit(EXIT_FAILURE);
        }
        parse_html(html, videos, video_count);
        free(html);
    }
}

/* Launch mpv to play the selected video. */
void play_video(const char *url) {
    check_command("mpv");

    pid_t pid = fork();
    if (pid == -1) {
        log_error("fork (play_video) failed");
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
        perror("execlp mpv failed");
        exit(EXIT_FAILURE);
    } else {
        wait(NULL);
    }
}

/* Download video using yt-dlp with current quality presets. */
void download_video(const char *url) {
    check_command("yt-dlp");

    pid_t pid = fork();
    if (pid == -1) {
        log_error("fork (download_video) failed");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        const char *format_option = NULL;
        switch (current_quality) {
            case POTATO: format_option = "worst"; break;
            case LOW:    format_option = "bestvideo[height<=360]+bestaudio/best"; break;
            case MEDIUM: format_option = "bestvideo[height<=720]+bestaudio/best"; break;
            case HIGH:   format_option = "bestvideo[height<=2160]+bestaudio/best"; break;
            case NASA:   format_option = "bestvideo+bestaudio/best"; break;
            default:     format_option = "best";
        }
        execlp("yt-dlp", "yt-dlp", "-f", format_option, "-o", "~/Downloads/%(title)s.%(ext)s", url, NULL);
        perror("execlp yt-dlp failed");
        exit(EXIT_FAILURE);
    } else {
        wait(NULL);
    }
}

/* Display search results in a paginated view. */
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
    printf("←: previous page, →: next page, [1-%d]: play video\n", MAX_RESULTS);
    printf("D: download video, Q: toggle quality, R: reload current search, S: new search, 0: exit\n");
}

int main(void) {
    char query[256];
    struct Video videos[MAX_VIDEOS] = {0};
    int video_count = 0;
    int offset = 0;

    printf("Enter search query: ");
    if (!fgets(query, sizeof(query), stdin)) {
        fprintf(stderr, "[ERROR] Failed to read query.\n");
        exit(EXIT_FAILURE);
    }
    query[strcspn(query, "\n")] = '\0';
    append_search_history(query);

    search_youtube(query, videos, &video_count);
    if (video_count == 0) {
        fprintf(stderr, "[ERROR] No results found. YouTube may have changed its HTML structure.\n");
        exit(EXIT_FAILURE);
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
            if ((offset + choice - 1) < video_count) {
                printf("\nLaunching mpv...\n");
                play_video(videos[offset + choice - 1].url);
            }
        } else if (c == '0') {
            running = 0;
        } else if (c == 'q' || c == 'Q') {
            current_quality = (current_quality + 1) % 5;
        } else if (c == 'r' || c == 'R') {
            printf("\nReloading search results for \"%s\"...\n", query);
            video_count = 0;
            search_youtube(query, videos, &video_count);
            offset = 0;
        } else if (c == 's' || c == 'S') {
            restore_terminal();  // Exit raw mode temporarily.
            printf("\nEnter new search query: ");
            if (!fgets(query, sizeof(query), stdin)) {
                fprintf(stderr, "[ERROR] Failed to read query.\n");
                exit(EXIT_FAILURE);
            }
            query[strcspn(query, "\n")] = '\0';
            append_search_history(query);
            search_youtube(query, videos, &video_count);
            offset = 0;
            set_raw_mode();  // Re-enable raw mode.
        } else if (c == 'd' || c == 'D') {
            // Use first video in current page for download (can be enhanced to allow selection)
            int choice = 1;
            if ((offset + choice - 1) < video_count) {
                printf("\nDownloading video...\n");
                download_video(videos[offset + choice - 1].url);
            }
        } else if (c == 27) {  // Handle escape sequence for arrow keys.
            char seq[2];
            if (read(STDIN_FILENO, seq, 2) == 2) {
                if (seq[0] == '[') {
                    if (seq[1] == 'C' && (offset + MAX_RESULTS) < video_count)
                        offset += MAX_RESULTS;
                    else if (seq[1] == 'D' && offset >= MAX_RESULTS)
                        offset -= MAX_RESULTS;
                }
            }
        }
    }
    return 0;
}
