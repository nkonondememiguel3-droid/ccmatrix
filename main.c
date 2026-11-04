/* ccmatrix - a cmatrix clone with message mode.
 *
 * AUTHOR: NKONO NDEME Miguel
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "text.h"

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

#define TICK_MIN_MS 1 /* floor for -u 0, so we don't spin at 100% CPU */
#define SPEED_MIN 1   /* fastest drop: move every tick */
#define SPEED_MAX 3   /* slowest drop: move every 3 ticks */
#define TRAIL_MIN 4
#define START_DELAY_MAX 150 /* ticks a column may wait before (re)starting */
#define COLUMN_GAP 2        /* use every 2nd column, like cmatrix; 1 = all */

/* Half-width katakana: U+FF66 (ｦ) to U+FF9D (ﾝ), one column wide each. */
#define KATAKANA_FIRST 0xFF66
#define KATAKANA_LAST 0xFF9D

/* Screensaver: ignore input this long after starting, so the key press or
   mouse nudge that launched us doesn't immediately close us again. */
#define SCREENSAVER_GRACE_MS 500

/* Message mode timings (the rest come from Options). */
#define MSG_REVEAL_MAX_MS 4000 /* force any unrevealed letters after this */
#define MSG_DISSOLVE_MS 1500   /* letters drop back into the rain over this */
#define MSG_KICK_WAIT_MAX 15   /* idle columns over the text start this soon */

typedef struct {
  int col;  /* screen column, 1-based */
  int head; /* row the head is drawn on; 0 = not started yet */
  int trail_len;
  int speed;   /* move down once every `speed` ticks */
  int counter; /* ticks since the last move */
  int wait;    /* ticks left before this drop starts falling */
} Drop;

/* One character of the message, placed on screen. */
typedef struct {
  int row, col, width;
  uint32_t cp;
  int revealed;    /* currently shown, blocking the rain */
  int gone;        /* already dissolved; don't reveal again */
  long release_at; /* ms into the dissolve phase when it disappears */
} MsgGlyph;

typedef enum { PH_RAIN, PH_REVEAL, PH_HOLD, PH_DISSOLVE } Phase;

static Options opt;

static struct termios orig_terminal;
static int raw_enabled = 0;
static int mouse_enabled = 0;

static int rows, cols;
static uint16_t *glyphs; /* rows x cols: code point on each cell (0 = none) */
static unsigned char *cell_fgs; /* rows x cols: trail colour of each cell */
static Drop *drops;
static int ndrops;

/* Message mode state. */
static int msg_enabled = 0;
static TextFile msg_text;
static size_t msg_line = 0;
static Phase phase = PH_RAIN;
static long phase_start = 0;
static MsgGlyph *mg = NULL;
static int mg_count = 0, mg_cap = 0, mg_unrevealed = 0;
static int *msg_map = NULL; /* rows x cols: index into mg, or -1 */

/* Set from signal handlers, checked by the main loop. */
static volatile sig_atomic_t got_resize = 0;
static volatile sig_atomic_t got_quit_signal = 0;

static char *frame = NULL;
static size_t frame_len = 0, frame_cap = 0;

static void die(const char *func);

/* ---------- output ----------------------------------------------------- */

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

/* ---------- terminal setup / teardown ---------------------------------- */

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

/* ---------- signals ---------------------------------------------------- */

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

/* ---------- helpers ---------------------------------------------------- */

static int rand_between(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static uint16_t random_glyph(void) {
  if (opt.katakana)
    return (uint16_t)rand_between(KATAKANA_FIRST, KATAKANA_LAST);
  return (uint16_t)rand_between('!', '~'); /* printable ASCII */
}

static uint16_t *cell(int row, int col) {
  return &glyphs[(size_t)(row - 1) * cols + (col - 1)];
}

static unsigned char *cell_fg(int row, int col) {
  return &cell_fgs[(size_t)(row - 1) * cols + (col - 1)];
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL); /* may return early on a signal; that's fine */
}

