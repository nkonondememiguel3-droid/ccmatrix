#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLACEMENT_CHAR 0xFFFD
#define MAX_FILE_BYTES (4L * 1024 * 1024)

size_t utf8_decode(const unsigned char *s, size_t len, uint32_t *out) {
  static const uint32_t min_for_len[] = {0, 0, 0x80, 0x800, 0x10000};
  unsigned char c = s[0];
  uint32_t cp;
  size_t n;

  if (c < 0x80) {
    *out = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    n = 2;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    n = 3;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    n = 4;
  } else {
    *out = REPLACEMENT_CHAR;
    return 1;
  }

  if (n > len) {
    *out = REPLACEMENT_CHAR;
    return 1;
  }
  for (size_t i = 1; i < n; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *out = REPLACEMENT_CHAR;
      return 1;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }

  /* Reject overlong encodings, UTF-16 surrogates and out-of-range values. */
  if (cp < min_for_len[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *out = REPLACEMENT_CHAR;
    return 1;
  }
  *out = cp;
  return n;
}

int utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

int char_width(uint32_t cp) {
  /* Control characters. */
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
    return 0;

  /* Common zero-width and combining characters. */
  if ((cp >= 0x0300 && cp <= 0x036F) || /* combining diacritics */
      (cp >= 0x200B && cp <= 0x200F) || /* zero-width space, joiners, marks */
      (cp >= 0x20D0 && cp <= 0x20FF) || /* combining marks for symbols */
      (cp >= 0x3099 && cp <= 0x309A) || /* combining kana voicing marks */
      (cp >= 0xFE00 && cp <= 0xFE0F) || /* variation selectors */
      cp == 0xFEFF)                     /* byte order mark */
    return 0;

  /* East Asian Wide and Full-width ranges. */
  if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo */
      (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals, punctuation */
      (cp >= 0x3041 && cp <= 0x33FF) ||   /* hiragana, katakana, CJK compat */
      (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK extension A */
      (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified ideographs */
      (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi */
      (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables */
      (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility ideographs */
      (cp >= 0xFE30 && cp <= 0xFE4F) ||   /* CJK compatibility forms */
      (cp >= 0xFF00 && cp <= 0xFF60) ||   /* full-width forms */
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||   /* full-width signs */
      (cp >= 0x1F300 && cp <= 0x1F64F) || /* emoji & pictographs */
      (cp >= 0x1F900 && cp <= 0x1F9FF) || /* supplemental pictographs */
      (cp >= 0x20000 && cp <= 0x3FFFD))   /* CJK extensions B and later */
    return 2;

  return 1;
}

static int is_space(uint32_t cp) { return cp == ' ' || cp == 0x3000; }

/* Decode one line of raw bytes and append it to t if it isn't blank. */
static int add_line(TextFile *t, size_t *cap, const unsigned char *s,
                    size_t len) {
  uint32_t *cps = malloc((len + 1) * sizeof(uint32_t));
  if (!cps)
    return -1;

  size_t n = 0;
  for (size_t i = 0; i < len;) {
    uint32_t cp;
    i += utf8_decode(s + i, len - i, &cp);
    if (cp == '\t')
      cp = ' ';
    if (cp == 0xFEFF || (cp != ' ' && char_width(cp) == 0 && cp < 0x300))
      continue; /* BOM, \r and other control characters */
    cps[n++] = cp;
  }

  size_t start = 0;
  while (start < n && is_space(cps[start]))
    start++;
  while (n > start && is_space(cps[n - 1]))
    n--;

  if (n == start) {
    free(cps);
    return 0;
  }
  if (start > 0)
    memmove(cps, cps + start, (n - start) * sizeof(uint32_t));
  n -= start;

  if (t->count == *cap) {
    size_t new_cap = *cap ? *cap * 2 : 16;
    TextLine *p = realloc(t->lines, new_cap * sizeof(TextLine));
    if (!p) {
      free(cps);
      return -1;
    }
    t->lines = p;
    *cap = new_cap;
  }
  t->lines[t->count].cps = cps;
  t->lines[t->count].len = n;
  t->count++;
  return 0;
}

int text_load(const char *path, TextFile *out) {
  out->lines = NULL;
  out->count = 0;

  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;

  unsigned char *buf = NULL;
  size_t len = 0, cap = 0;
  for (;;) {
    if (len == cap) {
      cap = cap ? cap * 2 : 4096;
      if ((long)cap > MAX_FILE_BYTES + 4096) {
        free(buf);
        fclose(f);
        errno = EFBIG;
        return -1;
      }
      unsigned char *p = realloc(buf, cap);
      if (!p) {
        free(buf);
        fclose(f);
        return -1;
      }
      buf = p;
    }
    size_t got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0)
      break;
  }
  int read_error = ferror(f);
  int saved_errno = errno;
  fclose(f);
  if (read_error) {
    free(buf);
    errno = saved_errno ? saved_errno : EIO;
    return -1;
  }

  size_t lines_cap = 0;
  size_t line_start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || buf[i] == '\n') {
      if (add_line(out, &lines_cap, buf + line_start, i - line_start) == -1) {
        free(buf);
        text_free(out);
        errno = ENOMEM;
        return -1;
      }
      line_start = i + 1;
    }
  }
  free(buf);

  if (out->count == 0) {
    text_free(out);
    return -2;
  }
  return 0;
}

void text_free(TextFile *t) {
  for (size_t i = 0; i < t->count; i++)
    free(t->lines[i].cps);
  free(t->lines);
  t->lines = NULL;
  t->count = 0;
}
