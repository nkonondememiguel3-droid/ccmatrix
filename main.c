#define _DEFAULT_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* Mouse reporting: 1003 = report every event including plain movement,
   1006 = SGR encoding (clean, works on wide screens). Off reverses both. */
#define MOUSE_ON "\x1b[?1003h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1006l\x1b[?1003l"

/* SGR intensity codes. */
enum { BOLD = 1, DIM = 2, NORMAL = 22 };

/* SGR foreground colour codes. */
enum {
  FG_BLACK = 30,
  FG_RED,
  FG_GREEN,
  FG_YELLOW,
  FG_BLUE,
  FG_MAGENTA,
  FG_CYAN,
  FG_WHITE,
  FG_DEFAULT = 39
};

static const struct {
  const char *name;
  int fg;
} colour_names[] = {
    {"green", FG_GREEN},     {"red", FG_RED},       {"blue", FG_BLUE},
    {"white", FG_WHITE},     {"yellow", FG_YELLOW}, {"cyan", FG_CYAN},
    {"magenta", FG_MAGENTA}, {"black", FG_BLACK},
};
#define N_COLOURS (sizeof(colour_names) / sizeof(colour_names[0]))

#define UPDATE_DEFAULT 4 /* -u default; tick = update * 10 ms, like cmatrix */
#define UPDATE_MAX 10
#define TICK_MIN_MS 1 /* floor for -u 0, so we don't spin at 100% CPU */
#define SPEED_MIN 1   /* fastest drop: move every tick */
#define SPEED_MAX 3   /* slowest drop: move every 3 ticks */
#define TRAIL_MIN 4
#define START_DELAY_MAX 150 /* ticks a column may wait before (re)starting */
#define COLUMN_GAP 2        /* use every 2nd column, like cmatrix; 1 = all */

/* Half-width katakana: U+FF66 (ｦ) to U+FF9D (ﾝ), one column wide each. */
#define KATAKANA_FIRST 0xFF66
#define KATAKANA_LAST 0xFF9D

typedef struct {
  int col;  /* screen column, 1-based */
  int head; /* row the head is drawn on; 0 = not started yet */
  int trail_len;
  int speed;   /* move down once every `speed` ticks */
  int counter; /* ticks since the last move */
  int wait;    /* ticks left before this drop starts falling */
} Drop;

static struct termios orig_terminal;
static int raw_enabled = 0;
static int mouse_enabled = 0;

static int rows, cols;
static uint16_t *glyphs; /* rows x cols: code point on each cell (0 = none) */
static unsigned char *cell_fgs; /* rows x cols: trail colour of each cell */
static Drop *drops;
static int ndrops;

static int trail_fg = FG_GREEN; /* -C */
static int rainbow = 0;         /* -r */
static int katakana = 0;        /* -c */

/* Set from signal handlers, checked by the main loop. */
static volatile sig_atomic_t got_resize = 0;
static volatile sig_atomic_t got_quit_signal = 0;

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

/* Write all of buf, retrying after partial writes and signal interruptions
   (a resize signal can interrupt write()). Returns 0 or -1. */
static int write_all(const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = write(STDOUT_FILENO, buf, len);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

static void flush_frame(void) {
  if (frame_len > 0 && write_all(frame, frame_len) == -1)
    die("write");
  frame_len = 0;
}

static void write_str(const char *s) { (void)write_all(s, strlen(s)); }

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

/* Returns 0 on success, -1 if the size can't be read or is zero (some
   terminals briefly report 0x0 in the middle of a resize). */
static int get_window_size(int *r, int *c) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 ||
      ws.ws_col == 0)
    return -1;
  *r = ws.ws_row;
  *c = ws.ws_col;
  return 0;
}

static void on_resize(int sig) {
  (void)sig;
  got_resize = 1;
}

static void on_quit_signal(int sig) {
  (void)sig;
  got_quit_signal = 1;
}

static void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: let the signal cut the sleep short */

  sa.sa_handler = on_resize;
  if (sigaction(SIGWINCH, &sa, NULL) == -1)
    die("sigaction");

  /* Closing the window (SIGHUP) or `kill` (SIGTERM) would otherwise skip
     atexit() and leave the terminal raw. Turn them into a clean quit. */
  sa.sa_handler = on_quit_signal;
  if (sigaction(SIGTERM, &sa, NULL) == -1 ||
      sigaction(SIGHUP, &sa, NULL) == -1 || sigaction(SIGINT, &sa, NULL) == -1)
    die("sigaction");
}

static int rand_between(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

static uint16_t random_glyph(void) {
  if (katakana)
    return (uint16_t)rand_between(KATAKANA_FIRST, KATAKANA_LAST);
  return (uint16_t)rand_between('!', '~'); /* printable ASCII */
}

static uint16_t *cell(int row, int col) {
  return &glyphs[(size_t)(row - 1) * cols + (col - 1)];
}

static unsigned char *cell_fg(int row, int col) {
  return &cell_fgs[(size_t)(row - 1) * cols + (col - 1)];
}

/* Encode a code point below U+10000 as UTF-8. Returns the byte count. */
static int utf8_encode(uint16_t cp, char out[3]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL); /* may return early on a signal; that's fine */
}

