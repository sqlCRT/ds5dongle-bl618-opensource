#ifndef DS5_PROTOCOL_H
#define DS5_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define DS5_BT_INPUT_REPORT_ID    0x31
#define DS5_BT_OUTPUT_REPORT_ID   0x31
#define DS5_USB_INPUT_REPORT_ID   0x01

#define DS5_FEATURE_REPORT_CALIBRATION 0x05
#define DS5_FEATURE_REPORT_PAIRING     0x09
#define DS5_FEATURE_REPORT_FIRMWARE    0x20

#define DS5_BT_INPUT_REPORT_SIZE  78
#define DS5_BT_OUTPUT_REPORT_SIZE 78
#define DS5_BT_OUTPUT_CRC_SEED    0xA2
#define DS5_BT_FEATURE_CRC_SEED  0x53
#define DS5_BT_OUTPUT_TAG         0x10

#define DS5_BT_OUTPUT_REPORT_ID_EXT 0x32
#define DS5_BT_OUTPUT_EXT_SIZE      142
#define DS5_BT_OUTPUT_EXT_TAG       0x90  /* 0x10 | 0x80 (CRC flag) */
#define DS5_BT_OUTPUT_EXT_PAYLOAD   63

#define DS5_BT_AUDIO_REPORT_ID      0x39
#define DS5_BT_AUDIO_REPORT_SIZE    547
#define DS5_AUDIO_TAG_HEADER        0x91  /* 0x11 | 0x80 */
#define DS5_AUDIO_TAG_HAPTICS       0xD2  /* 0x12 | 0x40 | 0x80 — double frame */
#define DS5_AUDIO_TAG_SPEAKER       0xD3  /* 0x13 | 0x40 | 0x80 — double frame */
#define DS5_AUDIO_TAG_HEADSET       0xD6  /* 0x16 | 0x40 | 0x80 — double frame */
#define DS5_AUDIO_SAMPLE_SIZE       64
#define DS5_AUDIO_OPUS_SIZE         200
#define DS5_HEADSET_BYTE            53    /* offset in 63-byte USB payload for headset plug */

#define DS5_BT_PAYLOAD_OFFSET     2
#define DS5_USB_PAYLOAD_OFFSET    1

#define DS5_TOUCH_X_MAX           1919
#define DS5_TOUCH_Y_MAX           1079

#define DS5_VID                   0x054C
#define DS5_PID                   0x0CE6
#define DS5_EDGE_PID              0x0DF2

#define DS5_BATT_BYTE_OFFSET      52   /* offset in 63-byte USB payload */
#define DS5_BATT_LEVEL_MASK       0x0F
#define DS5_BATT_STATE_SHIFT      4
#define DS5_BATT_STATE_DISCHARGE  0x00
#define DS5_BATT_LOW_THRESHOLD    1    /* PowerPercent <= 1 → ≤ 10% */
#define DS5_BATT_WARN_THRESHOLD   2    /* PowerPercent <= 2 → ≤ 20% */

#define DS5_BTN_PS_BYTE           9    /* offset in 63-byte USB payload */
#define DS5_BTN_PS_BIT            0x01

enum ds5_dpad {
    DS5_DPAD_N    = 0,
    DS5_DPAD_NE   = 1,
    DS5_DPAD_E    = 2,
    DS5_DPAD_SE   = 3,
    DS5_DPAD_S    = 4,
    DS5_DPAD_SW   = 5,
    DS5_DPAD_W    = 6,
    DS5_DPAD_NW   = 7,
    DS5_DPAD_NONE = 8,
};

enum ds5_trigger_mode {
    DS5_TRIGGER_OFF           = 0x00,
    DS5_TRIGGER_RIGID         = 0x01,
    DS5_TRIGGER_PULSE         = 0x02,
    DS5_TRIGGER_RIGID_A       = 0x21,
    DS5_TRIGGER_RIGID_B       = 0x22,
    DS5_TRIGGER_RIGID_AB      = 0x23,
    DS5_TRIGGER_PULSE_A       = 0x24,
    DS5_TRIGGER_PULSE_B       = 0x25,
    DS5_TRIGGER_PULSE_AB      = 0x26,
    DS5_TRIGGER_CALIBRATION   = 0xFC,
};

enum ds5_mute_led {
    DS5_MUTE_LED_OFF   = 0,
    DS5_MUTE_LED_ON    = 1,
    DS5_MUTE_LED_PULSE = 2,
};

struct ds5_touch_point {
    bool     active;
    uint8_t  id;
    uint16_t x;
    uint16_t y;
};

struct ds5_input_state {
    uint8_t stick_lx;
    uint8_t stick_ly;
    uint8_t stick_rx;
    uint8_t stick_ry;

    uint8_t trigger_l2;
    uint8_t trigger_r2;

    uint8_t dpad;

    bool square, cross, circle, triangle;
    bool l1, r1, l2_btn, r2_btn;
    bool create, options, l3, r3;
    bool ps, touchpad_click, mute;

    int16_t  gyro_x, gyro_y, gyro_z;
    int16_t  accel_x, accel_y, accel_z;
    uint32_t sensor_timestamp;
    uint8_t  temperature;

    struct ds5_touch_point touch[2];

    uint8_t battery_level;
    bool    battery_charging;
    bool    battery_full;

    uint8_t counter;
    uint8_t sequence;
};

struct ds5_output_state {
    uint8_t motor_right;
    uint8_t motor_left;

    uint8_t led_r, led_g, led_b;
    uint8_t player_leds;
    uint8_t led_brightness;

    uint8_t r2_trigger_mode;
    uint8_t r2_trigger_params[10];
    uint8_t l2_trigger_mode;
    uint8_t l2_trigger_params[10];

    uint8_t mute_led;

    bool enable_rumble;
    bool enable_led;
    bool enable_player_led;
    bool enable_trigger;
};

/**
 * Parse a BT input report (Report ID 0x31) into structured state.
 * @param data   raw report bytes (including Report ID), length >= DS5_BT_INPUT_REPORT_SIZE
 * @param state  output parsed state
 * @return 0 on success, -1 on invalid data
 */
int ds5_parse_bt_input(const uint8_t *data, uint16_t len, struct ds5_input_state *state);

/**
 * Build a BT output report (Report ID 0x31) from structured state.
 * Appends CRC32 for BT transport.
 * @param state   desired output state
 * @param seq     sequence number (incremented per report)
 * @param buf     output buffer, must be >= DS5_BT_OUTPUT_REPORT_SIZE
 * @return number of bytes written
 */
int ds5_build_bt_output(const struct ds5_output_state *state, uint8_t seq, uint8_t *buf);

/**
 * Build the GET_FEATURE_REPORT request for calibration (Report 0x05).
 * Used to activate DualSense full-feature mode.
 * @param buf    output buffer, must be >= 2 bytes
 * @return number of bytes written
 */
int ds5_build_calibration_request(uint8_t *buf);

/**
 * CRC32 calculation for DualSense BT reports.
 */
uint32_t ds5_crc32(uint8_t seed, const uint8_t *data, uint16_t len);

/**
 * Write a 32-bit value in little-endian byte order.
 */
static inline void ds5_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

/**
 * Initialize default output state (LEDs off, no rumble).
 */
void ds5_output_state_init(struct ds5_output_state *state);

#endif /* DS5_PROTOCOL_H */
