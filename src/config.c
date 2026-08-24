#include "config.h"
#include <string.h>
#include "debug_log.h"
#include <math.h>

extern int bt_settings_set_bin(const char *key, const uint8_t *value,
                               size_t length);
extern int bt_settings_get_bin(const char *key, uint8_t *value,
                               size_t exp_len, size_t *real_len);

#define CONFIG_EF_KEY  "dongle_cfg"

static struct config_body cfg;

void config_validate(void)
{
    struct config_body *b = &cfg;

    if (b->config_version != CONFIG_VERSION) {
        LOG_WRN("[CFG] Version mismatch (%d vs %d), resetting\n",
               b->config_version, CONFIG_VERSION);
        memset(b, 0xFF, sizeof(*b));
        b->config_version = CONFIG_VERSION;
    }

    if (isnan(b->haptics_gain) || b->haptics_gain < 1.0f || b->haptics_gain > 2.0f)
        b->haptics_gain = 1.0f;
    if (b->speaker_volume > 127)
        b->speaker_volume = 100;
    if (b->headset_volume > 127)
        b->headset_volume = 100;
    if (b->speaker_gain > 7)
        b->speaker_gain = 2;
    if (b->inactive_time > 60)
        b->inactive_time = 30;
    if (b->disable_led > 1)
        b->disable_led = 0;
    if (b->polling_rate_mode > 2)
        b->polling_rate_mode = 2;
    if (b->audio_buffer_length < 16 || b->audio_buffer_length > 128)
        b->audio_buffer_length = 64;
    if (b->controller_mode > 2)
        b->controller_mode = 2;
    if (b->enable_usb_sn > 1)
        b->enable_usb_sn = 0;
    if (b->ps_shortcut_enabled > 1)
        b->ps_shortcut_enabled = 0;
    if (b->disable_mic > 1)
        b->disable_mic = 0;
    if (b->disable_speaker > 1)
        b->disable_speaker = 0;
    if (b->enable_wake > 1)
        b->enable_wake = 1;
    if (b->trigger_reduce > 10)
        b->trigger_reduce = 0;
    if (b->lock_volume > 1)
        b->lock_volume = 0;
    if (b->dse_detected > 1)
        b->dse_detected = 0;
    if (b->usb_stealth > 1)
        b->usb_stealth = 0;
    /* Default LED color to white for configs migrated from older firmware
     * (memset(0) leaves these as 0,0,0 which would be black). */
    if (b->led_r == 0 && b->led_g == 0 && b->led_b == 0) {
        b->led_r = 0xFF;
        b->led_g = 0xFF;
        b->led_b = 0xFF;
    }
    if (b->tp_mode > 4)
        b->tp_mode = 0;
    if (b->tp_mode_enabled_mask == 0 || b->tp_mode_enabled_mask > 0x1F)
        b->tp_mode_enabled_mask = 0x01; /* at least mode 0 enabled */
    if (b->tp_mouse_sensitivity == 0 || b->tp_mouse_sensitivity > 32)
        b->tp_mouse_sensitivity = 8;
    if (b->audio_haptic > 2)
        b->audio_haptic = 0;
}

void config_load(void)
{
    size_t rlen = 0;
    memset(&cfg, 0, sizeof(cfg));
    int err = bt_settings_get_bin(CONFIG_EF_KEY, (uint8_t *)&cfg,
                                  sizeof(cfg), &rlen);
    if (err != 0 || rlen < 19) {
        LOG_INF("[CFG] No saved config, using defaults\n");
        memset(&cfg, 0, sizeof(cfg));
        cfg.config_version    = CONFIG_VERSION;
        cfg.haptics_gain      = 1.0f;
        cfg.speaker_volume    = 100;
        cfg.headset_volume    = 100;
        cfg.speaker_gain      = 2;
        cfg.inactive_time     = 30;
        cfg.disable_led       = 1;     /* auto-off LED after 1 min */
        cfg.polling_rate_mode = 0;     /* default 250 Hz */
        cfg.audio_buffer_length = 64;
        cfg.controller_mode   = 2;     /* Auto */
        cfg.enable_wake       = 0;
        cfg.enable_usb_sn     = 1;     /* DS5Dongle always provides serial */
        cfg.usb_stealth       = 0;     /* USB visible at boot (non-stealth) */
        cfg.led_r             = 0xFF;
        cfg.led_g             = 0xFF;
        cfg.led_b             = 0xFF;
        cfg.tp_mode           = 0;     /* off */
        cfg.tp_mode_enabled_mask = 0x03; /* mode 0 + mode 1 enabled */
        cfg.tp_mouse_sensitivity = 8;
    }
    /* Migrate old 27-byte config: byte 26 was tp_mouse_sensitivity, not mask */
    if (err == 0 && rlen == 27) {
        cfg.tp_mouse_sensitivity = cfg.tp_mode_enabled_mask;
        cfg.tp_mode_enabled_mask = 0x03;
    }
    config_validate();
    LOG_INF("[CFG] Loaded: wake=%d led_off=%d inactive=%dmin poll=%d ps=%d\n",
           cfg.enable_wake, cfg.disable_led, cfg.inactive_time,
           cfg.polling_rate_mode, cfg.ps_shortcut_enabled);
    LOG_INF("[CFG] tp_mode=%u mask=0x%02x sens=%u (rlen=%u)\n",
           cfg.tp_mode, cfg.tp_mode_enabled_mask, cfg.tp_mouse_sensitivity,
           (unsigned)rlen);
}

bool config_save(void)
{
    int rc = bt_settings_set_bin(CONFIG_EF_KEY, (const uint8_t *)&cfg,
                                 sizeof(cfg));
    if (rc == 0) {
        LOG_INF("[CFG] Saved to flash\n");
        return true;
    }
    LOG_ERR("[CFG] Save failed: %d\n", rc);
    return false;
}

struct config_body *config_get(void)
{
    return &cfg;
}

void config_set(const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len < sizeof(cfg) ? len : sizeof(cfg);
    memcpy(&cfg, data, copy_len);
    config_validate();
    LOG_INF("[CFG] Updated from host (%d bytes)\n", copy_len);
}
