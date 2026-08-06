#ifndef WIIUSYNC_APPSTATE_H
#define WIIUSYNC_APPSTATE_H

#include <stdbool.h>

/*
 * Process lifecycle.
 *
 * There are two hosting environments and they need different handling — this
 * is what kept the app on a black screen when it tried to exit:
 *
 *   Aroma          The homebrew_kernel module is loaded.  Drive ProcUI
 *                  directly (ProcUIInit / ProcUIProcessMessages /
 *                  ProcUIShutdown) and leave the HOME menu ENABLED — the
 *                  real overlay works here.  WHBProc* must not be used: its
 *                  exit path relaunches only when it recognises one of three
 *                  hard-coded launcher title ids, and under Aroma it does
 *                  not, so nothing ever hands the foreground back.
 *
 *   Legacy / HBL   No homebrew_kernel.  libwhb's WHBProc* is correct: it
 *                  disables the overlay, turns the denied HOME press into a
 *                  quit, and relaunches the launcher on shutdown.
 *
 * Modelled on SaveMii's StateUtils, which does the same job (Wii U + vWii
 * saves through libmocha) and exits cleanly on both.
 */

void app_init(void);
bool app_running(void);
void app_shutdown(void);

/* True when running under Aroma (homebrew_kernel present). */
bool app_is_aroma(void);

#endif /* WIIUSYNC_APPSTATE_H */