/* ---------- drawing ---------------------------------------------------- */

/* Queue code point cp at (row, col); 1-based coordinates.
   intensity is an SGR code (BOLD, NORMAL or DIM), fg a foreground colour code
   (30-37, or FG_DEFAULT). The leading 0 resets so attributes never leak. */
static void draw_at(int row, int col, int intensity, int fg, uint32_t cp) {
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
  if (opt.rainbow)
    return (unsigned char)rand_between(FG_RED, FG_CYAN); /* 31..36 */
  return (unsigned char)opt.trail_fg;
}

/* Give a cell a fresh glyph and colour. */
static void fill_cell(int row, int col) {
  *cell(row, col) = random_glyph();
  *cell_fg(row, col) = pick_trail_fg();
}

/* ---------- message mode ----------------------------------------------- */

/* True if a resolved message letter is sitting on this cell; the rain must
   not draw there or it would break the letter (or half of a wide one). */
static int msg_covers(int row, int col) {
  if (!msg_map)
    return 0;
  int idx = msg_map[(size_t)(row - 1) * cols + (col - 1)];
  return idx >= 0 && mg[idx].revealed;
}

static void msg_clear(void) {
  mg_count = 0;
  mg_unrevealed = 0;
  if (msg_map)
    for (size_t i = 0; i < (size_t)rows * cols; i++)
      msg_map[i] = -1;
}

static void msg_add(int row, int col, int width, uint32_t cp) {
  if (col < 1 || col + width - 1 > cols || row < 1 || row > rows)
    return;
  if (mg_count == mg_cap) {
    int new_cap = mg_cap ? mg_cap * 2 : 64;
    MsgGlyph *p = realloc(mg, (size_t)new_cap * sizeof(MsgGlyph));
    if (!p)
      die("realloc");
    mg = p;
    mg_cap = new_cap;
  }
  MsgGlyph *g = &mg[mg_count];
  g->row = row;
  g->col = col;
  g->width = width;
  g->cp = cp;
  g->revealed = 0;
  g->gone = 0;
  g->release_at = 0;
  for (int c = col; c < col + width; c++)
    msg_map[(size_t)(row - 1) * cols + (c - 1)] = mg_count;
  mg_count++;
  mg_unrevealed++;
}

/* Lay the current message line out in the middle of the screen, wrapping
   it to the window width. Japanese has no spaces, so it is broken between
   any two characters; text with spaces is broken at the last space. Lines
   that don't fit vertically are dropped. Nothing is drawn yet. */
static void msg_layout(void) {
  msg_clear();
  const TextLine *line = &msg_text.lines[msg_line];
  const uint32_t *cp = line->cps;
  size_t n = line->len;
  int max_w = cols > 4 ? cols - 2 : cols;

  /* First pass: find where each screen row starts and ends. */
  enum { MAX_ROWS = 256 };
  size_t starts[MAX_ROWS], ends[MAX_ROWS];
  int widths[MAX_ROWS];
  int nrows = 0;

  size_t i = 0;
  while (i < n && nrows < rows && nrows < MAX_ROWS) {
    while (i < n && cp[i] == ' ')
      i++; /* no leading spaces on a wrapped row */
    if (i >= n)
      break;

    size_t start = i, last_space = 0;
    int w = 0, have_space = 0;
    while (i < n) {
      int cw = char_width(cp[i]);
      if (w + cw > max_w)
        break;
      if (cp[i] == ' ') {
        last_space = i;
        have_space = 1;
      }
      w += cw;
      i++;
    }

    size_t end = i;
    if (i < n && have_space && last_space > start) {
      end = last_space; /* break at the space instead of mid-word */
      i = last_space + 1;
    }
    if (end == start) { /* a single character wider than the window */
      end = start + 1;
      i = start + 1;
    }

    while (end > start && cp[end - 1] == ' ')
      end--; /* trailing spaces don't count when centring */
    int row_w = 0;
    for (size_t k = start; k < end; k++)
      row_w += char_width(cp[k]);

    starts[nrows] = start;
    ends[nrows] = end;
    widths[nrows] = row_w;
    nrows++;
  }

  /* Second pass: place each character, centred. */
  int top = (rows - nrows) / 2 + 1;
  for (int r = 0; r < nrows; r++) {
    int col = (cols - widths[r]) / 2 + 1;
    /* Start on a rain column so wide letters line up with the drops. */
    if (COLUMN_GAP == 2 && col % 2 == 0 && col > 1)
      col--;
    for (size_t k = starts[r]; k < ends[r]; k++) {
      int w = char_width(cp[k]);
      if (w == 0)
        continue;
      msg_add(top + r, col, w, cp[k]);
      col += w;
    }
  }
}

