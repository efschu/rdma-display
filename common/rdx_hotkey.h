/* SPDX-License-Identifier: MIT
 *
 * The desktop hotkey of a client on a desktop PC (config "desktop_hotkey", default "ctrl+alt+h";
 * Ctrl+Alt+Shift+Z stays as an alias): "ctrl+alt+h" -> modifiers + one evdev key, matched against
 * the keys held on a device. Exact modifiers: Ctrl+Alt+Shift+H is not Ctrl+Alt+H. Header-only,
 * shared by p4/rdx_input_tx (evdev) and p4/rdx_input_win (X keycodes = evdev + 8).
 */
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <linux/input-event-codes.h>

#define RDX_HK_CTRL  1u
#define RDX_HK_ALT   2u
#define RDX_HK_SHIFT 4u
#define RDX_HK_META  8u

struct rdx_hotkey { unsigned mods; uint16_t key; };

static inline uint16_t rdx_hotkey_keycode(const char *n)
{
    static const struct { const char *name; uint16_t code; } T[] = {
        {"esc", KEY_ESC}, {"escape", KEY_ESC}, {"tab", KEY_TAB}, {"space", KEY_SPACE}, {"enter", KEY_ENTER},
        {"return", KEY_ENTER}, {"backspace", KEY_BACKSPACE}, {"insert", KEY_INSERT}, {"delete", KEY_DELETE},
        {"home", KEY_HOME}, {"end", KEY_END}, {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN},
        {"grave", KEY_GRAVE}, {"pause", KEY_PAUSE}, {"scrolllock", KEY_SCROLLLOCK}, {"left", KEY_LEFT},
        {"right", KEY_RIGHT}, {"up", KEY_UP}, {"down", KEY_DOWN}};
    static const uint16_t letters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K,
                                         KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V,
                                         KEY_W, KEY_X, KEY_Y, KEY_Z};
    static const uint16_t digits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
    static const uint16_t fkeys[12] = {KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
                                       KEY_F11, KEY_F12};
    if (strlen(n) == 1 && isalpha((unsigned char)n[0])) return letters[tolower((unsigned char)n[0]) - 'a'];
    if (strlen(n) == 1 && isdigit((unsigned char)n[0])) return digits[n[0] - '0'];
    if ((n[0] == 'f' || n[0] == 'F') && isdigit((unsigned char)n[1])) {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 12) return fkeys[f - 1];
    }
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (!strcasecmp(n, T[i].name)) return T[i].code;
    return 0;
}

/* "ctrl+alt+h" (case and spaces ignored, "control", "super"/"meta"/"win" too); 0 = ok */
static inline int rdx_hotkey_parse(const char *spec, struct rdx_hotkey *h)
{
    char buf[64], *save = NULL;
    h->mods = 0;
    h->key = 0;
    size_t j = 0;
    for (size_t i = 0; spec[i] && j + 1 < sizeof(buf); i++) if (!isspace((unsigned char)spec[i])) buf[j++] = (char)tolower((unsigned char)spec[i]);
    buf[j] = 0;
    for (char *t = strtok_r(buf, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        if (!strcmp(t, "ctrl") || !strcmp(t, "control")) h->mods |= RDX_HK_CTRL;
        else if (!strcmp(t, "alt")) h->mods |= RDX_HK_ALT;
        else if (!strcmp(t, "shift")) h->mods |= RDX_HK_SHIFT;
        else if (!strcmp(t, "super") || !strcmp(t, "meta") || !strcmp(t, "win")) h->mods |= RDX_HK_META;
        else if (h->key || !(h->key = rdx_hotkey_keycode(t))) return -1;    /* one key, a known one */
    }
    return h->key && h->mods ? 0 : -1;                /* a plain key would eat it from every app */
}

/* "Ctrl+Alt+H" */
static inline void rdx_hotkey_text(const char *spec, char *out, size_t n)
{
    size_t j = 0;
    int up = 1;
    for (size_t i = 0; spec[i] && j + 1 < n; i++) {
        char c = spec[i];
        if (isspace((unsigned char)c)) continue;
        out[j++] = up ? (char)toupper((unsigned char)c) : c;
        up = c == '+';
    }
    out[j] = 0;
}

/* the modifiers held, from a test of key codes (both sides of each) */
static inline unsigned rdx_hotkey_mods(int (*held)(void *user, uint16_t code), void *user)
{
    unsigned m = 0;
    if (held(user, KEY_LEFTCTRL) || held(user, KEY_RIGHTCTRL)) m |= RDX_HK_CTRL;
    if (held(user, KEY_LEFTALT) || held(user, KEY_RIGHTALT)) m |= RDX_HK_ALT;
    if (held(user, KEY_LEFTSHIFT) || held(user, KEY_RIGHTSHIFT)) m |= RDX_HK_SHIFT;
    if (held(user, KEY_LEFTMETA) || held(user, KEY_RIGHTMETA)) m |= RDX_HK_META;
    return m;
}

/* code was just pressed: one of the n hotkeys, with exactly its modifiers held */
static inline int rdx_hotkey_hit(const struct rdx_hotkey *hk, int n, uint16_t code, unsigned mods)
{
    for (int i = 0; i < n; i++) if (hk[i].key == code && hk[i].mods == mods) return 1;
    return 0;
}
