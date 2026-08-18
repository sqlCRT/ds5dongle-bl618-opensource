#include "remap.h"
#include "usb_gamepad.h"
#include "debug_log.h"
#include "easyflash.h"
#include <string.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Bit extraction helpers: indices into the 63-byte USB input payload  */
/* p[7]:  dpad[3:0]  ■[4] ✕[5] ○[6] △[7]                           */
/* p[8]:  L1[0] R1[1] L2[2] R2[3] Create[4] Options[5] L3[6] R3[7]  */
/* p[9]:  PS[0] TP_click[1] Mute[2]                                   */
/* ------------------------------------------------------------------ */

static const struct { uint8_t off; uint8_t mask; } BTN_LOC[REMAP_BTN_COUNT] = {
    [REMAP_BTN_SQUARE]   = { 7, 0x10 },
    [REMAP_BTN_CROSS]    = { 7, 0x20 },
    [REMAP_BTN_CIRCLE]   = { 7, 0x40 },
    [REMAP_BTN_TRIANGLE] = { 7, 0x80 },
    [REMAP_BTN_L1]       = { 8, 0x01 },
    [REMAP_BTN_R1]       = { 8, 0x02 },
    [REMAP_BTN_L2]       = { 8, 0x04 },
    [REMAP_BTN_R2]       = { 8, 0x08 },
    [REMAP_BTN_CREATE]   = { 8, 0x10 },
    [REMAP_BTN_OPTIONS]  = { 8, 0x20 },
    [REMAP_BTN_L3]       = { 8, 0x40 },
    [REMAP_BTN_R3]       = { 8, 0x80 },
    [REMAP_BTN_PS]       = { 9, 0x01 },
    [REMAP_BTN_TP_CLICK] = { 9, 0x02 },
    [REMAP_BTN_MUTE]     = { 9, 0x04 },
};

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static remap_entry_t g_profiles[REMAP_PROFILE_COUNT][REMAP_BTN_COUNT];
static uint8_t active_profile = 0;
static bool g_remap_is_identity = true;
static uint8_t last_kbd_report[8];