static void msg_draw_glyph(int i) {
  draw_at(mg[i].row, mg[i].col, BOLD, opt.message_fg, mg[i].cp);
}

static void msg_reveal(int i) {
  if (mg[i].revealed || mg[i].gone)
    return;
  mg[i].revealed = 1;
  mg_unrevealed--;
  msg_draw_glyph(i);
}

/* Remove a letter, blanking every cell it covered, and hand those cells
   back to the rain. */
static void msg_dissolve_glyph(int i) {
  for (int c = mg[i].col; c < mg[i].col + mg[i].width; c++)
    draw_at(mg[i].row, c, NORMAL, FG_DEFAULT, ' ');
  mg[i].revealed = 0;
  mg[i].gone = 1;
}

/* Reveal the letter under a drop's head, if any. A drop "owns" its column
   and the gap column(s) to its right, so every letter is reachable. */
static void msg_reveal_under(const Drop *d) {
  if (phase != PH_REVEAL || d->head < 1 || d->head > rows)
    return;
  for (int c = d->col; c < d->col + COLUMN_GAP && c <= cols; c++) {
    int idx = msg_map[(size_t)(d->head - 1) * cols + (c - 1)];
    if (idx >= 0)
      msg_reveal(idx);
  }
}

static void msg_start_reveal(void) {
  msg_layout();
  if (mg_count == 0)
    return;

  /* Columns over the text that are idle get started soon, so the letters
     are revealed by falling drops rather than the timeout. */
  int min_c = cols, max_c = 1;
  for (int i = 0; i < mg_count; i++) {
    if (mg[i].col < min_c)
      min_c = mg[i].col;
    if (mg[i].col + mg[i].width - 1 > max_c)
      max_c = mg[i].col + mg[i].width - 1;
  }
  for (int i = 0; i < ndrops; i++) {
    Drop *d = &drops[i];
    if (d->col + COLUMN_GAP - 1 >= min_c && d->col <= max_c && d->head == 0 &&
        d->wait > MSG_KICK_WAIT_MAX)
      d->wait = rand_between(0, MSG_KICK_WAIT_MAX);
  }
}

static void set_phase(Phase p, long now) {
  phase = p;
  phase_start = now;
}

/* Advance the message state machine:
   RAIN -> REVEAL (drops uncover letters) -> HOLD -> DISSOLVE -> RAIN. */
static void msg_tick(long now) {
  long elapsed = now - phase_start;

  switch (phase) {
  case PH_RAIN:
    if (elapsed >= opt.message_interval_ms) {
      msg_start_reveal();
      set_phase(PH_REVEAL, now);
    }
    break;

  case PH_REVEAL:
    if (mg_unrevealed == 0 || elapsed >= MSG_REVEAL_MAX_MS) {
      for (int i = 0; i < mg_count; i++)
        msg_reveal(i);
      set_phase(PH_HOLD, now);
    }
    break;

  case PH_HOLD:
    if (elapsed >= opt.message_hold_ms) {
      for (int i = 0; i < mg_count; i++)
        mg[i].release_at = rand_between(0, MSG_DISSOLVE_MS);
      set_phase(PH_DISSOLVE, now);
    }
    break;

  case PH_DISSOLVE: {
    int remaining = 0;
    for (int i = 0; i < mg_count; i++) {
      if (!mg[i].revealed)
        continue;
      if (elapsed >= mg[i].release_at)
        msg_dissolve_glyph(i);
      else
        remaining++;
    }
    if (remaining == 0) {
      msg_clear();
      msg_line = (msg_line + 1) % msg_text.count;
      set_phase(PH_RAIN, now);
    }
    break;
  }
  }
}

