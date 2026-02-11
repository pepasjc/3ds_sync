#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

// Compute SHA-256 hash of data
void sha256_hash(const uint8_t *data, uint32_t len, uint8_t hash[32]);

#endif
