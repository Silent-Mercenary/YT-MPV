// C version skeleton using ncurses
#include <ncurses.h>
#include <stdlib.h>

int main() {
    initscr();
    printw("Press any key to download video...\n");
    refresh();
    getch();
    endwin();
    
    system("yt-dlp https://www.youtube.com/watch?v=example");
    return 0;
}