/* After a resize: lay the current message out again for the new size. */
static void msg_on_resize(void) {
  if (phase == PH_RAIN)
    return;
  if (phase == PH_DISSOLVE) { /* nearly done anyway: skip to the next line */
    msg_clear();
    msg_line = (msg_line + 1) % msg_text.count;
    set_phase(PH_RAIN, now_ms());
    return;
  }
  msg_layout();
  if (phase == PH_HOLD) {
    for (int i = 0; i < mg_count; i++)
      mg[i].revealed = 1; /* drawn by apply_size() after the clear */
    mg_unrevealed = 0;
  }
}

/* ---------- rain ------------------------------------------------------- */

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
   position. Only rows inside the window, and not under a message letter,
   are drawn. With flicker set, some trail cells change character and flash
   bright for this one frame. */
static void render_drop(const Drop *d, int flicker) {
  if (d->head == 0 || d->col > cols)
    return;

  if (d->head <= rows && !msg_covers(d->head, d->col)) {
    if (*cell(d->head, d->col) == 0) /* row that appeared after a resize */
      fill_cell(d->head, d->col);
    draw_at(d->head, d->col, BOLD, FG_WHITE, *cell(d->head, d->col));
  }

  for (int dist = 1; dist <= d->trail_len; dist++) {
    int row = d->head - dist;
    if (row < 1)
      break;
    if (row > rows || msg_covers(row, d->col))
      continue;
    if (*cell(row, d->col) == 0) /* row that appeared after a resize */
      fill_cell(row, d->col);

    int intensity = trail_intensity(dist, d->trail_len);
    if (flicker && opt.flicker > 0 && rand() % 100 < opt.flicker) {
      *cell(row, d->col) = random_glyph();
      intensity = BOLD;
    }
    draw_at(row, d->col, intensity, *cell_fg(row, d->col), *cell(row, d->col));
  }

  int blank = d->head - d->trail_len - 1;
  if (blank >= 1 && blank <= rows && !msg_covers(blank, d->col))
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
  if (msg_enabled)
    msg_reveal_under(d);
  render_drop(d, 1);

  /* The blank has just cleared the bottom row: the trail is gone. */
  if (d->head - d->trail_len - 1 >= rows)
    reset_drop(d, START_DELAY_MAX);
}

/* ---------- screen size ------------------------------------------------ */

/* (Re)build the grids and drops for a new window size, keeping as much of
   the running animation as fits. Called once at start (with no old state)
   and again after every resize. */
static void apply_size(int new_rows, int new_cols) {
  size_t ncells = (size_t)new_rows * new_cols;
  uint16_t *new_glyphs = calloc(ncells, sizeof(uint16_t));
  unsigned char *new_fgs = calloc(ncells, 1);
  int new_ndrops = (new_cols + COLUMN_GAP - 1) / COLUMN_GAP;
  Drop *new_drops = calloc((size_t)new_ndrops, sizeof(Drop));
  int *new_map = msg_enabled ? malloc(ncells * sizeof(int)) : NULL;
  if (!new_glyphs || !new_fgs || !new_drops || (msg_enabled && !new_map))
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
  free(msg_map);
  glyphs = new_glyphs;
  cell_fgs = new_fgs;
  drops = new_drops;
  msg_map = new_map;
  ndrops = new_ndrops;
  rows = new_rows;
  cols = new_cols;

  /* Brand-new columns (window got wider) start after a random wait. */
  for (int i = keep_drops; i < ndrops; i++) {
    drops[i].col = 1 + i * COLUMN_GAP;
    reset_drop(&drops[i], START_DELAY_MAX);
  }

  if (msg_enabled) {
    msg_clear();
    msg_on_resize();
  }

  /* Wipe everything (including fragments the terminal may have reflowed)
     and redraw every running trail and visible letter at once. */
  frame_len = 0;
  frame_append(RESET_COLOURS CLEAR_SCREEN, strlen(RESET_COLOURS CLEAR_SCREEN));
  for (int i = 0; i < ndrops; i++)
    render_drop(&drops[i], 0);
  for (int i = 0; i < mg_count; i++)
    if (mg[i].revealed)
      msg_draw_glyph(i);
  flush_frame();
}

