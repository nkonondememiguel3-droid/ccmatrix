#ifndef CCMATRIX_CONFIG_H
#define CCMATRIX_CONFIG_H

#include <stdio.h>

#define UPDATE_DEFAULT 4 /* -u default; tick = update * 10 ms, like cmatrix */
#define UPDATE_MAX 10
#define FLICKER_DEFAULT 6 /* % chance per trail cell, each time a drop moves */

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

/* Everything the user can choose. Filled from defaults, then the config
   file, then the command line (later sources win). */
typedef struct {
  int update;       /* 0 (fastest) .. 10 (slowest) */
  int trail_fg;     /* FG_* */
  int message_fg;   /* FG_* colour of resolved message text */
  int rainbow;      /* bool */
  int katakana;     /* bool */
  int screensaver;  /* bool */
  int flicker;      /* 0..100 percent */
  char *message_file;     /* NULL = no message mode; malloc'd */
  long message_interval_ms; /* plain rain between two messages */
  long message_hold_ms;     /* how long a resolved message stays */
} Options;

void options_defaults(Options *o);
void options_free(Options *o);

/* Where the config file is looked for: $CCMATRIX_CONFIG, else
   $XDG_CONFIG_HOME/ccmatrix/config, else ~/.config/ccmatrix/config.
   Returns a malloc'd path, or NULL if no home directory is known. */
char *config_default_path(void);

/* Read key = value settings from path into o. A missing file is fine
   unless must_exist is set. On any problem prints "path:line: reason" to
   stderr and returns -1; otherwise returns 0. */
int config_load(Options *o, const char *path, int must_exist);

/* Value parsers shared by the config file and the command line.
   Each returns -1 for an invalid value. */
int parse_update(const char *s);  /* 0..UPDATE_MAX */
int parse_colour(const char *s);  /* colour name -> FG_* */
int parse_percent(const char *s); /* 0..100 */

void print_colour_list(FILE *f);

#endif
