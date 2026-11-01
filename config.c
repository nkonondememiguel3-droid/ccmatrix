#define _DEFAULT_SOURCE
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct {
  const char *name;
  int fg;
} colour_names[] = {
    {"green", FG_GREEN},     {"red", FG_RED},       {"blue", FG_BLUE},
    {"white", FG_WHITE},     {"yellow", FG_YELLOW}, {"cyan", FG_CYAN},
    {"magenta", FG_MAGENTA}, {"black", FG_BLACK},
};
#define N_COLOURS (sizeof(colour_names) / sizeof(colour_names[0]))

void options_defaults(Options *o) {
  o->update = UPDATE_DEFAULT;
  o->trail_fg = FG_GREEN;
  o->message_fg = FG_WHITE;
  o->rainbow = 0;
  o->katakana = 0;
  o->screensaver = 0;
  o->flicker = FLICKER_DEFAULT;
  o->message_file = NULL;
  o->message_interval_ms = 8000;
  o->message_hold_ms = 5000;
}

void options_free(Options *o) {
  free(o->message_file);
  o->message_file = NULL;
}

/* ---------- value parsers ---------------------------------------------- */

static int parse_int_range(const char *s, long lo, long hi) {
  char *end;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno == ERANGE || v < lo || v > hi)
    return -1;
  return (int)v;
}

int parse_update(const char *s) { return parse_int_range(s, 0, UPDATE_MAX); }

int parse_percent(const char *s) { return parse_int_range(s, 0, 100); }

int parse_colour(const char *s) {
  for (size_t i = 0; i < N_COLOURS; i++)
    if (strcasecmp(s, colour_names[i].name) == 0)
      return colour_names[i].fg;
  return -1;
}

void print_colour_list(FILE *f) {
  fprintf(f, "Supported colours:");
  for (size_t i = 0; i < N_COLOURS; i++)
    fprintf(f, " %s", colour_names[i].name);
  fprintf(f, "\n");
}

static int parse_bool(const char *s) {
  static const char *yes[] = {"yes", "true", "on", "1"};
  static const char *no[] = {"no", "false", "off", "0"};
  for (size_t i = 0; i < 4; i++) {
    if (strcasecmp(s, yes[i]) == 0)
      return 1;
    if (strcasecmp(s, no[i]) == 0)
      return 0;
  }
  return -1;
}

/* Seconds, decimals allowed, within [lo, hi]. Returns milliseconds or -1. */
static long parse_seconds(const char *s, double lo, double hi) {
  char *end;
  errno = 0;
  double v = strtod(s, &end);
  if (end == s || *end != '\0' || errno == ERANGE || !(v >= lo && v <= hi))
    return -1;
  return (long)(v * 1000.0 + 0.5);
}

/* ---------- paths ------------------------------------------------------ */

static char *join_path(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *p = malloc(la + 1 + lb + 1);
  if (!p)
    return NULL;
  memcpy(p, a, la);
  p[la] = '/';
  memcpy(p + la + 1, b, lb + 1);
  return p;
}

char *config_default_path(void) {
  const char *env = getenv("CCMATRIX_CONFIG");
  if (env && *env)
    return strdup(env);

  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg == '/')
    return join_path(xdg, "ccmatrix/config");

  const char *home = getenv("HOME");
  if (home && *home)
    return join_path(home, ".config/ccmatrix/config");
  return NULL;
}

/* Turn a path from the config file into a usable one: "~/x" is relative to
   $HOME, and a plain relative path is relative to the config file's own
   directory (so the config works no matter where ccmatrix is started). */
static char *resolve_path(const char *value, const char *config_path) {
  if (value[0] == '/')
    return strdup(value);

  if (value[0] == '~' && (value[1] == '/' || value[1] == '\0')) {
    const char *home = getenv("HOME");
    if (!home)
      return NULL;
    return join_path(home, value[1] ? value + 2 : "");
  }

  const char *slash = strrchr(config_path, '/');
  if (!slash)
    return strdup(value);
  size_t dir_len = (size_t)(slash - config_path);
  char *dir = malloc(dir_len + 1);
  if (!dir)
    return NULL;
  memcpy(dir, config_path, dir_len);
  dir[dir_len] = '\0';
  char *p = join_path(dir, value);
  free(dir);
  return p;
}

