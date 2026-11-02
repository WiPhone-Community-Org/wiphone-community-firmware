/* files_paths.h — the path arithmetic behind the Files app's folder operations, kept in a
 * header of its own so the HOST SUITE can test the code the firmware executes.
 *
 * A folder can now be copied, moved and deleted from the phone ({user}, 2026-09-20: "there is
 * no way to select a folder and copy/paste/delete... found it out when trying to delete
 * 'home' in the maps folder"). Every one of those is a tree walk that ends in unlink(), and
 * the questions that decide whether it is SAFE are pure string questions: is the destination
 * inside the source (a copy of /maps into /maps/x never ends); does the clipboard point into
 * the tree being deleted (a paste of a ghost); is this the card's root. Getting a prefix test
 * wrong here — "/maps/home" matching "/maps/homely" — deletes the wrong folder, so the tests
 * pin the boundaries.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 */
#ifndef FILES_PATHS_H
#define FILES_PATHS_H

#include <stddef.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdio.h>

/* Is `path` the same as `dir`, or inside it? Paths are absolute, "/"-separated, no trailing
 * slash except the root itself. "/" contains everything. */
static inline int filePathWithin(const char* dir, const char* path) {
  if (!dir || !path || dir[0] != '/' || path[0] != '/') {
    return 0;
  }
  if (dir[1] == '\0') {
    return 1;                                  /* the root holds everything */
  }
  const size_t dl = strlen(dir);
  if (strncmp(dir, path, dl) != 0) {
    return 0;
  }
  return path[dl] == '\0' || path[dl] == '/';  /* the boundary must be the end or a slash */
}

/* filePathWithin, blind to ASCII case - the card is FAT, where "/maps/ELEV" IS "/maps/elev": a
 * guard over a folder the firmware names in lower case (the map download's /maps/<key> and
 * /maps/elev) must match it however it is spelt on the card (review 2026-09-27: a card's "Elev"
 * slipped past the Files app's download guard; the map's own scan is case-blind already). */
static inline int filePathWithinNoCase(const char* dir, const char* path) {
  if (!dir || !path || dir[0] != '/' || path[0] != '/') {
    return 0;
  }
  if (dir[1] == '\0') {
    return 1;
  }
  const size_t dl = strlen(dir);
  if (strncasecmp(dir, path, dl) != 0) {
    return 0;
  }
  return path[dl] == '\0' || path[dl] == '/';
}

/* dir + "/" + name into out. 1 = fits; 0 = it would not, and out is left "" — a truncated
 * path names the WRONG place, which is worse than no path. */
static inline int filePathJoin(const char* dir, const char* name, char* out, size_t cap) {
  if (!dir || !name || !out || cap == 0) {
    return 0;
  }
  const int root = (dir[0] == '/' && dir[1] == '\0');
  const int n = snprintf(out, cap, "%s%s%s", dir, root ? "" : "/", name);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return 1;
}

/* The last component of a path ("/maps/home" -> "home"; "/" -> ""). */
static inline const char* filePathBase(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

/* The same file or folder, re-rooted: src root -> dst root, the part below the root kept.
 * `full` must lie within `srcRoot` (filePathWithin). 0 when it does not fit. */
static inline int filePathReroot(const char* srcRoot, const char* dstRoot, const char* full,
                                 char* out, size_t cap) {
  if (!filePathWithin(srcRoot, full)) {
    return 0;
  }
  const char* rel = full + strlen(srcRoot);   /* "" or "/x/y" */
  const int n = snprintf(out, cap, "%s%s", dstRoot, rel);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return 1;
}

/* Is `path` a KEY FILE - anything inside a folder named "API Keys" (any case, any depth: FAT is
 * case-blind and a card may be laid out by hand), or a file named gemini.txt anywhere (where the
 * T-Deck firmware keeps one, at the card's root)? The Files viewer refuses to draw these (Menu >
 * AI's key, README "AI"): a screenshot of a key is a leaked key. */
static inline int filePathIsKeyFile(const char* path) {
  if (!path) {
    return 0;
  }
  const char* p = path;
  while (*p) {
    while (*p == '/') p++;
    const char* e = p;
    while (*e && *e != '/') e++;
    const size_t n = (size_t)(e - p);
    if (*e == '/' && n == 8 && strncasecmp(p, "API Keys", 8) == 0) {
      return 1;                                /* a folder on the way down */
    }
    if (*e == '\0' && n == 10 && strncasecmp(p, "gemini.txt", 10) == 0) {
      return 1;
    }
    p = e;
  }
  return 0;
}

#endif /* FILES_PATHS_H */
