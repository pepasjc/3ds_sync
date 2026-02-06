#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

// Load config from sdmc:/3ds/3dssync/config.txt
// Returns true on success, false on failure.
// On failure, writes a human-readable error to error_out.
// Format: key=value per line (server_url=..., api_key=...)
bool config_load(AppConfig *config, char *error_out, int error_size);

#endif // CONFIG_H