/* Queue code point cp at (row, col); 1-based coordinates.
   intensity is an SGR code (BOLD, NORMAL or DIM), fg a foreground colour code
   (30-37, or FG_DEFAULT). The leading 0 resets so attributes never leak. */
static void draw_at(int row, int col, int intensity, int fg, uint16_t cp) {
  char buf[48];
  int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[0;%d;%dm", row, col,
                     intensity, fg);
  len += utf8_encode(cp, buf + len);
  frame_append(buf, (size_t)len);
}

/* Pick the trail brightness by distance from the head (1 = just behind it). */
static int trail_intensity(int distance, int trail_len) {
  if (distance <= 2)
    return BOLD;
  if (distance > trail_len - trail_len / 3)
    return DIM;
  return NORMAL;
}

/* Colour for a cell the head has just reached: fixed, or random in -r mode. */
static unsigned char pick_trail_fg(void) {
  if (rainbow)
    return (unsigned char)rand_between(FG_RED, FG_CYAN); /* 31..36 */
  return (unsigned char)trail_fg;
}

/* Give a cell a fresh glyph and colour. */
static void fill_cell(int row, int col) {
  *cell(row, col) = random_glyph();
  *cell_fg(row, col) = pick_trail_fg();
}

/* Give a drop fresh random settings and put it back above the screen. */
static void reset_drop(Drop *d, int max_initial_wait) {
  d->head = 0;
  d->trail_len =
      rand_between(TRAIL_MIN, rows > TRAIL_MIN * 2 ? rows * 2 / 3 : TRAIL_MIN);
  d->speed = rand_between(SPEED_MIN, SPEED_MAX);
  d->counter = 0;
  d->wait = rand_between(0, max_initial_wait);
}

/* Queue the head, trail and trailing blank for one drop at its current
   position. Only rows inside the window are drawn. */
static void render_drop(const Drop *d) {
  if (d->head == 0 || d->col > cols)
    return;

  if (d->head <= rows) {
    if (*cell(d->head, d->col) == 0) /* row that appeared after a resize */
      fill_cell(d->head, d->col);
    draw_at(d->head, d->col, BOLD, FG_WHITE, *cell(d->head, d->col));
  }

  for (int dist = 1; dist <= d->trail_len; dist++) {
    int row = d->head - dist;
    if (row < 1)
      break;
    if (row > rows)
      continue;
    if (*cell(row, d->col) == 0) /* row that appeared after a resize */
      fill_cell(row, d->col);
    draw_at(row, d->col, trail_intensity(dist, d->trail_len),
            *cell_fg(row, d->col), *cell(row, d->col));
  }

  int blank = d->head - d->trail_len - 1;
  if (blank >= 1 && blank <= rows)
    draw_at(blank, d->col, NORMAL, FG_DEFAULT, ' ');
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

  d->head++;
  if (d->head <= rows)
    fill_cell(d->head, d->col);
  render_drop(d);

  /* The blank has just cleared the bottom row: the trail is gone. */
  if (d->head - d->trail_len - 1 >= rows)
    reset_drop(d, START_DELAY_MAX);
}

/* (Re)build the grids and drops for a new window size, keeping as much of
   the running animation as fits. Called once at start (with no old state)
   and again after every resize. */
static void apply_size(int new_rows, int new_cols) {
  uint16_t *new_glyphs = calloc((size_t)new_rows * new_cols, sizeof(uint16_t));
  unsigned char *new_fgs = calloc((size_t)new_rows * new_cols, 1);
  int new_ndrops = (new_cols + COLUMN_GAP - 1) / COLUMN_GAP;
  Drop *new_drops = calloc((size_t)new_ndrops, sizeof(Drop));
  if (!new_glyphs || !new_fgs || !new_drops)
    die("calloc");

  /* Copy the overlapping part of the old grids. */
  if (glyphs) {
    int keep_r = rows < new_rows ? rows : new_rows;
    int keep_c = cols < new_cols ? cols : new_cols;
    for (int r = 0; r < keep_r; r++) {
      memcpy(&new_glyphs[(size_t)r * new_cols], &glyphs[(size_t)r * cols],
             (size_t)keep_c * sizeof(uint16_t));
      memcpy(&new_fgs[(size_t)r * new_cols], &cell_fgs[(size_t)r * cols],
             (size_t)keep_c);
    }
  }

  int keep_drops = drops ? (ndrops < new_ndrops ? ndrops : new_ndrops) : 0;
  if (keep_drops > 0)
    memcpy(new_drops, drops, (size_t)keep_drops * sizeof(Drop));

  free(glyphs);
  free(cell_fgs);
  free(drops);
  glyphs = new_glyphs;
  cell_fgs = new_fgs;
  drops = new_drops;
  ndrops = new_ndrops;
  rows = new_rows;
  cols = new_cols;

  /* Brand-new columns (window got wider) start after a random wait. */
  for (int i = keep_drops; i < ndrops; i++) {
    drops[i].col = 1 + i * COLUMN_GAP;
    reset_drop(&drops[i], START_DELAY_MAX);
  }

  /* Wipe everything (including fragments the terminal may have reflowed)
     and redraw every running trail at once, so nothing flickers away. */
  frame_len = 0;
  frame_append(RESET_COLOURS CLEAR_SCREEN, strlen(RESET_COLOURS CLEAR_SCREEN));
  for (int i = 0; i < ndrops; i++)
    render_drop(&drops[i]);
  flush_frame();
}

