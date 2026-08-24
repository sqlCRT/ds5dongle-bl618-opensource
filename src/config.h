#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Configuration body — layout-compatible with DS5Dongle's Config_body
 * for potential companion-app interoperability.  Audio-related fields
 * are preserved but currently ignored on BL618.
 */
struct __attribute__((packed)) config_body {
    uint8_t config_version;
    float   haptics_gain;         /* [1.0,2.0]  — haptics amplitude scaling */
    uint8_t speaker_volume;       /* [0,127]    — speaker volume level */
    uint8_t headset_volume;       /* [0,127]    — headset volume level */
    uint8_t speaker_gain;         /* [0,7]      — speaker gain (SpeakerCompPreGain) */
    uint8_t inactive_time;        /* [0,60] min  (0 = disable) */
    uint8_t disable_led;          /* bool */
    uint8_t polling_rate_mode;    /* 0: 250Hz, 1: 500Hz, 2: real-time */
    uint8_t audio_buffer_length;  /* [16,127]   — controller audio buffer depth */
    uint8_t controller_mode;      /* 0: DS5, 1: DSE, 2: Auto */
    uint8_t enable_usb_sn;        /* bool */
    uint8_t ps_shortcut_enabled;  /* bool */
    uint8_t disable_mic;          /* bool       — disable mic passthrough */
    uint8_t disable_speaker;      /* bool       — disable Opus speaker encoding */
    uint8_t enable_wake;          /* bool */
    uint8_t trigger_reduce;       /* [0,10]     — trigger motor power reduction */
    uint8_t lock_volume;          /* bool       — ignore host volume changes */
    uint8_t dse_detected;         /* bool — Auto mode remembers last detection */
    uint8_t usb_stealth;          /* bool — hide USB until BT controller connects */
    uint8_t led_r;                /* [0-255] custom LED red   (0xFF = default white) */
    uint8_t led_g;                /* [0-255] custom LED green (0xFF = default white) */
    uint8_t led_b;                /* [0-255] custom LED blue  (0xFF = default white) */
    uint8_t tp_mode;              /* 0=off, 1=dpad, 2=Lmouse+Rdpad, 3=Ldpad+Rmouse, 4=split */
    uint8_t tp_mode_enabled_mask; /* bitmask of enabled modes (bit0-bit4) */
    uint8_t tp_mouse_sensitivity; /* [1-32], default 8 */
    uint8_t audio_haptic;         /* 0=off, 1=auto(game priority), 2=force */
};

#define CONFIG_VERSION  2

void config_load(void);
bool config_save(void);
void config_validate(void);

struct config_body *config_get(void);
void config_set(const uint8_t *data, uint16_t len);

/* Convenience accessors for hot-path fields */
static inline bool config_wake_enabled(void)       { return config_get()->enable_wake; }
static inline bool config_led_disabled(void)        { return config_get()->disable_led; }
static inline uint8_t config_inactive_minutes(void) { return config_get()->inactive_time; }
static inline uint8_t config_polling_mode(void)     { return config_get()->polling_rate_mode; }
static inline bool config_ps_shortcut(void)         { return config_get()->ps_shortcut_enabled; }
static inline uint8_t config_controller_mode(void)  { return config_get()->controller_mode; }
static inline bool config_dse_detected(void)        { return config_get()->dse_detected; }
static inline bool config_speaker_disabled(void)    { return config_get()->disable_speaker; }
static inline bool config_mic_disabled(void)         { return config_get()->disable_mic; }
static inline uint8_t config_audio_buf_len(void)    { return config_get()->audio_buffer_length; }
static inline bool config_usb_stealth(void)         { return config_get()->usb_stealth; }

#endif /* CONFIG_H */
