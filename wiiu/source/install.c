/*
 * install.c — hand a downloaded WUP folder to the system installer (MCP).
 *
 * Sequence and the alignment requirement are as used by WUP Installer GX2
 * (Dimok / Maschell, GPL-2.0-or-later) — MCP_InstallGetInfo and
 * MCP_InstallTitleAsync write into IOS-visible buffers, so the structs have
 * to come from the default heap at 0x40 alignment, not the stack.  Based on
 * analysis of that project; no code copied.
 *
 * Installing an unsigned title needs the IOSU install patch that Aroma /
 * Tiramisu apply.  Without it MCP rejects the title and we surface its error
 * rather than pretending the install worked.
 */

#include "install.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

bool install_fsa_path(const char *devoptab_path, char *out, size_t out_size) {
    if (!devoptab_path || !out) return false;

    /* "fs:/vol/external01/..." — wut's SD devoptab already carries the /vol
     * prefix, so dropping "fs:" is the whole translation. */
    if (!strncmp(devoptab_path, "fs:/", 4)) {
        snprintf(out, out_size, "%s", devoptab_path + 3);
        return true;
    }
    /* "usb:/..." — our own libmocha mount, made at /vol/usb. */
    if (!strncmp(devoptab_path, USB_ROOT_DEFAULT "/", sizeof(USB_ROOT_DEFAULT))) {
        snprintf(out, out_size, "/vol/" USB_FAT_NAME "/%s",
                 devoptab_path + sizeof(USB_ROOT_DEFAULT));
        return true;
    }
    /* Already an FSA path. */
    if (devoptab_path[0] == '/') {
        snprintf(out, out_size, "%s", devoptab_path);
        return true;
    }
    return false;
}

int install_wup_folder(const char *dir, const char *target,
                       InstallProgress *progress,
                       void (*on_poll)(const InstallProgress *)) {
    InstallProgress local;
    if (!progress) progress = &local;
    memset(progress, 0, sizeof(*progress));

    if (!dir || !dir[0]) {
        snprintf(progress->message, sizeof(progress->message), "No folder given");
        progress->done = true;
        return -1;
    }

    char fsa[SAVE_DIR_LEN];
    if (!install_fsa_path(dir, fsa, sizeof(fsa))) {
        snprintf(progress->message, sizeof(progress->message),
                 "Unsupported storage root: %s", dir);
        progress->done = true;
        return -1;
    }

    int handle = MCP_Open();
    if (handle < 0) {
        snprintf(progress->message, sizeof(progress->message),
                 "MCP_Open failed (%d)", handle);
        progress->done = true;
        return -1;
    }

    /* IOS writes these, so they must be heap allocated at 0x40 alignment. */
    MCPInstallInfo      *info = MEMAllocFromDefaultHeapEx(sizeof(MCPInstallInfo), 0x40);
    MCPInstallTitleInfo *tinfo = MEMAllocFromDefaultHeapEx(sizeof(MCPInstallTitleInfo), 0x40);
    MCPInstallProgress  *prog = MEMAllocFromDefaultHeapEx(sizeof(MCPInstallProgress), 0x40);
    int rc = -1;

    if (!info || !tinfo || !prog) {
        snprintf(progress->message, sizeof(progress->message),
                 "Out of memory for MCP buffers");
        goto out;
    }
    memset(info, 0, sizeof(*info));
    memset(tinfo, 0, sizeof(*tinfo));
    memset(prog, 0, sizeof(*prog));

    /* Validates the WUP set (tmd/tik/cert present and readable) before we
     * commit to a multi-GB write.  A bad dump fails here, cheaply. */
    MCPError err = MCP_InstallGetInfo(handle, fsa, info);
    if (err < 0) {
        snprintf(progress->message, sizeof(progress->message),
                 "Not installable (MCP %d) — check title.tmd/tik at %s",
                 (int)err, fsa);
        rc = (int)err;
        goto out;
    }

    bool to_usb = target && !strcasecmp(target, "usb");
    err = MCP_InstallSetTargetDevice(handle,
                                     to_usb ? MCP_INSTALL_TARGET_USB
                                            : MCP_INSTALL_TARGET_MLC);
    if (err < 0) {
        snprintf(progress->message, sizeof(progress->message),
                 "Target %s rejected (MCP %d)%s",
                 to_usb ? "USB" : "NAND", (int)err,
                 to_usb ? " — is a Wii U formatted USB drive attached?" : "");
        rc = (int)err;
        goto out;
    }
    err = MCP_InstallSetTargetUsb(handle, to_usb ? 1 : 0);
    if (err < 0) {
        snprintf(progress->message, sizeof(progress->message),
                 "MCP_InstallSetTargetUsb failed (%d)", (int)err);
        rc = (int)err;
        goto out;
    }

    progress->running = true;
    err = MCP_InstallTitleAsync(handle, fsa, tinfo);
    if (err < 0) {
        progress->running = false;
        snprintf(progress->message, sizeof(progress->message),
                 "Install refused (MCP %d) — IOSU install patch active?",
                 (int)err);
        rc = (int)err;
        goto out;
    }

    /* Poll until MCP reports it is no longer in progress.  ``inProgress``
     * starts at 0 for a moment before IOS picks the job up, so wait for it
     * to go high once before treating a 0 as completion. */
    bool started = false;
    for (;;) {
        memset(prog, 0, sizeof(*prog));
        if (MCP_InstallGetProgress(handle, prog) < 0) break;

        if (prog->inProgress) {
            started = true;
            progress->size_total     = prog->sizeTotal;
            progress->size_done      = prog->sizeProgress;
            progress->contents_total = prog->contentsTotal;
            progress->contents_done  = prog->contentsProgress;
            if (on_poll) on_poll(progress);
        } else if (started) {
            break;
        }
        OSSleepTicks(OSMillisecondsToTicks(500));
    }

    progress->running = false;
    rc = 0;
    snprintf(progress->message, sizeof(progress->message),
             "Installed to %s", to_usb ? "USB" : "NAND");

out:
    if (info)  MEMFreeToDefaultHeap(info);
    if (tinfo) MEMFreeToDefaultHeap(tinfo);
    if (prog)  MEMFreeToDefaultHeap(prog);
    MCP_Close(handle);

    progress->done = true;
    progress->result = rc;
    if (!progress->message[0])
        snprintf(progress->message, sizeof(progress->message),
                 rc == 0 ? "Installed" : "Install failed (%d)", rc);
    if (on_poll) on_poll(progress);
    return rc;
}
