#ifndef UI_H
#define UI_H

#include "common.h"

// Initialize both screens for console output
void ui_init(void);

// Draw the title list on the top screen.
// selected = currently highlighted index, count = number of titles.
void ui_draw_title_list(const TitleInfo *titles, int count, int selected, int scroll_offset);

// Draw status/action bar on the bottom screen
void ui_draw_status(const char *status_line);

// Show a message on the bottom screen (clears it first)
void ui_draw_message(const char *msg);

// Lightweight progress update - overwrites line 1 without clearing screen
void ui_update_progress(const char *msg);

// Clear both screens
void ui_clear(void);

#endif // UI_H
