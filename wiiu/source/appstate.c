/*
 * appstate.c — ProcUI lifecycle, split by hosting environment.  See appstate.h.
 */

#include "appstate.h"

#include <coreinit/core.h>
#include <coreinit/dynload.h>
#include <coreinit/foreground.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <whb/log.h>
#include <whb/proc.h>

static bool g_aroma = false;
static bool g_shutdown_done = false;

bool app_is_aroma(void) { return g_aroma; }

void app_init(void) {
    /* Aroma ships a "homebrew_kernel" RPL; its presence is the standard way
     * homebrew tells the two environments apart. */
    OSDynLoad_Module mod;
    g_aroma = (OSDynLoad_Acquire("homebrew_kernel", &mod) == OS_DYNLOAD_OK);

    if (g_aroma) {
        OSDynLoad_Release(mod);
        ProcUIInit(&OSSavesDone_ReadyToRelease);
        /* The overlay genuinely works under Aroma, so HOME behaves the way
         * the user expects instead of being hijacked as a quit button. */
        OSEnableHomeButtonMenu(TRUE);
    } else {
        WHBProcInit();
    }

    WHBLogPrintf("proc: %s environment",
                 g_aroma ? "Aroma (direct ProcUI)" : "legacy/HBL (libwhb)");
}

bool app_running(void) {
    if (!g_aroma) return WHBProcIsRunning();

    /* ProcUI messages are only pumped on the main core. */
    if (!OSIsMainCore()) return true;

    switch (ProcUIProcessMessages(TRUE)) {
        case PROCUI_STATUS_EXITING:
            return false;
        case PROCUI_STATUS_RELEASE_FOREGROUND:
            /* MEM1 is handed back by the registered RELEASE callback; this
             * tells the system we are done with it. */
            ProcUIDrawDoneRelease();
            break;
        case PROCUI_STATUS_IN_BACKGROUND:
            OSSleepTicks(OSMillisecondsToTicks(20));
            break;
        case PROCUI_STATUS_IN_FOREGROUND:
        default:
            break;
    }
    return true;
}

void app_shutdown(void) {
    if (g_shutdown_done) return;
    g_shutdown_done = true;

    /* WHBProcShutdown must NOT run under Aroma — it would fire a competing
     * SYSRelaunchTitle.  ProcUIShutdown runs in both cases. */
    if (!g_aroma) WHBProcShutdown();
    ProcUIShutdown();
}