static void handle_resize(void) {
  got_resize = 0;
  int r, c;
  if (get_window_size(&r, &c) == -1)
    return; /* transient 0x0: wait for the next SIGWINCH */
  apply_size(r, c);
}

static void usage(const char *prog) {
  fprintf(
      stderr,
      "Usage: %s [-s] [-r] [-c] [-u delay] [-C colour]\n"
      "  -s         screensaver mode: exit on any key press or mouse event\n"
      "  -u delay   update delay, 0 (fastest) to 10 (slowest); default %d\n"
      "             while running, press 0-9 to change it\n"
      "  -C colour  trail colour; default green\n"
      "  -r         rainbow trails (overrides -C)\n"
      "  -c         half-width katakana (needs a UTF-8 terminal)\n",
      prog, UPDATE_DEFAULT);
}

static void print_colour_list(FILE *f) {
  fprintf(f, "Supported colours:");
  for (size_t i = 0; i < N_COLOURS; i++)
    fprintf(f, " %s", colour_names[i].name);
  fprintf(f, "\n");
}

/* Look a colour name up (case-insensitive). Returns its fg code or -1. */
static int parse_colour(const char *s) {
  for (size_t i = 0; i < N_COLOURS; i++)
    if (strcasecmp(s, colour_names[i].name) == 0)
      return colour_names[i].fg;
  return -1;
}

/* Parse a whole-number string in [0, UPDATE_MAX]. Returns -1 if it is empty,
   has trailing junk ("4x", "fast"), overflows, or is out of range. */
static int parse_update(const char *s) {
  char *end;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno == ERANGE || v < 0 || v > UPDATE_MAX)
    return -1;
  return (int)v;
}

static long tick_ms_for(int update) {
  long ms = update * 10L;
  return ms < TICK_MIN_MS ? TICK_MIN_MS : ms;
}

int main(int argc, char *argv[]) {
  int screensaver = 0;
  int update = UPDATE_DEFAULT;
  int opt;
  while ((opt = getopt(argc, argv, "su:C:rc")) != -1) {
    switch (opt) {
    case 's':
      screensaver = 1;
      break;
    case 'r':
      rainbow = 1;
      break;
    case 'c':
      katakana = 1;
      break;
    case 'C':
      trail_fg = parse_colour(optarg);
      if (trail_fg == -1) {
        fprintf(stderr, "%s: unknown colour '%s' for -C\n", argv[0], optarg);
        print_colour_list(stderr);
        return EXIT_FAILURE;
      }
      break;
    case 'u':
      update = parse_update(optarg);
      if (update == -1) {
        fprintf(stderr,
                "%s: invalid delay '%s' for -u: must be a whole number "
                "from 0 to %d\n",
                argv[0], optarg, UPDATE_MAX);
        return EXIT_FAILURE;
      }
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
  int r, c;
  if (get_window_size(&r, &c) == -1) {
    fprintf(stderr,
            "%s: cannot get the terminal size (is stdout a terminal?)\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  enable_raw_mode();
  install_signal_handlers();
  if (screensaver)
    enable_mouse();
  srand((unsigned)time(NULL));
  apply_size(r, c);

  while (!got_quit_signal) {
    /* 1. Resize: rebuild for the new size before drawing anything. */
    if (got_resize)
      handle_resize();

    /* 2. Input. Normal mode: 'q' quits, '0'-'9' set the speed, everything
          else is ignored. Screensaver mode: any byte at all (key, Enter,
          arrow key, mouse report) means quit. Drain everything waiting. */
    char ch;
    ssize_t n;
    int quit = 0;
    while ((n = read(STDIN_FILENO, &ch, 1)) == 1) {
      if (screensaver || ch == 'q')
        quit = 1;
      else if (ch >= '0' && ch <= '9')
        update = ch - '0'; /* takes effect on this very tick */
    }
    if (n == -1 && errno != EAGAIN && errno != EINTR)
      die("read");
    if (quit)
      break;

    /* 3. Update every column, collecting all drawing into one frame. */
    for (int i = 0; i < ndrops; i++)
      update_drop(&drops[i]);
    flush_frame();

    sleep_ms(tick_ms_for(update));
  }

  free(drops);
  free(glyphs);
  free(cell_fgs);
  free(frame);
  return EXIT_SUCCESS; /* atexit -> restore_terminal() */
}