static void handle_resize(void) {
  got_resize = 0;
  int r, c;
  if (get_window_size(&r, &c) == -1)
    return; /* transient 0x0: wait for the next SIGWINCH */
  apply_size(r, c);
}

/* ---------- options ---------------------------------------------------- */

static void usage(FILE *f, const char *prog) {
  fprintf(
      f,
      "Usage: %s [-s] [-r] [-c] [-u delay] [-C colour] [-p file] [-F percent]"
      " [-N]\n"
      "  -s          screensaver mode: exit on any key press or mouse event\n"
      "  -u delay    update delay, 0 (fastest) to 10 (slowest); default %d\n"
      "              while running, press 0-9 to change it\n"
      "  -C colour   trail colour; default green\n"
      "  -r          rainbow trails (overrides -C)\n"
      "  -c          half-width katakana (needs a UTF-8 terminal)\n"
      "  -p file     message mode: the rain resolves into each line of file\n"
      "              in turn (UTF-8; Japanese and other wide text work)\n"
      "  -F percent  how often trail characters flicker, 0 to 100; "
      "default %d\n"
      "  -N          ignore the config file\n"
      "  -h          show this help\n"
      "\n"
      "Settings are also read from $CCMATRIX_CONFIG, or else\n"
      "$XDG_CONFIG_HOME/ccmatrix/config (~/.config/ccmatrix/config).\n"
      "Command-line options override the config file.\n",
      prog, UPDATE_DEFAULT, FLICKER_DEFAULT);
}

static long tick_ms_for(int update) {
  long ms = update * 10L;
  return ms < TICK_MIN_MS ? TICK_MIN_MS : ms;
}

#define OPTSTRING "su:C:rcp:F:Nh"

