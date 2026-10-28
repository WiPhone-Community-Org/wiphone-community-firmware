/* test_filepaths.cpp — the path questions the Files app's folder copy/move/delete rest on
 * (WiPhone/files_paths.h). A wrong prefix test here deletes the wrong folder. */
#include <cstdio>
#include <cstring>
#include "../WiPhone/files_paths.h"

static int failures = 0, checks = 0;
static void ok(bool c, const char* what) {
  checks++;
  if (!c) { failures++; printf("  \033[31mFAIL\033[0m %s\n", what); }
  else    { printf("  ok  %s\n", what); }
}

int main() {
  printf("\n\033[1mfilePathWithin\033[0m\n");
  ok(filePathWithin("/maps/home", "/maps/home"), "a folder is within itself");
  ok(filePathWithin("/maps/home", "/maps/home/15/5296/11463.565"), "a deep file is within");
  ok(!filePathWithin("/maps/home", "/maps/homely"), "a longer sibling name is NOT within (the boundary)");
  ok(!filePathWithin("/maps/home", "/maps/hom"), "a shorter name is not within");
  ok(!filePathWithin("/maps/home", "/maps"), "the parent is not within the child");
  ok(filePathWithin("/", "/anything/at/all"), "the root holds everything");
  ok(filePathWithin("/", "/"), "the root holds itself");
  ok(!filePathWithin("maps", "/maps/x"), "a relative dir is refused");
  ok(!filePathWithin("/maps", "maps/x"), "a relative path is refused");
  ok(!filePathWithin(NULL, "/x") && !filePathWithin("/x", NULL), "NULLs are refused");

  printf("\n\033[1mfilePathWithinNoCase\033[0m (the map download's guard: FAT is case-blind)\n");
  ok(filePathWithinNoCase("/maps/elev", "/maps/Elev"), "the card's 'Elev' IS the job's 'elev'");
  ok(filePathWithinNoCase("/maps/elev", "/MAPS/ELEV/13/1330/2862.elv"), "...and a tile under 'ELEV'");
  ok(filePathWithinNoCase("/MAPS/ELEV", "/maps/elev"), "either way round");
  ok(filePathWithinNoCase("/maps/Elev", "/maps") == 0 && filePathWithinNoCase("/maps", "/maps/Elev"),
     "parent/child as filePathWithin: the parent holds the child, not the reverse");
  ok(!filePathWithinNoCase("/maps/elev", "/maps/Elevation"), "the boundary still holds ('Elevation' is not 'elev')");
  ok(!filePathWithinNoCase("/maps/usgs", "/maps/usgsimg"), "...for a source key too");
  ok(filePathWithinNoCase("/", "/X") && !filePathWithinNoCase("maps", "/maps") &&
     !filePathWithinNoCase(NULL, "/x") && !filePathWithinNoCase("/x", NULL),
     "the root, relative paths and NULLs as filePathWithin");

  printf("\n\033[1mfilePathJoin\033[0m\n");
  char out[32];
  ok(filePathJoin("/", "maps", out, sizeof(out)) && !strcmp(out, "/maps"), "root + name has one slash");
  ok(filePathJoin("/maps", "home", out, sizeof(out)) && !strcmp(out, "/maps/home"), "dir + name");
  ok(!filePathJoin("/a/very/long/dir", "and-a-long-name.565", out, 16) && out[0] == '\0',
     "a path that would not fit is refused, and out is empty");
  ok(filePathJoin("/abc", "defgh", out, 11) && !strcmp(out, "/abc/defgh"), "exactly fits (10 chars + NUL)");
  ok(!filePathJoin("/abc", "defgh", out, 10), "one byte short is refused");

  printf("\n\033[1mfilePathBase\033[0m\n");
  ok(!strcmp(filePathBase("/maps/home"), "home"), "the last component");
  ok(!strcmp(filePathBase("/"), ""), "the root has no name");
  ok(!strcmp(filePathBase("name"), "name"), "a bare name is its own base");

  printf("\n\033[1mfilePathReroot\033[0m\n");
  char o[64];
  ok(filePathReroot("/maps/home", "/backup/home", "/maps/home/15/1.565", o, sizeof(o)) &&
     !strcmp(o, "/backup/home/15/1.565"), "a file below the root moves with it");
  ok(filePathReroot("/maps/home", "/backup/home", "/maps/home", o, sizeof(o)) &&
     !strcmp(o, "/backup/home"), "the root itself maps to the new root");
  ok(!filePathReroot("/maps/home", "/backup/home", "/maps/homely/x", o, sizeof(o)),
     "a path outside the source is refused");
  ok(!filePathReroot("/maps/home", "/backup/home", "/maps/home/15/1.565", o, 20) && o[0] == '\0',
     "a result that would not fit is refused, and out is empty");

  printf("\n\033[1mfilePathIsKeyFile\033[0m (the Files viewer never draws a key)\n");
  ok(filePathIsKeyFile("/API Keys/gemini.txt"), "the AI's key");
  ok(filePathIsKeyFile("/api keys/Gemini.TXT") && filePathIsKeyFile("/API KEYS/other.txt"),
     "any case (FAT), any file in the folder");
  ok(filePathIsKeyFile("/API Keys/gemini.pem") && filePathIsKeyFile("/books/API Keys/x.txt"),
     "any file type in it, and a folder of that name anywhere");
  ok(filePathIsKeyFile("/gemini.txt") && filePathIsKeyFile("/roms/GEMINI.txt"),
     "gemini.txt anywhere (the T-Deck keeps it at the root)");
  ok(!filePathIsKeyFile("/API Keysx/a.txt") && !filePathIsKeyFile("/my API Keys/a.txt") &&
     !filePathIsKeyFile("/books/gemini.txt.epub") && !filePathIsKeyFile("/books/notes.txt"),
     "only the exact names");
  ok(!filePathIsKeyFile("/API Keys") && !filePathIsKeyFile(NULL) && !filePathIsKeyFile("/"),
     "the folder itself is not a file in it; nothing");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
