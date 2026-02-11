#ifndef UI_H
#define UI_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>

// Show save details screen
void ui_show_save_details(Title *title);

// Show confirmation dialog before upload/download with local vs server info
// Returns: true if user confirms (A), false if cancelled (B)
bool ui_confirm_sync(Title *title, const char *server_hash, size_t server_size, bool is_upload);

#endif
