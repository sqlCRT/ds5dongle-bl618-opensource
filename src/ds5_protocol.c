#include "ds5_protocol.h"
#include "debug_log.h"

static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = true;
}

uint32_t ds5_crc32(uint8_t seed, const uint8_t *data, uint16_t len)
{
    if (!crc32_initialized)
        crc32_init();

    uint32_t crc = 0xFFFFFFFF;
    crc = (crc >> 8) ^ crc32_table[(crc ^ seed) & 0xFF];
    for (uint16_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return ~crc;
}

static inline int16_t read_le16(const uint8_t *p)
{
    return (int16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static inline void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

int ds5_parse_bt_input(const uint8_t *data, uint16_t len, struct ds5_input_state *state)
{
    if (!data || !state || len < DS5_BT_INPUT_REPORT_SIZE)
        return -1;

    if (data[0] != DS5_BT_INPUT_REPORT_ID)
        return -1;

    memset(state, 0, sizeof(*state));

    const uint8_t *p = data + DS5_BT_PAYLOAD_OFFSET;

    state->sequence  = data[1];
    state->stick_lx  = p[0];
    state->stick_ly  = p[1];
    state->stick_rx  = p[2];
    state->stick_ry  = p[3];
    state->trigger_l2 = p[4];
    state->trigger_r2 = p[5];
    state->counter   = p[6];

    uint8_t btn0 = p[7];
    state->dpad     = btn0 & 0x0F;
    state->square   = (btn0 >> 4) & 1;
    state->cross    = (btn0 >> 5) & 1;
    state->circle   = (btn0 >> 6) & 1;
    state->triangle = (btn0 >> 7) & 1;

    uint8_t btn1 = p[8];
    state->l1      = (btn1 >> 0) & 1;
    state->r1      = (btn1 >> 1) & 1;
    state->l2_btn  = (btn1 >> 2) & 1;
    state->r2_btn  = (btn1 >> 3) & 1;
    state->create  = (btn1 >> 4) & 1;
    state->options = (btn1 >> 5) & 1;
    state->l3      = (btn1 >> 6) & 1;
    state->r3      = (btn1 >> 7) & 1;

    uint8_t btn2 = p[9];
    state->ps             = (btn2 >> 0) & 1;
    state->touchpad_click = (btn2 >> 1) & 1;
    state->mute           = (btn2 >> 2) & 1;

    /* p[10] = UNK2, p[11..14] = UNK_COUNTER (4 bytes) */
    state->gyro_x = read_le16(&p[15]);
    state->gyro_z = read_le16(&p[17]);
    state->gyro_y = read_le16(&p[19]);
    state->accel_x = read_le16(&p[21]);
    state->accel_y = read_le16(&p[23]);
    state->accel_z = read_le16(&p[25]);
    state->sensor_timestamp = read_le32(&p[27]);
    state->temperature = p[31];

    for (int i = 0; i < 2; i++) {
        const uint8_t *tp = &p[32 + i * 4];
        state->touch[i].active = !(tp[0] & 0x80);
        state->touch[i].id     = tp[0] & 0x7F;
        state->touch[i].x      = tp[1] | ((tp[2] & 0x0F) << 8);
        state->touch[i].y      = ((tp[2] & 0xF0) >> 4) | (tp[3] << 4);
    }

    uint8_t batt = p[52];
    state->battery_level    = (batt & 0x0F) * 10;
    if (state->battery_level > 100)
        state->battery_level = 100;
    state->battery_charging = (batt & 0x10) != 0;
    state->battery_full     = (batt & 0x20) != 0;

    return 0;
}

void ds5_output_state_init(struct ds5_output_state *state)
{
    memset(state, 0, sizeof(*state));
    state->led_r = 0;
    state->led_g = 0;
    state->led_b = 64;
    state->led_brightness = 0x02;
    state->mute_led = DS5_MUTE_LED_OFF;
}

int ds5_build_bt_output(const struct ds5_output_state *state, uint8_t seq, uint8_t *buf)
{
    memset(buf, 0, DS5_BT_OUTPUT_REPORT_SIZE);

    buf[0] = DS5_BT_OUTPUT_REPORT_ID;
    buf[1] = (seq << 4);
    buf[2] = DS5_BT_OUTPUT_TAG;

    uint8_t flags0 = 0;
    uint8_t flags1 = 0;

    if (state->enable_rumble)
        flags0 |= 0x01;
    if (state->enable_trigger)
        flags0 |= 0x04;
    if (state->enable_led)
        flags1 |= 0x04;
    if (state->enable_player_led)
        flags1 |= 0x10;

    buf[3] = flags0;
    buf[4] = flags1;

    buf[5] = state->motor_right;
    buf[6] = state->motor_left;

    buf[11] = state->mute_led;

    buf[13] = state->r2_trigger_mode;
    memcpy(&buf[14], state->r2_trigger_params, 10);

    buf[24] = state->l2_trigger_mode;
    memcpy(&buf[25], state->l2_trigger_params, 10);

    buf[41] = 0x02;
    buf[42] = state->led_brightness;
    buf[43] = state->player_leds;
    buf[44] = state->led_r;
    buf[45] = state->led_g;
    buf[46] = state->led_b;

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, buf,
                             DS5_BT_OUTPUT_REPORT_SIZE - 4);
    write_le32(&buf[DS5_BT_OUTPUT_REPORT_SIZE - 4], crc);

    return DS5_BT_OUTPUT_REPORT_SIZE;
}

int ds5_build_calibration_request(uint8_t *buf)
{
    /* GET_REPORT(0x4) | Feature(0x3) = 0x43 */
    buf[0] = 0x43;
    buf[1] = DS5_FEATURE_REPORT_CALIBRATION;
    return 2;
}
