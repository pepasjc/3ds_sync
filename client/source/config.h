#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

// Load config from sdmc:/3ds/3dssync/config.txt
// Returns true on success, false on failure.
// On failure, writes a human-readable error to error_out.
// Format: key=value per line (server_url=..., api_key=...)
bool config_load(AppConfig *config, char *error_out, int error_size);

// Save config to sdmc:/3ds/3dssync/config.txt
// Returns true on success, false on failure.
bool config_save(const AppConfig *config);

// Open software keyboard to edit a string field
// Returns true if user confirmed, false if cancelled
// hint: placeholder text shown when empty
// max_len: maximum characters allowed
bool config_edit_field(const char *hint, char *buffer, int max_len);

#endif // CONFIG_H
