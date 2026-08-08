#include "state_mgr.h"
#include "config.h"
#include <stdbool.h>
#include <string.h>
#include "debug_log.h"

/*
 * SetStateData byte layout (47 bytes):
 *
 *  [0]  flags0:  EnableRumbleEmulation(0), UseRumbleNotHaptics(1),
 *                AllowRightTriggerFFB(2), AllowLeftTriggerFFB(3),
 *                AllowHeadphoneVolume(4), AllowSpeakerVolume(5),
 *                AllowMicVolume(6), AllowAudioControl(7)
 *  [1]  flags1:  AllowMuteLight(0), AllowAudioMute(1), AllowLedColor(2),
 *                ResetLights(3), AllowPlayerIndicators(4),
 *                AllowHapticLowPassFilter(5), AllowMotorPowerLevel(6),
 *                AllowAudioControl2(7)
 *  [2]  RumbleRight
 *  [3]  RumbleLeft
 *  [4]  VolumeHeadphones
 *  [5]  VolumeSpeaker
 *  [6]  VolumeMic
 *  [7]  AudioControl
 *  [8]  MuteLightMode
 *  [9]  MuteControl
 *  [10..20]  RightTriggerFFB  (11 bytes)
 *  [21..31]  LeftTriggerFFB   (11 bytes)
 *  [32..35]  HostTimestamp    (4 bytes)
 *  [36] MotorPowerLevel
 *  [37] AudioControl2
 *  [38] Flags: AllowLightBrightnessChange(0), AllowColorLightFadeAnimation(1),
 *              EnableImprovedRumbleEmulation(2), UseRumbleNotHaptics2(3)
 *  [39] HapticLowPassFilter + UNKBIT
 *  [40] UNK
 *  [41] LightFadeAnimation
 *  [42] LightBrightness
 *  [43] PlayerIndicators
 *  [44] LedRed
 *  [45] LedGreen
 *  [46] LedBlue
 */

static uint8_t state[SET_STATE_SIZE];
static volatile bool spk_active;
static volatile bool vol_dirty;

void state_mgr_init(const uint8_t *init_data, uint8_t len)
{
    memset(state, 0, SET_STATE_SIZE);
    if (len > SET_STATE_SIZE)
        len = SET_STATE_SIZE;
    memcpy(state, init_data, len);
    spk_active = false;
    vol_dirty = false;
}

void state_mgr_update(const uint8_t *data, uint8_t len)
{
    if (len < SET_STATE_SIZE)
        return;

    uint8_t f0 = data[0];
    uint8_t f1 = data[1];

    /* Selective merge based on flags — only update fields the host claims
     * control of in this frame. This prevents later non-trigger frames from
     * zeroing trigger data that was set by an earlier frame in the same
     * drain batch. */

    /* flags always update (they accumulate across frames) */
    state[0] |= f0;
    state[1] |= f1;

    /* Rumble (flags0 bit 0 or bit 1: either rumble mode uses bytes 2-3) */
    if (f0 & 0x03) {
        state[2] = data[2];
        state[3] = data[3];
    }

    /* Right trigger (flags0 bit 2: AllowRightTriggerFFB) */
    if (f0 & 0x04)
        memcpy(state + 10, data + 10, 11);

    /* Left trigger (flags0 bit 3: AllowLeftTriggerFFB) */
    if (f0 & 0x08)
        memcpy(state + 21, data + 21, 11);

    /* Headphone volume (flags0 bit 4) */
    if (f0 & 0x10)
        state[4] = data[4];

    /* Speaker volume (flags0 bit 5) */
    if (f0 & 0x20)
        state[5] = data[5];

    /* Mic volume (flags0 bit 6) */
    if (f0 & 0x40)
        state[6] = data[6];

    /* Audio control (flags0 bit 7) */
    if (f0 & 0x80)
        state[7] = data[7];

    /* Mute light (flags1 bit 0) */
    if (f1 & 0x01)
        state[8] = data[8];

    /* Audio mute (flags1 bit 1) */
    if (f1 & 0x02)
        state[9] = data[9];

    /* LED color (flags1 bit 2: AllowLedColor)
     * Light-only bytes [41] (LightFadeAnimation) and [42] (LightBrightness)
     * are gated here so rumble-only reports can't overwrite the primer's
     * critical FadeOut=2 value.  Byte [38] is NOT gated because it has
     * mixed content: bits 0-1 are light sub-flags, but bit 2 is
     * EnableImprovedRumbleEmulation which must be updated by rumble reports. */
    if (f1 & 0x04) {
        state[41] = data[41];
        state[42] = data[42];
        state[44] = data[44];
        state[45] = data[45];
        state[46] = data[46];
    }

    /* ResetLights (flags1 bit 3) — one-shot behavior bit, no data */

    /* Player indicators (flags1 bit 4) */
    if (f1 & 0x10)
        state[43] = data[43];

    /* Haptic low-pass filter (flags1 bit 5) */
    if (f1 & 0x20)
        state[39] = data[39];

    /* Motor power level (flags1 bit 6) */
    if (f1 & 0x40)
        state[36] = data[36];

    /* Audio control 2 (flags1 bit 7) */
    if (f1 & 0x80)
        state[37] = data[37];

    /* Timestamp + misc — always update */
    memcpy(state + 32, data + 32, 4); /* HostTimestamp */
    state[38] = data[38];
    state[40] = data[40];

    /* Persist volume changes to config */
    if (!config_get()->lock_volume) {
        if (f0 & 0x10)
            config_get()->headset_volume = data[4];
        if (f0 & 0x20)
            config_get()->speaker_volume = data[5];
    } else {
        state[0] &= ~0x70;
    }

    /* Config overlays — match DS5Dongle's tud_hid_set_report_cb */
    struct config_body *cfg = config_get();
    if (cfg->trigger_reduce > 0) {
        state[1] |= 0x40;
        state[36] = (state[36] & 0x0F) |
                    ((cfg->trigger_reduce & 0x0F) << 4);
    }
    if (cfg->speaker_gain > 0) {
        state[1] |= 0x80;
        state[37] = cfg->speaker_gain & 0x07;
    }
}

