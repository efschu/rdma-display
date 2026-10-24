/* SPDX-License-Identifier: MIT
 * The desktop hotkey (common/rdx_hotkey.h): parsing, exact modifiers, the text shown.
 *   tests/test_hotkey      exit 0 = PASS */
#include "../common/rdx_hotkey.h"
#include <stdio.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static uint8_t keys[(KEY_CNT + 7) / 8];
static int held(void *u, uint16_t c) { (void)u; return (keys[c / 8] >> (c % 8)) & 1; }
static void press(uint16_t c, int down) { if (down) keys[c / 8] |= (uint8_t)(1u << (c % 8)); else keys[c / 8] &= (uint8_t)~(1u << (c % 8)); }

int main(void)
{
    struct rdx_hotkey h[2];
    check(!rdx_hotkey_parse("ctrl+alt+h", &h[0]) && h[0].key == KEY_H && h[0].mods == (RDX_HK_CTRL | RDX_HK_ALT), "ctrl+alt+h");
    check(!rdx_hotkey_parse(" Ctrl + Alt + Shift + Z ", &h[1]) && h[1].key == KEY_Z &&
          h[1].mods == (RDX_HK_CTRL | RDX_HK_ALT | RDX_HK_SHIFT), "Ctrl+Alt+Shift+Z (case, spaces)");
    struct rdx_hotkey t;
    check(!rdx_hotkey_parse("super+f12", &t) && t.key == KEY_F12 && t.mods == RDX_HK_META, "super+f12");
    check(rdx_hotkey_parse("h", &t) != 0, "a plain key is refused");
    check(rdx_hotkey_parse("ctrl+alt+h+j", &t) != 0, "two keys are refused");
    check(rdx_hotkey_parse("ctrl+alt+nokey", &t) != 0, "an unknown key is refused");
    press(KEY_LEFTCTRL, 1);
    press(KEY_RIGHTALT, 1);
    check(rdx_hotkey_hit(h, 2, KEY_H, rdx_hotkey_mods(held, NULL)), "LeftCtrl+RightAlt+H hits");
    press(KEY_LEFTSHIFT, 1);
    check(!rdx_hotkey_hit(h, 2, KEY_H, rdx_hotkey_mods(held, NULL)), "with Shift too it does not (exact)");
    check(rdx_hotkey_hit(h, 2, KEY_Z, rdx_hotkey_mods(held, NULL)), "the alias Ctrl+Alt+Shift+Z hits");
    char txt[32];
    rdx_hotkey_text("ctrl+alt+h", txt, sizeof(txt));
    check(!strcmp(txt, "Ctrl+Alt+H"), "shown as Ctrl+Alt+H");
    printf("test_hotkey: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
