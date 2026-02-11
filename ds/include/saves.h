#ifndef SAVES_H
#define SAVES_H

#include "common.h"

// Scan for local saves (cartridge and flashcard)
int saves_scan(SyncState *state);

// Compute SHA-256 of a save file
int saves_compute_hash(const char *path, uint8_t *hash);

// Compute hash for a title if not already calculated
int saves_ensure_hash(Title *title);

// Read save from cartridge
int saves_read_cartridge(uint8_t *buffer, uint32_t *size);

// Write save to cartridge
int saves_write_cartridge(const uint8_t *buffer, uint32_t size);

#endif
