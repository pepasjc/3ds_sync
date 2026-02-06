#ifndef TITLE_H
#define TITLE_H

#include "common.h"

// Scan for installed titles that have save data.
// Fills titles array, returns number found (up to MAX_TITLES).
int titles_scan(TitleInfo *titles, int max_titles);

// Format a u64 title ID as a 16-char uppercase hex string.
void title_id_to_hex(u64 title_id, char *out);

#endif // TITLE_H
