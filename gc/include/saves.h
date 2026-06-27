#ifndef GCSYNC_SAVES_H
#define GCSYNC_SAVES_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

/*
 * GameCube memory-card save sync.
 *
 * A GC save = one memory-card directory entry (the 64-byte card_direntry, which
 * IS the GCI header) plus its data blocks (block_count * 8 KB).  The server
 * keys saves by title_id "GC_<4-char gamecode>" and exchanges the compact GCI:
 *
 *   POST /saves/{tid}/gc-card?format=gci   — body = GCI (header + data)
 *   GET  /saves/{tid}/gc-card?format=gci   — returns the GCI
 *
 * Read/written file-level via libogc CARD_* — no client-side FS parsing.
 */

#define SAVES_MAX_CARD   127
#define SAVES_GCI_MAX    (2 * 1024 * 1024)   /* per-save GCI cap */
#define GC_BLOCK_SIZE    8192

typedef struct {
    char     gamecode[8];     /* 4-char game code + null (e.g. "GZLE") */
    char     company[4];      /* 2-char maker code + null */
    char     filename[40];    /* on-card filename (<=32) + null */
    char     title_id[16];    /* "GC_GZLE" — server key */
    char     name[64];        /* game name from server (or "") */
    int      fileno;          /* directory index */
    int      blocks;          /* block count */
    uint32_t size;            /* bytes = blocks * 8 KB */
} GcSave;

typedef struct {
    GcSave items[SAVES_MAX_CARD];
    int    count;
    int    port;              /* 0 = slot A, 1 = slot B */
    char   last_error[128];
} GcSaveList;

/* Enumerate save files on the memory card in the given slot. */
void saves_scan_card(int port, GcSaveList *out);

/* Read one card save as a GCI and POST it to the server.
 * Returns 0 on success, negative on error; ``msg`` gets a result line. */
int  saves_upload_card_game(const SyncState *state, int port, const GcSave *save,
                            char *msg, size_t msg_size);

/* Download one game's GCI from the server and write it onto the card.
 * Returns 0 on success, negative on error. */
int  saves_restore_card_game(const SyncState *state, int port, const char *title_id,
                             char *msg, size_t msg_size);

/* ---- server saves (shared "Server" view) ---- */

#define SAVES_MAX_SERVER 512

typedef struct {
    char     title_id[16];    /* "GC_GZLE" */
    char     name[64];        /* game name (or title_id if unknown) */
    uint32_t timestamp;       /* server client_timestamp */
    bool     local;           /* present on a scanned memory card */
} ServerSave;

typedef struct {
    ServerSave items[SAVES_MAX_SERVER];
    int        count;
    char       last_error[128];
} ServerSaveList;

/* Fetch all GC saves the server holds (GET /titles?console_type=GC). */
void saves_fetch_server(const SyncState *state,
                        char *scratch, uint32_t scratch_size,
                        ServerSaveList *out);

/* Build "GC_<gamecode>" from a 4-char game code. */
void saves_title_id_from_gamecode(const char *gamecode, char *out, size_t out_size);

#endif /* GCSYNC_SAVES_H */