void state_mgr_get(uint8_t *out, uint8_t size)
{
    if (size > SET_STATE_SIZE)
        size = SET_STATE_SIZE;
    memcpy(out, state, size);
}

void state_mgr_clear_flags(void)
{
    state[0] = 0;
    state[1] = 0;
    state[38] &= ~0x03; /* clear light sub-flags (bits 0-1) to prevent
                         * FadeOut animation re-triggering on non-LED reports;
                         * bit 2 (EnableImprovedRumbleEmulation) preserved */
}

void state_mgr_clear_oneshot_flags(void)
{
    state[1] &= ~0x08; /* ResetLights is one-shot; must not accumulate */
}

void state_mgr_apply_config(uint8_t spk_vol, uint8_t hp_vol, uint8_t spk_gain,
                            uint8_t trigger_reduce)
{
    state[4] = hp_vol;          /* VolumeHeadphones */
    state[5] = spk_vol;         /* VolumeSpeaker */
    state[37] = spk_gain & 0x07; /* AudioControl2: SpeakerCompPreGain */
    if (trigger_reduce > 0) {
        /* state[36] high nibble = TriggerMotorPowerReduction [0..10] */
        state[36] = (state[36] & 0x0F) | ((trigger_reduce & 0x0F) << 4);
    }
}

void state_mgr_set_spk_active(bool active)
{
    spk_active = active;
}

bool state_mgr_is_spk_active(void)
{
    return spk_active;
}

void state_mgr_set_volume(uint8_t spk_vol, uint8_t hp_vol)
{
    if (config_get()->lock_volume)
        return;
    state[0] |= 0x30;
    state[4] = hp_vol;
    state[5] = spk_vol;
    config_get()->headset_volume = hp_vol;
    config_get()->speaker_volume = spk_vol;
    vol_dirty = true;
}

void state_mgr_set_mute(bool mute)
{
    if (config_get()->lock_volume)
        return;
    state[1] |= 0x02;
    if (mute)
        state[9] |= 0x60;
    else
        state[9] &= ~0x60;
    vol_dirty = true;
}

void state_mgr_restore_config_volume(void)
{
    struct config_body *cfg = config_get();
    state[0] |= 0x30;
    state[1] |= 0x02;
    state[4] = cfg->headset_volume;
    state[5] = cfg->speaker_volume;
    state[9] &= ~0x60;
    vol_dirty = true;
}

bool state_mgr_vol_dirty(void) { return vol_dirty; }
void state_mgr_vol_ack(void)   { vol_dirty = false; }

bool state_mgr_should_send(const uint8_t *data)
{
    (void)data;
    return true;
}

void state_mgr_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    state[44] = r;
    state[45] = g;
    state[46] = b;
}
