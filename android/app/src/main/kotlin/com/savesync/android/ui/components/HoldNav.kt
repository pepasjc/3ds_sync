package com.savesync.android.ui.components

import android.os.SystemClock
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEvent
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.type
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Uppercase first alphabetic character of [name], or "" if none. */
fun firstLetter(name: String): String {
    for (c in name) if (c.isLetter()) return c.uppercase()
    return ""
}

/**
 * Mutable state for d-pad left/right hold-to-accelerate navigation.
 *
 * Each screen wires its DirectionLeft/Right key handler to
 * [handleHorizontalHoldKeyEvent];
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
    var lastInput: Long = 0L
    var lastAction: Long = 0L
    private var pressedDir: Int = 0
    private var repeatJob: Job? = null

    fun onKeyDown(
        dir: Int,
        scope: CoroutineScope,
        onPage: (Int) -> Unit,
        onAlphabet: (Int) -> Unit,
    ) {
        if (pressedDir == dir && repeatJob?.isActive == true) return

        repeatJob?.cancel()
        pressedDir = dir
        onPress(dir, onPage, onAlphabet)
        repeatJob = scope.launch {
            delay(REPEAT_POLL_MS)
            while (isActive && pressedDir == dir) {
                onPress(dir, onPage, onAlphabet)
                delay(REPEAT_POLL_MS)
            }
        }
    }

    fun onKeyUp(dir: Int) {
        if (pressedDir != dir) return
        pressedDir = 0
        repeatJob?.cancel()
        repeatJob = null
    }
}

@Composable
fun rememberHoldNavState(): HoldNavState = remember { HoldNavState() }

private const val HOLD_FAST_AFTER_MS = 500L
private const val HOLD_ALPHA_AFTER_MS = 1500L
private const val FAST_CADENCE_MS = 100L
private const val ALPHA_CADENCE_MS = 250L
private const val REPEAT_POLL_MS = 50L
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
    val sinceLastInput = now - lastInput
    val freshPress = this.dir != dir || sinceLastInput > RESET_GAP_MS
    lastInput = now
    if (freshPress) {
        this.dir = dir
        heldSince = now
        lastAction = now
        onPage(dir)
        return
    }
    val held = now - heldSince
    val sinceLastAction = now - lastAction
    when {
        held >= HOLD_ALPHA_AFTER_MS -> {
            if (sinceLastAction >= ALPHA_CADENCE_MS) {
                lastAction = now
                onAlphabet(dir)
            }
        }
        held >= HOLD_FAST_AFTER_MS -> {
            if (sinceLastAction >= FAST_CADENCE_MS) {
                lastAction = now
                onPage(dir)
            }
        }
        // Still in the post-initial-press debounce window — drop the
        // event entirely.  The initial press already fired one onPage.
    }
}

fun HoldNavState.handleHorizontalHoldKeyEvent(
    event: KeyEvent,
    scope: CoroutineScope,
    onPage: (Int) -> Unit,
    onAlphabet: (Int) -> Unit,
): Boolean {
    val dir = when (event.key) {
        Key.DirectionLeft -> -1
        Key.DirectionRight -> 1
        else -> return false
    }
    return when (event.type) {
        KeyEventType.KeyDown -> {
            onKeyDown(dir, scope, onPage, onAlphabet)
            true
        }
        KeyEventType.KeyUp -> {
            onKeyUp(dir)
            true
        }
        else -> false
    }
}
