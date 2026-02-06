#ifndef UI_H
#define UI_H

#include "common.h"
#include "sync.h"

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

// Show save details dialog on top screen
// Returns when user presses B to close
void ui_show_save_details(const TitleInfo *title, const SaveDetails *details);

// Show config editor menu on top screen
// Returns true if config was changed, false otherwise
bool ui_show_config_editor(AppConfig *config);

#endif // UI_H