/* ---------- config file ------------------------------------------------ */

static char *trim(char *s) {
  while (isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = '\0';
  return s;
}

/* Apply one key/value pair. Returns NULL on success, or an error message. */
static const char *apply_setting(Options *o, const char *key,
                                 const char *value, const char *config_path) {
  int v;
  long ms;

  if (strcmp(key, "delay") == 0 || strcmp(key, "speed") == 0) {
    if ((v = parse_update(value)) == -1)
      return "must be a whole number from 0 to 10";
    o->update = v;
  } else if (strcmp(key, "colour") == 0 || strcmp(key, "color") == 0) {
    if ((v = parse_colour(value)) == -1)
      return "unknown colour";
    o->trail_fg = v;
  } else if (strcmp(key, "message_colour") == 0 ||
             strcmp(key, "message_color") == 0) {
    if ((v = parse_colour(value)) == -1)
      return "unknown colour";
    o->message_fg = v;
  } else if (strcmp(key, "rainbow") == 0) {
    if ((v = parse_bool(value)) == -1)
      return "must be yes or no";
    o->rainbow = v;
  } else if (strcmp(key, "katakana") == 0) {
    if ((v = parse_bool(value)) == -1)
      return "must be yes or no";
    o->katakana = v;
  } else if (strcmp(key, "screensaver") == 0) {
    if ((v = parse_bool(value)) == -1)
      return "must be yes or no";
    o->screensaver = v;
  } else if (strcmp(key, "flicker") == 0) {
    if ((v = parse_percent(value)) == -1)
      return "must be a percentage from 0 to 100";
    o->flicker = v;
  } else if (strcmp(key, "message_file") == 0) {
    char *p = resolve_path(value, config_path);
    if (!p)
      return "cannot resolve path";
    free(o->message_file);
    o->message_file = p;
  } else if (strcmp(key, "message_interval") == 0) {
    if ((ms = parse_seconds(value, 0, 3600)) == -1)
      return "must be a number of seconds from 0 to 3600";
    o->message_interval_ms = ms;
  } else if (strcmp(key, "message_hold") == 0) {
    if ((ms = parse_seconds(value, 0.5, 3600)) == -1)
      return "must be a number of seconds from 0.5 to 3600";
    o->message_hold_ms = ms;
  } else {
    return "unknown setting";
  }
  return NULL;
}

int config_load(Options *o, const char *path, int must_exist) {
  FILE *f = fopen(path, "r");
  if (!f) {
    if (errno == ENOENT && !must_exist)
      return 0;
    fprintf(stderr, "ccmatrix: cannot read config file '%s': %s\n", path,
            strerror(errno));
    return -1;
  }

  char line[4096];
  int lineno = 0, ok = 1;
  while (fgets(line, sizeof(line), f)) {
    lineno++;
    if (!strchr(line, '\n') && !feof(f)) {
      fprintf(stderr, "%s:%d: line too long\n", path, lineno);
      ok = 0;
      break;
    }

    char *s = trim(line);
    if (*s == '\0' || *s == '#' || *s == ';')
      continue;

    char *eq = strchr(s, '=');
    if (!eq) {
      fprintf(stderr, "%s:%d: expected 'key = value'\n", path, lineno);
      ok = 0;
      continue;
    }
    *eq = '\0';
    char *key = trim(s);
    char *value = trim(eq + 1);

    /* Allow "quoted values" (handy for paths with spaces). */
    size_t vlen = strlen(value);
    if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
      value[vlen - 1] = '\0';
      value++;
    }
    if (*value == '\0') {
      fprintf(stderr, "%s:%d: '%s' has no value\n", path, lineno, key);
      ok = 0;
      continue;
    }

    const char *err = apply_setting(o, key, value, path);
    if (err && strcmp(err, "unknown setting") == 0) {
      fprintf(stderr, "%s:%d: unknown setting '%s'\n", path, lineno, key);
      ok = 0;
    } else if (err) {
      fprintf(stderr, "%s:%d: %s: '%s' %s\n", path, lineno, key, value, err);
      if (strstr(err, "colour"))
        print_colour_list(stderr);
      ok = 0;
    }
  }
  fclose(f);
  return ok ? 0 : -1;
}
