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

/* Mouse reporting: 1003 = report every event including plain movement,
   1006 = SGR encoding (clean, works on wide screens). Off reverses both. */
#define MOUSE_ON "\x1b[?1003h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1006l\x1b[?1003l"
#define CLEAR_SCREEN "\x1b[2J"
#define RESET_COLOURS "\x1b[0m"

/* Colours (SGR sequences). Each one resets first so attributes don't leak. */
#define COLOUR_HEAD "\x1b[0;1;37m"       /* bold white */
#define COLOUR_TRAIL_NEAR "\x1b[0;1;32m" /* bold (bright) green */
#define COLOUR_TRAIL_MID "\x1b[0;32m"    /* normal green */
#define COLOUR_TRAIL_FAR "\x1b[0;2;32m"  /* dim green */

#define TICK_MS 20  /* one frame of the main loop */
#define SPEED_MIN 1 /* fastest: move every tick */
#define SPEED_MAX 5 /* slowest: move every 5 ticks */
#define TRAIL_MIN 4
#define START_DELAY_MAX 150 /* ticks a column may wait before (re)starting */
#define COLUMN_GAP 2        /* use every 2nd column, like cmatrix; 1 = all */

typedef struct {
  int col;  /* screen column, 1-based */
  int head; /* row of the leading character; may be off-screen */
  int trail_len;
  int speed;   /* move down once every `speed` ticks */
  int counter; /* ticks since the last move */
  int wait;    /* ticks left before this drop starts falling */
} Drop;

static struct termios orig_terminal;
static int raw_enabled = 0;
static int mouse_enabled = 0;

static int rows, cols;
static char *glyphs; /* rows x cols grid of the character on each cell */

static char *frame = NULL;
static size_t frame_len = 0, frame_cap = 0;

static void die(const char *func);

static void frame_append(const char *s, size_t len) {
  if (frame_len + len > frame_cap) {
    size_t new_cap = frame_cap ? frame_cap * 2 : 16384;
    while (new_cap < frame_len + len)
      new_cap *= 2;
    char *p = realloc(frame, new_cap);
    if (!p)
      die("realloc");
    frame = p;
    frame_cap = new_cap;
  }
  memcpy(frame + frame_len, s, len);
  frame_len += len;
}

static void write_str(const char *s) {
  ssize_t r = write(STDOUT_FILENO, s, strlen(s));
  (void)r;
}

/* Undo everything we changed. Must never call die(), because it runs from
   die() and from atexit(). */
static void restore_terminal(void) {
  if (!raw_enabled)
    return;
  raw_enabled = 0;

  if (mouse_enabled) {
    /* Stop mouse reports, then give the terminal a moment so any report
       already on its way arrives before TCSAFLUSH throws pending input away.
       Otherwise stray "\x1b[<35;10;5M" junk can land on the shell prompt. */
    write_str(MOUSE_OFF);
    struct timespec ts = {0, 30 * 1000000L};
    nanosleep(&ts, NULL);
    mouse_enabled = 0;
  }

  write_str(RESET_COLOURS CURSOR_SHOW ALT_SCREEN_OFF);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminal);
}

static void die(const char *func) {
  restore_terminal(); /* back on the normal screen so the error is visible */
  perror(func);
  exit(1);
}

/* Ask the terminal to report mouse clicks, wheel and movement as input. */
static void enable_mouse(void) {
  write_str(MOUSE_ON);
  mouse_enabled = 1;
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

static void get_window_size(int *r, int *c) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    die("ioctl(TIOCGWINSZ)");
  *r = ws.ws_row;
  *c = ws.ws_col;
}

static int rand_between(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

static char random_glyph(void) { return (char)('!' + rand() % 94); }

static char *cell(int row, int col) {
  return &glyphs[(size_t)(row - 1) * cols + (col - 1)];
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
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

/* Give a drop fresh random settings and put it back above the screen. */
static void reset_drop(Drop *d, int max_initial_wait) {
  d->head = 1;
  d->trail_len =
      rand_between(TRAIL_MIN, rows > TRAIL_MIN * 2 ? rows * 2 / 3 : TRAIL_MIN);
  d->speed = rand_between(SPEED_MIN, SPEED_MAX);
  d->counter = 0;
  d->wait = rand_between(0, max_initial_wait);
}

/* Queue the head, trail and trailing blank for one drop. */
static void draw_drop(const Drop *d) {
  if (d->head <= rows) {
    *cell(d->head, d->col) = random_glyph();
    draw_at(d->head, d->col, COLOUR_HEAD, *cell(d->head, d->col));
  }

  for (int dist = 1; dist <= d->trail_len; dist++) {
    int row = d->head - dist;
    if (row >= 1 && row <= rows)
      draw_at(row, d->col, trail_colour(dist, d->trail_len),
              *cell(row, d->col));
  }

  int blank = d->head - d->trail_len - 1;
  if (blank >= 1 && blank <= rows)
    draw_at(blank, d->col, RESET_COLOURS, ' ');
}

/* Advance one drop by one tick; draw it only if it moved. */
static void update_drop(Drop *d) {
  if (d->wait > 0) {
    d->wait--;
    return;
  }

  if (++d->counter < d->speed)
    return;
  d->counter = 0;

  draw_drop(d);
  d->head++;

  /* Whole trail (and the blank after it) has left the screen: start over. */
  if (d->head - d->trail_len - 1 > rows)
    reset_drop(d, START_DELAY_MAX);
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [-s]\n"
          "  -s  screensaver mode: exit on any key press or mouse event\n",
          prog);
}

int main(int argc, char *argv[]) {
  int screensaver = 0;
  int opt;
  while ((opt = getopt(argc, argv, "s")) != -1) {
    switch (opt) {
    case 's':
      screensaver = 1;
      break;
    default:
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (optind < argc) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  /* Options are checked before touching the terminal, so a typo just
     prints the usage message on a normal screen. */
  enable_raw_mode();
  if (screensaver)
    enable_mouse();
  srand((unsigned)time(NULL));
  get_window_size(&rows, &cols);

  glyphs = calloc((size_t)rows * cols, 1);
  int ndrops = (cols + COLUMN_GAP - 1) / COLUMN_GAP;
  Drop *drops = calloc((size_t)ndrops, sizeof(Drop));
  if (!glyphs || !drops)
    die("calloc");

  for (int i = 0; i < ndrops; i++) {
    drops[i].col = 1 + i * COLUMN_GAP;
    reset_drop(&drops[i], START_DELAY_MAX);
  }

  while (1) {
    /* 1. Input. Normal mode: quit on 'q', ignore everything else.
          Screensaver mode: any byte at all (key, Enter, arrow key, mouse
          report) means quit. Drain everything that's waiting either way. */
    char c;
    ssize_t n;
    int quit = 0;
    while ((n = read(STDIN_FILENO, &c, 1)) == 1)
      if (screensaver || c == 'q')
        quit = 1;
    if (n == -1 && errno != EAGAIN)
      die("read");
    if (quit)
      break;

    /* 2. Update every column, collecting all drawing into one frame. */
    frame_len = 0;
    for (int i = 0; i < ndrops; i++)
      update_drop(&drops[i]);

    /* 3. Send the frame in one go. */
    if (frame_len > 0 && write(STDOUT_FILENO, frame, frame_len) == -1)
      die("write");

    sleep_ms(TICK_MS);
  }

  free(drops);
  free(glyphs);
  free(frame);
  return EXIT_SUCCESS; /* atexit -> restore_terminal() */
}
