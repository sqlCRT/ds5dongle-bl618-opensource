#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Button remap table — maps 15 DualSense buttons to BTN or KBD       */
/* BTN: remap to another gamepad button                                */
/* KBD: emit HID keyboard keycode (+ optional modifier or 2nd key)    */
/* ------------------------------------------------------------------ */

#define REMAP_BTN_COUNT  15

/* Source button IDs (index into g_remap[]) */
#define REMAP_BTN_SQUARE    0
#define REMAP_BTN_CROSS     1
#define REMAP_BTN_CIRCLE    2
#define REMAP_BTN_TRIANGLE  3
#define REMAP_BTN_L1        4
#define REMAP_BTN_R1        5
#define REMAP_BTN_L2        6
#define REMAP_BTN_R2        7
#define REMAP_BTN_CREATE    8
#define REMAP_BTN_OPTIONS   9
#define REMAP_BTN_L3        10
#define REMAP_BTN_R3        11
#define REMAP_BTN_PS        12
#define REMAP_BTN_TP_CLICK  13
#define REMAP_BTN_MUTE      14

/* Target types */
#define REMAP_TYPE_BTN 0   /* value = target button ID 0-14 */
#define REMAP_TYPE_KBD 1   /* value = USB HID keycode       */

/* flags bit 0 */
#define REMAP_FLAG_SUPPRESS  0x01  /* suppress original gamepad bit output */
/* flags bit 1 */
#define REMAP_FLAG_EXTRA_KEY 0x02  /* modifier byte carries a 2nd keycode instead of modifier bitmask */

typedef struct {
    uint8_t type;      /* REMAP_TYPE_BTN or REMAP_TYPE_KBD */
    uint8_t value;     /* BTN: target btn ID; KBD: HID keycode (key1) */
    uint8_t modifier;  /* KBD: modifier bitmask — OR key2 when REMAP_FLAG_EXTRA_KEY is set */
    uint8_t flags;     /* bit0: REMAP_FLAG_SUPPRESS; bit1: REMAP_FLAG_EXTRA_KEY */
} remap_entry_t;

#define REMAP_PROFILE_COUNT 2

/* Public API */
void remap_init(void);
void remap_load(void);
bool remap_save(void);
bool remap_save_profile(uint8_t profile);
void remap_set(const uint8_t *data, uint8_t len);
void remap_set_profile(uint8_t profile, const uint8_t *data, uint8_t len);
void remap_reset(void);
void remap_reset_profile(uint8_t profile);
void remap_apply(uint8_t *payload);
void remap_kbd_tick(const uint8_t *payload);
void remap_on_disconnect(void);
bool remap_has_kbd_targets(void);
const remap_entry_t *remap_get_table(void);
const remap_entry_t *remap_get_profile_table(uint8_t profile);
uint8_t remap_get_active_profile(void);
void remap_switch_profile(uint8_t profile);