static const char *profile_keys[REMAP_PROFILE_COUNT] = {
    "btn_remap",      /* profile 0 — backward compatible key */
    "btn_remap_1",    /* profile 1 */
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static bool validate_entry(const remap_entry_t *e)
{
    if (e->type == REMAP_TYPE_BTN)
        return e->value < REMAP_BTN_COUNT;
    if (e->type == REMAP_TYPE_KBD)
        return e->value > 0 && e->value <= 0x73;
    return false;
}

static void sanitize_entry(remap_entry_t *e, uint8_t src_id)
{
    if (!validate_entry(e))
        *e = (remap_entry_t){ REMAP_TYPE_BTN, src_id, 0, 0 };
}

static void reset_table(remap_entry_t *table)
{
    for (int i = 0; i < REMAP_BTN_COUNT; i++)
        table[i] = (remap_entry_t){ REMAP_TYPE_BTN, (uint8_t)i, 0, 0 };
}

static bool is_table_identity(const remap_entry_t *table)
{
    for (int i = 0; i < REMAP_BTN_COUNT; i++) {
        if (table[i].type   != REMAP_TYPE_BTN ||
            table[i].value  != (uint8_t)i     ||
            table[i].modifier != 0             ||
            table[i].flags  != 0)
            return false;
    }
    return true;
}

static void update_identity_cache(void)
{
    g_remap_is_identity = is_table_identity(g_profiles[active_profile]);
}

static inline uint8_t get_src_bit(const uint8_t *p, int id)
{
    return (p[BTN_LOC[id].off] & BTN_LOC[id].mask) ? 1u : 0u;
}

static void load_single_profile(uint8_t idx)
{
    size_t len = 0;
    ef_get_env_blob(profile_keys[idx], g_profiles[idx],
                    sizeof(g_profiles[idx]), &len);
    size_t n_loaded = len / sizeof(remap_entry_t);
    for (size_t i = 0; i < n_loaded; i++)
        sanitize_entry(&g_profiles[idx][i], (uint8_t)i);
    for (size_t i = n_loaded; i < REMAP_BTN_COUNT; i++)
        g_profiles[idx][i] = (remap_entry_t){ REMAP_TYPE_BTN, (uint8_t)i, 0, 0 };
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void remap_init(void)
{
    for (int p = 0; p < REMAP_PROFILE_COUNT; p++)
        reset_table(g_profiles[p]);
    active_profile = 0;
    g_remap_is_identity = true;
    memset(last_kbd_report, 0, sizeof(last_kbd_report));
}

void remap_load(void)
{
    for (int p = 0; p < REMAP_PROFILE_COUNT; p++)
        load_single_profile(p);

    size_t len = 0;
    uint8_t saved_prof = 0;
    ef_get_env_blob("remap_prof", &saved_prof, 1, &len);
    if (len == 1 && saved_prof < REMAP_PROFILE_COUNT)
        active_profile = saved_prof;
    else
        active_profile = 0;

    update_identity_cache();
    LOG_INF("[REMAP] Loaded profiles, active=%d, identity=%d\n",
            active_profile, (int)g_remap_is_identity);
}

bool remap_save(void)
{
    return remap_save_profile(active_profile);
}

bool remap_save_profile(uint8_t profile)
{
    if (profile >= REMAP_PROFILE_COUNT) return false;
    int r = ef_set_env_blob(profile_keys[profile],
                            g_profiles[profile], sizeof(g_profiles[profile]));
    return r == 0;
}

void remap_set(const uint8_t *data, uint8_t len)
{
    remap_set_profile(active_profile, data, len);
}

void remap_set_profile(uint8_t profile, const uint8_t *data, uint8_t len)
{
    if (profile >= REMAP_PROFILE_COUNT) return;
    if (len < REMAP_BTN_COUNT * sizeof(remap_entry_t)) return;

    memcpy(g_profiles[profile], data, REMAP_BTN_COUNT * sizeof(remap_entry_t));
    for (int i = 0; i < REMAP_BTN_COUNT; i++)
        sanitize_entry(&g_profiles[profile][i], (uint8_t)i);

    if (profile == active_profile)
        update_identity_cache();
    LOG_INF("[REMAP] Profile %d updated, identity=%d\n",
            profile, (int)is_table_identity(g_profiles[profile]));
}

void remap_reset(void)
{
    remap_reset_profile(active_profile);
}

void remap_reset_profile(uint8_t profile)
{
    if (profile >= REMAP_PROFILE_COUNT) return;
    reset_table(g_profiles[profile]);
    if (profile == active_profile) {
        g_remap_is_identity = true;
    }
}

void remap_switch_profile(uint8_t profile)
{
    if (profile >= REMAP_PROFILE_COUNT) return;
    active_profile = profile;
    update_identity_cache();
    ef_set_env_blob("remap_prof", &active_profile, 1);
    LOG_INF("[REMAP] Switched to profile %d, identity=%d\n",
            active_profile, (int)g_remap_is_identity);
}

uint8_t remap_get_active_profile(void)
{
    return active_profile;
}

const remap_entry_t *remap_get_table(void)
{
    return g_profiles[active_profile];
}

const remap_entry_t *remap_get_profile_table(uint8_t profile)
{
    if (profile >= REMAP_PROFILE_COUNT) profile = 0;
    return g_profiles[profile];
}

void remap_apply(uint8_t *p)
{
    if (g_remap_is_identity)
        return;

    const remap_entry_t *remap = g_profiles[active_profile];

    uint8_t src[REMAP_BTN_COUNT];
    for (int i = 0; i < REMAP_BTN_COUNT; i++)
        src[i] = get_src_bit(p, i);
    uint8_t analog_l2 = p[4];
    uint8_t analog_r2 = p[5];

    uint8_t dst[REMAP_BTN_COUNT];
    memset(dst, 0, sizeof(dst));

    for (int i = 0; i < REMAP_BTN_COUNT; i++) {
        if (!src[i])
            continue;
        if (remap[i].type == REMAP_TYPE_BTN) {
            dst[remap[i].value] |= 1;
        } else {
            if (!(remap[i].flags & REMAP_FLAG_SUPPRESS))
                dst[i] |= 1;
        }
    }

    if (remap[REMAP_BTN_L2].type  == REMAP_TYPE_BTN &&
        remap[REMAP_BTN_L2].value == REMAP_BTN_R2   &&
        remap[REMAP_BTN_R2].type  == REMAP_TYPE_BTN &&
        remap[REMAP_BTN_R2].value == REMAP_BTN_L2) {
        p[4] = analog_r2;
        p[5] = analog_l2;
    }

    p[7] = (p[7] & 0x0F)
         | (dst[REMAP_BTN_SQUARE]   ? 0x10 : 0)
         | (dst[REMAP_BTN_CROSS]    ? 0x20 : 0)
         | (dst[REMAP_BTN_CIRCLE]   ? 0x40 : 0)
         | (dst[REMAP_BTN_TRIANGLE] ? 0x80 : 0);

    p[8] = (dst[REMAP_BTN_L1]      ? 0x01 : 0)
         | (dst[REMAP_BTN_R1]      ? 0x02 : 0)
         | (dst[REMAP_BTN_L2]      ? 0x04 : 0)
         | (dst[REMAP_BTN_R2]      ? 0x08 : 0)
         | (dst[REMAP_BTN_CREATE]  ? 0x10 : 0)
         | (dst[REMAP_BTN_OPTIONS] ? 0x20 : 0)
         | (dst[REMAP_BTN_L3]      ? 0x40 : 0)
         | (dst[REMAP_BTN_R3]      ? 0x80 : 0);

    p[9] = (p[9] & 0xF8)
         | (dst[REMAP_BTN_PS]       ? 0x01 : 0)
         | (dst[REMAP_BTN_TP_CLICK] ? 0x02 : 0)
         | (dst[REMAP_BTN_MUTE]     ? 0x04 : 0);
}

void remap_kbd_tick(const uint8_t *p)
{
    const remap_entry_t *remap = g_profiles[active_profile];
    uint8_t report[8] = { 0 };
    int slot = 2;

    for (int i = 0; i < REMAP_BTN_COUNT; i++) {
        if (remap[i].type != REMAP_TYPE_KBD)
            continue;
        if (!get_src_bit(p, i))
            continue;

        if (remap[i].flags & REMAP_FLAG_EXTRA_KEY) {
            if (slot < 8) report[slot++] = remap[i].value;
            if (slot < 8) report[slot++] = remap[i].modifier;
        } else {
            report[0] |= remap[i].modifier;
            if (slot < 8) report[slot++] = remap[i].value;
        }
    }

    if (memcmp(report, last_kbd_report, 8) == 0)
        return;

    memcpy(last_kbd_report, report, 8);
    if (usb_gamepad_kbd_ready())
        usb_gamepad_send_kbd_report(report, 8);
}

void remap_on_disconnect(void)
{
    bool had_keys = (last_kbd_report[0] != 0);
    for (int k = 2; !had_keys && k < 8; k++)
        had_keys = (last_kbd_report[k] != 0);

    if (had_keys && usb_gamepad_kbd_ready()) {
        uint8_t zero[8] = { 0 };
        usb_gamepad_send_kbd_report(zero, 8);
    }
    memset(last_kbd_report, 0, sizeof(last_kbd_report));
}

bool remap_has_kbd_targets(void)
{
    for (int p = 0; p < REMAP_PROFILE_COUNT; p++)
        for (int i = 0; i < REMAP_BTN_COUNT; i++)
            if (g_profiles[p][i].type == REMAP_TYPE_KBD)
                return true;
    return false;
}
