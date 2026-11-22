#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../api.h"
#include "../handles.h"
#include "../log.h"
#include "../lowmem.h"
#include "../thread.h"

#define TRUE 1
#define FALSE 0

#define ERROR_SUCCESS 0
#define ERROR_INVALID_FUNCTION 1
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_HANDLE_EOF 38
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_IO_PENDING 997
#define ERROR_CALL_NOT_IMPLEMENTED 120

void win32_init(const char *cmdline);
_Noreturn void win32_process_exit(uint32_t code);

extern uint32_t g_guest_cmdline; // guest string

// errno -> Win32 error
uint32_t win32_errno(int e);

// ANSI (treated as Latin-1) <-> UTF-16 helpers
size_t wlen(const uint16_t *s);
void w2a(const uint16_t *w, char *out, size_t outsz);
void a2w(const char *s, uint16_t *out, size_t outcnt);

// Copy string into a guest buffer with Windows "returns length" semantics.
// Returns chars copied (excluding NUL) or required size (including NUL) if too small.
uint32_t copy_out_str(char *dst, uint32_t dstlen, const char *src);

// Monotonic ms since process start
uint32_t win32_tick_ms(void);

// Window/GUI layer hooks (user32.c)
void user32_init(void);
void user32_shutdown(void);
