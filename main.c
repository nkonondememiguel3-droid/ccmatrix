#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ALT_SCREEN_ON "\x1b[?1049h"
#define ALT_SCREEN_OFF "\x1b[?1049l"
#define CURSOR_HIDE "\x1b[?25l"
#define CURSOR_SHOW "\x1b[?25h"
#define CLEAR_SCREEN "\x1b[2J"
#define RESET_COLOURS "\x1b[0m"

#define FRAME_DELAY_MS 60

static struct termios orig_terminal;
static int raw_enabled = 0;

static void write_str(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }

static void restore_terminal(void) {
  if (!raw_enabled)
    return;
  raw_enabled = 0;
  write_str(RESET_COLOURS CURSOR_SHOW ALT_SCREEN_OFF);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal);
}

static void die(const char *func) {
  restore_terminal(); /* back on the normal screen so the error is visible */
  perror(func);
  exit(1);
}

static void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &orig_terminal) == -1)
    die("tcgetattr");
  atexit(restore_terminal);

  struct termios raw = orig_terminal;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); /* local flags */
  raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
  raw_enabled = 1;

  /* Switch to the alternate screen so the user's screen comes back on exit. */
  write_str(ALT_SCREEN_ON CURSOR_HIDE CLEAR_SCREEN);
}

static void get_window_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    die("ioctl(TIOCGWINSZ)");
  *rows = ws.ws_row;
  *cols = ws.ws_col;
}

/* Draw c at (row, col); both are 1-based, as the terminal expects. */
static void draw_at(int row, int col, char c) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH%c", row, col, c);
  if (write(STDOUT_FILENO, buf, len) == -1)
    die("write");
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

int main(void) {
  enable_raw_mode();
  srand((unsigned)time(NULL));

  int rows, cols;
  get_window_size(&rows, &cols);

  int col = rand() % cols + 1;
  int row = 1;

  while (1) {
    /* 1. Input: quit on 'q', ignore every other key. */
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == -1 && errno != EAGAIN)
      die("read");
    if (n == 1 && c == 'q')
      break;

    /* 2. Update: erase the old position, draw the new one. */
    if (row > 1)
      draw_at(row - 1, col, ' ');
    draw_at(row, col, 'm');

    /* 3. Move down; when past the bottom, start again in a new column. */
    row++;
    if (row > rows) {
      draw_at(rows, col, ' ');
      row = 1;
      col = rand() % cols + 1;
    }

    sleep_ms(FRAME_DELAY_MS);
  }

  return EXIT_SUCCESS; /* atexit -> restore_terminal() */
}
