#pragma once
#include <stdint.h>

enum { DISPLAY_FULLSCREEN, DISPLAY_BORDERLESS, DISPLAY_WINDOWED };

#ifdef __cplusplus
extern "C" {
#endif
struct SDL_Window;
void display_init(void); // requires SDL video to be initialized
int display_mode_count(void);
void display_mode(int i, uint32_t *w, uint32_t *h, uint32_t *hz);
void display_desktop(uint32_t *w, uint32_t *h, uint32_t *hz);
int display_policy(void);
// Make the real window match the game's resolution and fullscreen request.
void display_apply(struct SDL_Window *win, uint32_t w, uint32_t h, int game_wants_fullscreen);
// Call when SDL reports the window changed display.
void display_window_moved(struct SDL_Window *win);
// Called by the D3D9 bridge on CreateDevice/Reset (implemented in user32.c).
int wineless_window_presentable(void);
void wineless_set_game_resolution(uint32_t hwnd, uint32_t w, uint32_t h, int game_wants_fullscreen);
#ifdef __cplusplus
}
#endif
