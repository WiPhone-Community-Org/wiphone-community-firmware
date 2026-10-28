/*
 * json_read.h - the firmware's JSON reader: no allocation, no recursion, every buffer the
 * caller's. Written for Menu > AI (gemini.cpp, 0.9.81) and moved here unchanged when the
 * Almanac's weather (weather.cpp) needed the same reader with numbers and null, which Google's
 * answers never carry and Open-Meteo's do ("temperature_2m":[17.1,null,...]).
 *
 * The model is a cursor over [p, e): a caller walks an object with jsOpen(j, '{') then
 * jsObjNext until it says 0 (closed), reading or skipping each value; an array the same way
 * with jsArrNext. A nested value nobody wants is skipped whole by counting brackets (strings
 * inside skipped properly), so a hostile body costs a scan, never stack - safe on an 8 KB worker
 * stack. A body cut short makes the next call fail (-1 / false); nothing reads past `e`.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header (the host suite links it: test_gemini,
 * test_weather). kosync.cpp has its own small reader with the same names, file-static; this one
 * is shared on purpose.
 */
#ifndef JSON_READ_H
#define JSON_READ_H

#include <stddef.h>
#include <stdint.h>

struct Js {
  const char* p;
  const char* e;
};

void jsWs(Js* j);                         // skip whitespace
/* At '"': decode the string, appending at out[*o] (NUL kept after it) while it fits; *trunc
 * when something did not. out may be NULL (skip). \uXXXX and surrogate pairs become UTF-8; a
 * lone surrogate U+FFFD; \u0000 is dropped. False on a malformed string. */
bool jsString(Js* j, char* out, size_t cap, size_t* o, bool* trunc);
/* Skip one value of any kind: a string, a primitive, or a whole object/array by counting
 * brackets. False on a body cut short. */
bool jsSkip(Js* j);
bool jsOpen(Js* j, char c);               // at `c` ('{' or '['): consume it
/* Object iteration: 1 = a key was read into `key` and the value is next; 0 = the object
 * closed; -1 = not JSON. */
int  jsObjNext(Js* j, char* key, size_t kcap);
/* Array iteration: 1 = a value is next; 0 = the array closed; -1 = not JSON. */
int  jsArrNext(Js* j);
/* A string value into out (cut to fit); any other value is skipped and out left alone. */
bool jsStrInto(Js* j, char* out, size_t cap);
/* `true` -> true (consumed); any other value is skipped -> false. */
bool jsTrue(Js* j);

/* (0.9.81, for Open-Meteo) The value at the cursor as a number: 1 = a JSON number in *v; 0 =
 * `null` (consumed; *v untouched) - Open-Meteo puts null in any array where a model has no
 * value; -1 = anything else (skipped whole, *v untouched) or a body cut short. A number is
 * JSON's grammar exactly (-?int frac? exp?), at most 40 characters. */
int  jsNumber(Js* j, double* v);

#endif // JSON_READ_H