static int parse_args(int argc, char *argv[]) {
  bool use_config = true;
  int o;
  opterr = 0;
  //   #define OPTSTRING "su:C:rcp:F:Nh"
  while ((o = getopt(argc, argv, OPTSTRING)) != -1)
    if (o == 'N')
      use_config = false;

  if (use_config == true) {
    char *path = config_default_path();
    bool must_exist = getenv("CCMATRIX_CONFIG") != NULL;
    int rc = path ? config_load(&opt, path, must_exist) : 0;
    free(path);
    if (rc == -1)
      return 1 + EXIT_FAILURE;
  }

  /* Pass 2: the real parse; command-line values override the config. */
  opterr = 1;
#if defined(__GLIBC__)
  optind = 0; /* full reset on glibc */
#else
  optind = 1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
  optreset = 1;
#endif
#endif
  while ((o = getopt(argc, argv, OPTSTRING)) != -1) {
    int v;
    switch (o) {
    case 's':
      opt.screensaver = 1;
      break;
    case 'r':
      opt.rainbow = 1;
      break;
    case 'c':
      opt.katakana = 1;
      break;
    case 'N':
      break;
    case 'h':
      usage(stdout, argv[0]);
      return 1 + EXIT_SUCCESS;
    case 'C':
      if ((v = parse_colour(optarg)) == -1) {
        fprintf(stderr, "%s: unknown colour '%s' for -C\n", argv[0], optarg);
        print_colour_list(stderr);
        return 1 + EXIT_FAILURE;
      }
      opt.trail_fg = v;
      break;
    case 'u':
      if ((v = parse_update(optarg)) == -1) {
        fprintf(stderr,
                "%s: invalid delay '%s' for -u: must be a whole number "
                "from 0 to %d\n",
                argv[0], optarg, UPDATE_MAX);
        return 1 + EXIT_FAILURE;
      }
      opt.update = v;
      break;
    case 'F':
      if ((v = parse_percent(optarg)) == -1) {
        fprintf(stderr,
                "%s: invalid flicker '%s' for -F: must be a whole number "
                "from 0 to 100\n",
                argv[0], optarg);
        return 1 + EXIT_FAILURE;
      }
      opt.flicker = v;
      break;
    case 'p':
      free(opt.message_file);
      opt.message_file = strdup(optarg);
      if (!opt.message_file) {
        perror("strdup");
        return 1 + EXIT_FAILURE;
      }
      break;
    default:
      usage(stderr, argv[0]);
      return 1 + EXIT_FAILURE;
    }
  }
  if (optind < argc) {
    usage(stderr, argv[0]);
    return 1 + EXIT_FAILURE;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  options_defaults(&opt);
  int rc = parse_args(argc, argv);
  if (rc) {
    options_free(&opt);
    return rc - 1;
  }

  /* Load the message file before touching the terminal, so any problem is
     reported on a normal screen. */
  if (opt.message_file) {
    int lr = text_load(opt.message_file, &msg_text);
    if (lr == -1) {
      fprintf(stderr, "%s: cannot read message file '%s': %s\n", argv[0],
              opt.message_file, strerror(errno));
      options_free(&opt);
      return EXIT_FAILURE;
    }
    if (lr == -2) {
      fprintf(stderr, "%s: message file '%s' has no text in it\n", argv[0],
              opt.message_file);
      options_free(&opt);
      return EXIT_FAILURE;
    }
    msg_enabled = 1;
  }

  int r, c;
  if (get_window_size(&r, &c) == -1) {
    fprintf(stderr,
            "%s: cannot get the terminal size (is stdout a terminal?)\n",
            argv[0]);
    options_free(&opt);
    return EXIT_FAILURE;
  }

  enable_raw_mode();
  install_signal_handlers();
  if (opt.screensaver)
    enable_mouse();
  srand((unsigned)time(NULL));
  apply_size(r, c);

  long started = now_ms();
  /* The first message comes after half the usual wait. */
  set_phase(PH_RAIN, started - opt.message_interval_ms / 2);

  while (!got_quit_signal) {
    /* Resize: rebuild for the new size before drawing anything. */
    if (got_resize)
      handle_resize();

    /* Input. Normal mode: 'q' or Ctrl-C quits, '0'-'9' set the speed,
          everything else is ignored. Screensaver mode: any byte at all (key,
          Enter, arrow key, mouse report) means quit, after a short grace
          period. Drain everything waiting either way. */
    char ch;
    ssize_t n;
    int quit = 0;
    long now = now_ms();
    while ((n = read(STDIN_FILENO, &ch, 1)) == 1) {
      if (opt.screensaver) {
        if (now - started >= SCREENSAVER_GRACE_MS)
          quit = 1;
      } else if (ch == 'q' || ch == 3) {
        quit = 1;
      } else if (ch >= '0' && ch <= '9') {
        opt.update = ch - '0'; /* takes effect on this very tick */
      }
    }
    if (n == -1 && errno != EAGAIN && errno != EINTR)
      die("read");
    if (quit)
      break;

    /* Update every column and the message, then draw in one go. */
    for (int i = 0; i < ndrops; i++)
      update_drop(&drops[i]);
    if (msg_enabled)
      msg_tick(now_ms());
    flush_frame();

    sleep_ms(tick_ms_for(opt.update));
  }

  free(drops);
  free(glyphs);
  free(cell_fgs);
  free(msg_map);
  free(mg);
  free(frame);
  text_free(&msg_text);
  options_free(&opt);
  return EXIT_SUCCESS; /* atexit -> restore_terminal() */
}
