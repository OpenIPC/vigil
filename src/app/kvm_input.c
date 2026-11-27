/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm_input.c — browser input as evdev events (kvm_input.h). */
#include <string.h>
#include <linux/input.h>

#include "cJSON.h"
#include "kvm_input.h"

static const struct { const char *name; unsigned short key; } k_keys[] = {
    { "Escape", KEY_ESC }, { "Digit1", KEY_1 }, { "Digit2", KEY_2 }, { "Digit3", KEY_3 },
    { "Digit4", KEY_4 }, { "Digit5", KEY_5 }, { "Digit6", KEY_6 }, { "Digit7", KEY_7 },
    { "Digit8", KEY_8 }, { "Digit9", KEY_9 }, { "Digit0", KEY_0 }, { "Minus", KEY_MINUS },
    { "Equal", KEY_EQUAL }, { "Backspace", KEY_BACKSPACE }, { "Tab", KEY_TAB }, { "KeyQ", KEY_Q },
    { "KeyW", KEY_W }, { "KeyE", KEY_E }, { "KeyR", KEY_R }, { "KeyT", KEY_T }, { "KeyY", KEY_Y },
    { "KeyU", KEY_U }, { "KeyI", KEY_I }, { "KeyO", KEY_O }, { "KeyP", KEY_P },
    { "BracketLeft", KEY_LEFTBRACE }, { "BracketRight", KEY_RIGHTBRACE }, { "Enter", KEY_ENTER },
    { "ControlLeft", KEY_LEFTCTRL }, { "KeyA", KEY_A }, { "KeyS", KEY_S }, { "KeyD", KEY_D },
    { "KeyF", KEY_F }, { "KeyG", KEY_G }, { "KeyH", KEY_H }, { "KeyJ", KEY_J }, { "KeyK", KEY_K },
    { "KeyL", KEY_L }, { "Semicolon", KEY_SEMICOLON }, { "Quote", KEY_APOSTROPHE },
    { "Backquote", KEY_GRAVE }, { "ShiftLeft", KEY_LEFTSHIFT }, { "Backslash", KEY_BACKSLASH },
    { "KeyZ", KEY_Z }, { "KeyX", KEY_X }, { "KeyC", KEY_C }, { "KeyV", KEY_V }, { "KeyB", KEY_B },
    { "KeyN", KEY_N }, { "KeyM", KEY_M }, { "Comma", KEY_COMMA }, { "Period", KEY_DOT },
    { "Slash", KEY_SLASH }, { "ShiftRight", KEY_RIGHTSHIFT }, { "NumpadMultiply", KEY_KPASTERISK },
    { "AltLeft", KEY_LEFTALT }, { "Space", KEY_SPACE }, { "CapsLock", KEY_CAPSLOCK },
    { "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 }, { "F4", KEY_F4 }, { "F5", KEY_F5 },
    { "F6", KEY_F6 }, { "F7", KEY_F7 }, { "F8", KEY_F8 }, { "F9", KEY_F9 }, { "F10", KEY_F10 },
    { "NumLock", KEY_NUMLOCK }, { "Numpad7", KEY_KP7 }, { "Numpad8", KEY_KP8 }, { "Numpad9", KEY_KP9 },
    { "NumpadSubtract", KEY_KPMINUS }, { "Numpad4", KEY_KP4 }, { "Numpad5", KEY_KP5 },
    { "Numpad6", KEY_KP6 }, { "NumpadAdd", KEY_KPPLUS }, { "Numpad1", KEY_KP1 }, { "Numpad2", KEY_KP2 },
    { "Numpad3", KEY_KP3 }, { "Numpad0", KEY_KP0 }, { "NumpadDecimal", KEY_KPDOT },
    { "IntlBackslash", KEY_102ND }, { "F11", KEY_F11 }, { "F12", KEY_F12 },
    { "NumpadEnter", KEY_KPENTER }, { "ControlRight", KEY_RIGHTCTRL }, { "NumpadDivide", KEY_KPSLASH },
    { "AltRight", KEY_RIGHTALT }, { "Home", KEY_HOME }, { "ArrowUp", KEY_UP }, { "PageUp", KEY_PAGEUP },
    { "ArrowLeft", KEY_LEFT }, { "ArrowRight", KEY_RIGHT }, { "End", KEY_END }, { "ArrowDown", KEY_DOWN },
    { "PageDown", KEY_PAGEDOWN }, { "Insert", KEY_INSERT }, { "Delete", KEY_DELETE },
    { "NumpadEqual", KEY_KPEQUAL }, { "ContextMenu", KEY_COMPOSE },
};

unsigned kvm_key_code(const char *code)
{
    for (size_t i = 0; code && i < sizeof k_keys / sizeof k_keys[0]; ++i)
        if (!strcmp(k_keys[i].name, code)) return k_keys[i].key;
    return 0;
}

static int put(kvm_ev_t *out, int n, int max, int dev, unsigned short type, unsigned short code, int value)
{
    if (n >= max) return n;
    out[n] = (kvm_ev_t){ dev, type, code, value };
    return n + 1;
}

static int num(const cJSON *o, const char *k, double *v)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(j)) return -1;
    *v = j->valuedouble;
    return 0;
}

static int clampi(double v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

int kvm_input_events(const char *json, unsigned long len, unsigned w, unsigned h, kvm_ev_t *out, int max)
{
    cJSON *o = cJSON_ParseWithLength(json, len);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "t");
    int n = -1;
    double x, y, v;
    if (!cJSON_IsString(t)) {
        n = -1;
    } else if (!strcmp(t->valuestring, "move")) {
        if (num(o, "x", &x) == 0 && num(o, "y", &y) == 0 && w && h) {
            n = put(out, 0, max, KVM_DEV_POINTER, EV_ABS, ABS_X, clampi(x, 0, (int)w - 1));
            n = put(out, n, max, KVM_DEV_POINTER, EV_ABS, ABS_Y, clampi(y, 0, (int)h - 1));
            n = put(out, n, max, KVM_DEV_POINTER, EV_SYN, SYN_REPORT, 0);
        }
    } else if (!strcmp(t->valuestring, "btn")) {
        static const unsigned short btn[3] = { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT };
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(o, "down");
        if (num(o, "b", &v) == 0 && v >= 0 && v <= 2 && cJSON_IsBool(d)) {
            n = put(out, 0, max, KVM_DEV_POINTER, EV_KEY, btn[(int)v], cJSON_IsTrue(d) ? 1 : 0);
            n = put(out, n, max, KVM_DEV_POINTER, EV_SYN, SYN_REPORT, 0);
        }
    } else if (!strcmp(t->valuestring, "wheel")) {
        if (num(o, "n", &v) == 0) {
            int steps = clampi(v, -10, 10);
            n = 0;
            if (steps) {
                n = put(out, 0, max, KVM_DEV_POINTER, EV_REL, REL_WHEEL, steps);
                n = put(out, n, max, KVM_DEV_POINTER, EV_SYN, SYN_REPORT, 0);
            }
        }
    } else if (!strcmp(t->valuestring, "key")) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "code");
        if (cJSON_IsString(c) && num(o, "v", &v) == 0 && v >= 0 && v <= 2) {
            unsigned k = kvm_key_code(c->valuestring);
            n = 0;                          /* a key the console has no use for */
            if (k) {
                n = put(out, 0, max, KVM_DEV_KEYBOARD, EV_KEY, (unsigned short)k, (int)v);
                n = put(out, n, max, KVM_DEV_KEYBOARD, EV_SYN, SYN_REPORT, 0);
            }
        }
    }
    cJSON_Delete(o);
    return n;
}
