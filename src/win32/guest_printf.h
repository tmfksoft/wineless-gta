#pragma once
#include <stddef.h>
#include <stdint.h>

// Format a Windows/MSVC printf-style string whose varargs are 32-bit guest stack
// slots. Supports flags, width, precision, h/l/ll/I64 and d i u x X o c s p %.
// Returns length written (excluding NUL), truncating to outsz.
int guest_vsnprintf(char *out, size_t outsz, const char *fmt, const uint32_t *args);
