#ifndef WIIUSYNC_INSTALL_H
#define WIIUSYNC_INSTALL_H

#include "common.h"

/*
 * Wii U title installation via MCP.
 *
 * A WUP/NUS folder downloaded from the catalog (<install>/<Name>/ holding
 * title.tmd, title.tik, title.cert and the numbered .app contents) is handed
 * to the system's own installer, exactly as WUP Installer GX2 does:
 *
 *   1. MCP_InstallSetTargetDevice   — NAND (MLC) or the console's USB drive
 *   2. MCP_InstallSetTargetUsb      — which USB device, when target is USB
 *   3. MCP_InstallTitleAsync        — kick it off, then poll progress
 *
 * The path MCP receives is an *FSA* path, not a devoptab path: it wants
 * "/vol/external01/install/Foo" or "/vol/usb/install/Foo", never
 * "fs:/vol/external01/..." or "usb:/...".  install_fsa_path() does that
 * translation — getting it wrong is the classic -21 (invalid arg) failure.
 *
 * IMPORTANT: MCP validates the ticket.  A title dumped without title.tik, or
 * one whose ticket does not match this console's region/keys, is rejected by
 * the system installer and nothing we do client-side changes that.
 */

typedef struct {
    bool     running;
    bool     done;
    int      result;              /* 0 = installed, negative = MCP error */
    uint64_t size_total;
    uint64_t size_done;
    uint32_t contents_total;
    uint32_t contents_done;
    char     message[160];
} InstallProgress;

/* Translate a devoptab path ("fs:/vol/external01/install/Foo", "usb:/install/
 * Foo") into the FSA path MCP expects ("/vol/external01/install/Foo",
 * "/vol/usb/install/Foo").  Returns false when the root is unrecognised. */
bool install_fsa_path(const char *devoptab_path, char *out, size_t out_size);

/*
 * Install the WUP folder at ``dir`` (a devoptab path).  ``target`` is "mlc"
 * or "usb" and selects where the title lands, independent of where the WUP
 * folder was staged.  Blocks until the install finishes, calling ``on_poll``
 * (may be NULL) roughly twice a second so the caller can repaint.
 *
 * Returns 0 on success; ``progress->message`` always carries a result line.
 */
int install_wup_folder(const char *dir, const char *target,
                       InstallProgress *progress,
                       void (*on_poll)(const InstallProgress *));

#endif /* WIIUSYNC_INSTALL_H */
