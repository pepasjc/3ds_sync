"""Controller-family detection drives the on-screen button names.

The kernel maps every pad onto the same positional BTN_* codes, so only the
labels differ: BTN_SOUTH is "A" on an Xbox pad, "Cross" on a DualShock and
"B" on a Switch Pro Controller. Getting this wrong tells a user to press a
button they do not have. No hardware involved: the detection is a pure
function of the evdev name and USB vendor id.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for candidate in (ROOT, ROOT / "mister"):
    if str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from gamesync import input as gsinput  # noqa: E402
from gamesync.input import (  # noqa: E402
    BTN_EAST, BTN_SOUTH, BTN_TL2, BTN_START, Device, InputReader,
    LAYOUT_GENERIC, LAYOUT_NINTENDO, LAYOUT_PLAYSTATION, LAYOUT_XBOX,
    button_label, detect_layout,
)


def test_vendor_id_wins_over_a_generic_name():
    # A DualShock 4 over Bluetooth is named just "Wireless Controller";
    # an 8BitDo in Switch mode calls itself "Pro Controller" with Nintendo's id.
    assert detect_layout("Wireless Controller", 0x054C) == LAYOUT_PLAYSTATION
    assert detect_layout("Pro Controller", 0x057E) == LAYOUT_NINTENDO
    assert detect_layout("Controller", 0x045E) == LAYOUT_XBOX


def test_name_fallback_when_vendor_is_unknown():
    assert detect_layout("DualSense Wireless Controller") == LAYOUT_PLAYSTATION
    assert detect_layout("Sony Interactive Entertainment Wireless Controller") \
        == LAYOUT_PLAYSTATION
    assert detect_layout("Nintendo Co., Ltd. Pro Controller") == LAYOUT_NINTENDO
    assert detect_layout("Microsoft X-Box 360 pad") == LAYOUT_XBOX
    assert detect_layout("Xbox Wireless Controller") == LAYOUT_XBOX
    assert detect_layout("GP2040-CE (XInput)") == LAYOUT_XBOX
    # hid-sony's Bluetooth name for a DualShock 4 is just this.
    assert detect_layout("Wireless Controller") == LAYOUT_PLAYSTATION


def test_unknown_pad_is_generic():
    assert detect_layout("USB Gamepad", 0x0079) == LAYOUT_GENERIC
    assert detect_layout("", 0) == LAYOUT_GENERIC


def test_same_code_different_label_per_family():
    assert button_label(BTN_SOUTH, LAYOUT_XBOX) == "A"
    assert button_label(BTN_SOUTH, LAYOUT_PLAYSTATION) == "Cross"
    assert button_label(BTN_SOUTH, LAYOUT_NINTENDO) == "B"
    assert button_label(BTN_EAST, LAYOUT_NINTENDO) == "A"
    assert button_label(BTN_TL2, LAYOUT_XBOX) == "LT"
    assert button_label(BTN_TL2, LAYOUT_NINTENDO) == "ZL"
    assert button_label(BTN_START, LAYOUT_PLAYSTATION) == "Options"


def test_unlisted_code_falls_back_to_generic_name():
    # L1/R1 are not overridden for PlayStation because they are already right.
    assert button_label(gsinput.BTN_TL, LAYOUT_PLAYSTATION) == "L1"
    assert button_label(0x2C0, LAYOUT_XBOX) == "0x2c0"


def make_reader(*devices):
    reader = InputReader.__new__(InputReader)
    reader._devices = list(devices)
    reader._by_fd = {}
    reader._active_pad = None
    reader._capture = None
    reader._pending = []
    reader._last_action = {}
    reader.set_buttons(None, arcade=False)
    return reader


def pad(path, name, layout):
    return Device(path, name, fd=-1, is_pad=True, is_keyboard=False, axes={},
                  layout=layout)


def test_labels_follow_the_pad_that_was_last_pressed():
    xbox = pad("/dev/input/event3", "Xbox Wireless Controller", LAYOUT_XBOX)
    ds = pad("/dev/input/event4", "DualSense", LAYOUT_PLAYSTATION)
    reader = make_reader(xbox, ds)

    # Nothing pressed yet: the first pad is described.
    assert reader.layout == LAYOUT_XBOX
    assert reader.label(gsinput.PRIMARY) == "A"

    reader._on_key(ds, BTN_SOUTH, 1)
    assert reader.layout == LAYOUT_PLAYSTATION
    assert reader.label(gsinput.PRIMARY) == "Cross"
    assert reader.label(gsinput.BACK) == "Circle"
    assert reader.layout_description().startswith("PlayStation - DualSense")

    # The pad that was in hand went away: fall back rather than remember it.
    reader._devices = [xbox]
    assert reader.layout == LAYOUT_XBOX


def test_arcade_map_follows_the_pad_not_the_screen():
    """Seen live on a 240p CRT: a DualShock got the cabinet map, so its
    footer read "C Sync all / Square/Z Tab" and R2 opened Settings."""
    from gamesync.input import BTN_C, BTN_Z, BTN_TR2, is_arcade_pad

    keys_generic = {BTN_SOUTH, BTN_EAST, BTN_C, gsinput.BTN_NORTH,
                    gsinput.BTN_WEST, BTN_Z, gsinput.BTN_TL, gsinput.BTN_TR}
    keys_console = keys_generic - {BTN_C, BTN_Z}
    assert is_arcade_pad(keys_generic, LAYOUT_GENERIC) is True
    assert is_arcade_pad(keys_console, LAYOUT_GENERIC) is False
    # A dedicated driver's layout wins even with BTN_Z present (Switch Capture).
    assert is_arcade_pad(keys_generic | {BTN_Z}, LAYOUT_NINTENDO) is False

    ds = pad("/dev/input/event4", "DualSense", LAYOUT_PLAYSTATION)
    stick = pad("/dev/input/event5", "GP2040-CE", LAYOUT_GENERIC)
    stick.arcade = True
    reader = make_reader(ds, stick)

    reader._on_key(ds, BTN_TR2, 1)
    assert reader._pending[-1] == gsinput.NEXT_TAB      # console map: R2 = tab
    assert reader.label(gsinput.SYNC) == "Square"
    reader._on_key(stick, BTN_TR2, 1)
    assert reader._pending[-1] == gsinput.SETTINGS      # arcade map: S2 = start
    assert reader.label(gsinput.SYNC) == "C"
    assert "arcade map" in reader.layout_description()


def test_keyboard_only_is_generic():
    keyboard = Device("/dev/input/event0", "AT Keyboard", fd=-1, is_pad=False,
                      is_keyboard=True, axes={})
    reader = make_reader(keyboard)
    assert reader.layout == LAYOUT_GENERIC
    assert reader.layout_description() == "no controller"
    assert reader.label(gsinput.PRIMARY) == "A"
