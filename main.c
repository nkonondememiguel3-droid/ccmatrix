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

#define COLOUR_HEAD "\x1b[0;1;37m"       /* bold white */
#define COLOUR_TRAIL_NEAR "\x1b[0;1;32m" /* bold (bright) green */
#define COLOUR_TRAIL_MID "\x1b[0;32m"    /* normal green */
#define COLOUR_TRAIL_FAR "\x1b[0;2;32m"  /* dim green */

#define FRAME_DELAY_MS 60
#define TRAIL_MIN 6
#define TRAIL_MAX 16

static struct termios orig_terminal;
static int raw_enabled = 0;

/* buffer: build a whole frame, send it in one write */
static char frame[8192];
static size_t frame_len = 0;

static void frame_append(const char *s, size_t len) {
  if (frame_len + len > sizeof(frame))
    return; /* frame too big; drop the rest rather than overflow */
  memcpy(frame + frame_len, s, len);
  frame_len += len;
}

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
  raw.c_cc[VMIN] = 0; /* read() never blocks: returns 0 if no key */
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

/* Queue character c at (row, col) in the given colour; 1-based coordinates. */
static void draw_at(int row, int col, const char *colour, char c) {
  char buf[48];
  int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH%s%c", row, col, colour, c);
  frame_append(buf, (size_t)len);
}

/* Pick the trail colour by distance from the head (1 = just behind it). */
static const char *trail_colour(int distance, int trail_len) {
  if (distance <= 2)
    return COLOUR_TRAIL_NEAR;
  if (distance > trail_len - trail_len / 3)
    return COLOUR_TRAIL_FAR;
  return COLOUR_TRAIL_MID;
}

static char random_glyph(void) { return (char)('!' + rand() % 94); }

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

int main(void) {
  enable_raw_mode();
  srand((unsigned)time(NULL));

  int rows, cols;
  get_window_size(&rows, &cols);

  char *glyphs = calloc((size_t)rows + 1, 1);
  if (!glyphs)
    die("calloc");

  int col = rand() % cols + 1;
  int head = 1;
  int trail_len = TRAIL_MIN + rand() % (TRAIL_MAX - TRAIL_MIN + 1);

  while (1) {

    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == -1 && errno != EAGAIN)
      die("read");
    if (n == 1 && c == 'q')
      break;

    /* Draw the head, the trail behind it, and blank the cell after it. */
    frame_len = 0;

    if (head <= rows) {
      glyphs[head] = random_glyph();
      draw_at(head, col, COLOUR_HEAD, glyphs[head]);
    }

    for (int d = 1; d <= trail_len; d++) {
      int row = head - d;
      if (row >= 1 && row <= rows)
        draw_at(row, col, trail_colour(d, trail_len), glyphs[row]);
    }

    int tail_end = head - trail_len - 1;
    if (tail_end >= 1 && tail_end <= rows)
      draw_at(tail_end, col, RESET_COLOURS, ' ');

    if (write(STDOUT_FILENO, frame, frame_len) == -1)
      die("write");

    /* Move down. Keep going until the whole trail has left the screen,
          then start a new drop in a new column. */
    head++;
    if (head - trail_len - 1 > rows) {
      head = 1;
      col = rand() % cols + 1;
      trail_len = TRAIL_MIN + rand() % (TRAIL_MAX - TRAIL_MIN + 1);
    }

    sleep_ms(FRAME_DELAY_MS);
  }

  free(glyphs);
  return EXIT_SUCCESS;
}
