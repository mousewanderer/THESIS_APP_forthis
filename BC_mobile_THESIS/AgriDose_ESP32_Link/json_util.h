// json_util.h -- one helper for writing JSON text.
#pragma once
#include <Arduino.h>

// Prints s as a JSON string in quotes, escaping " \ and control characters.
static inline void jsonString(Print& out, const char* s) {
  out.print('"');
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { out.print('\\'); out.print(c); }
    else if ((unsigned char)c < 32) out.printf("\\u%04x", (unsigned char)c);
    else out.print(c);
  }
  out.print('"');
}
