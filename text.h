#ifndef CCMATRIX_TEXT_H
#define CCMATRIX_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* One line of a message file, decoded to Unicode code points. */
typedef struct {
  uint32_t *cps;
  size_t len;
} TextLine;

typedef struct {
  TextLine *lines;
  size_t count;
} TextFile;

/* Decode one UTF-8 sequence from s (len bytes available). Stores the code
   point in *out (U+FFFD for invalid input) and returns the bytes consumed
   (always at least 1). */
size_t utf8_decode(const unsigned char *s, size_t len, uint32_t *out);

/* Encode cp as UTF-8 into out. Returns the number of bytes (1-4). */
int utf8_encode(uint32_t cp, char out[4]);

/* Number of terminal columns cp occupies: 0 (control/combining),
   1 (most characters) or 2 (CJK, full-width forms, most emoji).
   Locale-independent, so it works the same on every system. */
int char_width(uint32_t cp);

/* Load a UTF-8 text file, one message per non-empty line. Leading and
   trailing spaces are trimmed, tabs become spaces, control characters are
   dropped. Returns 0 on success, -1 with errno set if the file can't be
   read (EFBIG if it is over 4 MiB), or -2 if it has no non-empty lines. */
int text_load(const char *path, TextFile *out);

void text_free(TextFile *t);

#endif
