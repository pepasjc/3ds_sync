#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

// Load config from file
bool config_load(SyncState *state, char *error, size_t error_size);

#endif
