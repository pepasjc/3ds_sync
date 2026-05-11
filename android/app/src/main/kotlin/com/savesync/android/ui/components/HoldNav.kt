package com.savesync.android.ui.components

import android.os.SystemClock
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember

/** Uppercase first alphabetic character of [name], or "" if none. */
fun firstLetter(name: String): String {
    for (c in name) if (c.isLetter()) return c.uppercase()
    return ""
}

/**
 * Mutable state for d-pad left/right hold-to-accelerate navigation.
 *
 * Each screen wires its DirectionLeft/Right key handler to [onPress];
 * after the user has been holding for longer than [HOLD_FAST_AFTER_MS]
 * the page scroll cadence speeds up, and past [HOLD_ALPHA_AFTER_MS] we
 * switch to alphabet jumps so the user can sweep a long list in seconds.
 *
 * Mirrors the Steam Deck client's `_nav_x_*` polling state so both apps
 * feel identical when you keep d-pad left/right held.
 */
class HoldNavState {
    var dir: Int = 0
    var heldSince: Long = 0L
    var lastAction: Long = 0L
}

@Composable
fun rememberHoldNavState(): HoldNavState = remember { HoldNavState() }

private const val HOLD_FAST_AFTER_MS = 500L
private const val HOLD_ALPHA_AFTER_MS = 1500L
private const val FAST_CADENCE_MS = 100L
private const val ALPHA_CADENCE_MS = 250L
// Gap after which a subsequent press is treated as a brand-new press
// (user lifted the d-pad and re-pressed).  Slightly larger than the
// analog-stick synth repeat interval (150 ms) so a smooth hold via the
// stick still counts as continuous.
private const val RESET_GAP_MS = 350L

/**
 * Run one d-pad-horizontal step through the hold ramp.  Pass [onPage]
 * and [onAlphabet] callbacks; this function picks the right one based
 * on how long the user has been holding the same direction.
 */
fun HoldNavState.onPress(
    dir: Int,
    onPage: (Int) -> Unit,
    onAlphabet: (Int) -> Unit,
) {
    val now = SystemClock.uptimeMillis()
    val sinceLast = now - lastAction
    val freshPress = this.dir != dir || sinceLast > RESET_GAP_MS
    if (freshPress) {
        this.dir = dir
        heldSince = now
        lastAction = now
        onPage(dir)
        return
    }
    val held = now - heldSince
    when {
        held >= HOLD_ALPHA_AFTER_MS -> {
            if (sinceLast >= ALPHA_CADENCE_MS) {
                lastAction = now
                onAlphabet(dir)
            }
        }
        held >= HOLD_FAST_AFTER_MS -> {
            if (sinceLast >= FAST_CADENCE_MS) {
                lastAction = now
                onPage(dir)
            }
        }
        // Still in the post-initial-press debounce window — drop the
        // event entirely.  The initial press already fired one onPage.
    }
}
