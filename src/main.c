#include "board.h"
#include "bflb_mtimer.h"
#include "bflb_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "usb_gamepad.h"
#include "ds5_usb_audio.h"
#include "audio.h"
#include "usb_wake.h"
#include "led_status.h"
#include "state_mgr.h"
#include "config.h"
#include "dse.h"
#include "remap.h"

#include "bl616_glb.h"
#include "btble_lib_api.h"
#include "rfparam_adapter.h"
#include "hci_driver.h"
#include "bflb_mtd.h"
#include "easyflash.h"
#include "board_config.h"
#include "debug_log.h"
#include <string.h>

#define BT_TASK_STACK_SIZE    12288
#define BT_TASK_PRIORITY      (configMAX_PRIORITIES - 2)
#define USB_TASK_STACK_SIZE   6144
#define USB_TASK_PRIORITY     (configMAX_PRIORITIES - 1)
#define AUDIO_TASK_STACK_SIZE 8192
#define AUDIO_TASK_PRIORITY   (configMAX_PRIORITIES - 1)
#define MIC_TASK_STACK_SIZE   4096
#define MIC_TASK_PRIORITY     (configMAX_PRIORITIES - 3)
#define LED_TASK_STACK_SIZE   512
#define LED_TASK_PRIORITY     (configMAX_PRIORITIES - 4)
#define BOOT_HOLD_TICKS      30   /* 30 x 100ms = 3 seconds */
#define BOOT_CLICK_WINDOW    5    /* 500ms window between clicks */
#define RECONNECT_TIMEOUT_MS 2000
#define HANDSHAKE_TIMEOUT_US  (10ULL * 1000000ULL)
#define CONNECTING_TIMEOUT_TICKS pdMS_TO_TICKS(8000)
#define L2CAP_FALLBACK_TIMEOUT_TICKS pdMS_TO_TICKS(3000)

#define INPUT_QUEUE_ITEM_SZ  DS5_BT_INPUT_REPORT_SIZE
#define USB_OUTPUT_BUF_SZ    64  /* Report ID (1) + SetStateData (up to 63 for DSE) */

static QueueHandle_t input_queue;
static QueueHandle_t output_queue;
static volatile bool ds5_connected = false;
static volatile bool ever_connected = false;
static volatile bool battery_low = false;
static volatile bool battery_warn = false;
static volatile bool bt_init_done = false;

static volatile uint8_t cached_battery_level = 0xFF;
static volatile uint8_t cached_battery_state = 0;

/* LED auto-off timers */
#define LED_DISABLE_OFF_US   (60ULL * 1000000ULL)  /* 1 minute */
static volatile uint64_t conn_led_start_us = 0;
static volatile bool conn_led_off = false;
static volatile uint64_t handshake_start_us;
static volatile TickType_t connecting_start_tick = 0;
static volatile bool dse_mode_changed = false;
static volatile bool prev_led_disabled = false;
static volatile bool scan_after_disconnect = false;
/* Stealth mode: frames of Windows USB output to wait before sending primer */
static volatile uint8_t stealth_primer_countdown = 0;
static uint8_t output_seq = 0;  /* sequence counter for 0x31 BT output */
static bool first_input_logged = false;
static uint32_t usb_fwd_count = 0;

static volatile uint64_t out_isr_ts_us = 0;   /* timestamp set in USB ISR */
static volatile uint32_t out_drop_count = 0;   /* queue-full drops */
static uint64_t out_last_send_us = 0;          /* last BT send timestamp */

/* Build a standard 0x31 BT output report (78 bytes) for ongoing output.
 * set_state_data: 47 bytes of SetStateData
 * seq: sequence counter
 * out: 78-byte output buffer */
static void build_bt_output(const uint8_t *set_state_data, uint16_t len,
                            uint8_t seq, uint8_t *out)
{
    memset(out, 0, DS5_BT_OUTPUT_REPORT_SIZE);
    out[0] = DS5_BT_OUTPUT_REPORT_ID;
    out[1] = seq << 4;
    out[2] = DS5_BT_OUTPUT_TAG;
    if (len > DS5_USB_OUTPUT_PAYLOAD_LEN)
        len = DS5_USB_OUTPUT_PAYLOAD_LEN;
    memcpy(out + 3, set_state_data, len);

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, out,
                             DS5_BT_OUTPUT_REPORT_SIZE - 4);
    ds5_write_le32(&out[DS5_BT_OUTPUT_REPORT_SIZE - 4], crc);
}

/* Build an extended 0x32 BT output report (142 bytes).
 * Matches DS5Dongle exactly: pkt[1]=0x10 hardcoded (PS5 native BT format). */
static void build_bt_output_ext(const uint8_t *set_state_data, uint16_t len,
                                uint8_t *out)
{
    memset(out, 0, DS5_BT_OUTPUT_EXT_SIZE);
    out[0] = DS5_BT_OUTPUT_REPORT_ID_EXT;
    out[1] = 0x10;  /* hardcoded — matches PS5 native BT format */
    out[2] = DS5_BT_OUTPUT_EXT_TAG;
    out[3] = DS5_BT_OUTPUT_EXT_PAYLOAD;
    if (len > DS5_BT_OUTPUT_EXT_PAYLOAD)
        len = DS5_BT_OUTPUT_EXT_PAYLOAD;
    memcpy(out + 4, set_state_data, len);

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, out,
                             DS5_BT_OUTPUT_EXT_SIZE - 4);
    ds5_write_le32(&out[DS5_BT_OUTPUT_EXT_SIZE - 4], crc);
}

/* Default SetStateData sent as LED primer on first connect / reconnect.
 * Matches DS5Dongle's update_state() primer exactly.
 *
 * !!!  DO NOT CHANGE THE FOLLOWING VALUES WITHOUT CAREFUL TESTING  !!!
 *
 * [0] flags0 = 0x80 (AllowAudioControl ONLY)
 *   — Adding more flags here causes the controller to misinterpret the
 *     primer as a real game output. LED will be overridden on first connect.
 *
 * [1] flags1 = 0x04 (AllowLedColor ONLY)
 *   — Must include AllowLedColor so the controller accepts our LED color.
 *     Do NOT add AllowRumbleEmulation or other flags; they break LED init.
 *
 * [38] = 0x03 (AllowLightBrightnessChange | AllowColorLightFadeAnimation)
 *   — Required so byte[41] and byte[42] are processed by the controller.
 *
 * [41] LightFadeAnimation = 0x02 (FadeOut)
 *   !!!  MUST BE FadeOut (2). Do NOT change to Nothing (0) or FadeIn (1).  !!!
 *   — Verified by extensive testing (v4.4d vs v4.5d): changing this to 0
 *     silently breaks LED on ALL subsequent game sessions. The controller
 *     ignores our color and keeps whatever the OS sent last (usually blue).
 *     This value matches DS5Dongle reference firmware exactly.
 *
 * [42] LightBrightness = 0x00 (Bright)
 *   — Must be Bright (0). DS5Dongle uses the same value.
 *
 * [44..46] = {0xFF, 0xFF, 0xFF} — initial color: white
 *   — Safe to change the color. Changing the animation/brightness is NOT. */
static const uint8_t init_set_state[DS5_USB_OUTPUT_PAYLOAD_LEN] = {
    /* [0] flags0: AllowAudioControl only — DO NOT ADD MORE FLAGS */
    0x80,
    /* [1] flags1: AllowLedColor only — DO NOT ADD MORE FLAGS */
    0x04,
    /* [2..9] all zeros (no rumble, no volumes, no mute) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* [10..20] RightTriggerFFB (11 bytes) */ 0,0,0,0,0,0,0,0,0,0,0,
    /* [21..31] LeftTriggerFFB (11 bytes) */  0,0,0,0,0,0,0,0,0,0,0,
    /* [32..37] zeros (Timestamp, MotorPowerLevel, AudioControl2) */ 0,0,0,0,0,0,
    /* [38] AllowLightBrightnessChange | AllowColorLightFadeAnimation — required */
    0x03,
    /* [39] HapticLowPassFilter */ 0x00,
    /* [40] UNK */  0x00,
    /* [41] LightFadeAnimation: FadeOut=2 — MUST BE 2, see note above */
    0x02,
    /* [42] LightBrightness: Bright=0 — MUST BE 0, see note above */
    0x00,
    /* [43] PlayerIndicators: none */
    0x00,
    /* [44] LedRed   — color only, safe to change */
    0xFF,
    /* [45] LedGreen — color only, safe to change */
    0xFF,
    /* [46] LedBlue  — color only, safe to change */
    0xFF,
};

/* ---- BT HID callbacks ---- */

/* Persistent cache of the last trigger configuration the game sent.
 * The DualSense controller is stateful (keeps trigger config until changed),
 * but on reconnection its state is lost. We replay this cache after primer
 * so the controller immediately resumes adaptive triggers. */
static uint8_t cached_trigger_flags = 0;          /* bits 2,3 of flags0 */
static uint8_t cached_trigger_r[11];              /* bytes 10-20: RightTriggerFFB */
static uint8_t cached_trigger_l[11];              /* bytes 21-31: LeftTriggerFFB */

/* Persistent cache of the last LED color the game set.
 * Same principle: controller loses LED state on BT disconnect,
 * games don't re-send color after USB replug. */
static bool    cached_led_valid = false;
static uint8_t cached_led_rgb[3];                 /* bytes 44-46: R, G, B */
static uint8_t cached_player_ind = 0;             /* byte 43: PlayerIndicators */
static bool    cached_player_valid = false;
static bool    game_was_active_at_disconnect = false;  /* snapshot at BT disconnect */

/* Called from the output forwarding path to snapshot game state
 * whenever the game sends a report with relevant flags set. */
static void cache_output_state(const uint8_t *set_state_data)
{
    uint8_t f0 = set_state_data[0];
    uint8_t f1 = set_state_data[1];

    /* Cache trigger data (accumulate flags independently — game may send
     * left and right in separate frames) */
    if (f0 & 0x04) {
        cached_trigger_flags |= 0x04;
        memcpy(cached_trigger_r, set_state_data + 10, 11);
    }
    if (f0 & 0x08) {
        cached_trigger_flags |= 0x08;
        memcpy(cached_trigger_l, set_state_data + 21, 11);
    }

    /* Cache LED color */
    if (f1 & 0x04) {
        cached_led_rgb[0] = set_state_data[44];
        cached_led_rgb[1] = set_state_data[45];
        cached_led_rgb[2] = set_state_data[46];
        cached_led_valid = true;
    }

    /* Cache player indicators */
    if (f1 & 0x10) {
        cached_player_ind = set_state_data[43];
        cached_player_valid = true;
    }
}

/* Send the 0x32 LED primer — shared by primer_cb and reconnect handler */
static void send_led_primer(void)
{
    state_mgr_init(init_set_state, DS5_USB_OUTPUT_PAYLOAD_LEN);
    struct config_body *cfg = config_get();
    state_mgr_apply_config(cfg->speaker_volume, cfg->headset_volume,
                           cfg->speaker_gain, cfg->trigger_reduce);
    uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
    state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);
    merged[44] = cfg->led_r;
    merged[45] = cfg->led_g;
    merged[46] = cfg->led_b;
    state_mgr_set_led_color(cfg->led_r, cfg->led_g, cfg->led_b);
    state_mgr_clear_flags();

    /* Only replay trigger cache — games don't re-send trigger data after
     * USB re-enumeration. LED and player indicators are NOT replayed because
     * USB replug causes the host to re-send them; caching would cause stale
     * game colors to persist after game exit. */
    if (cached_trigger_flags) {
        merged[0] |= cached_trigger_flags;
        if (cached_trigger_flags & 0x04)
            memcpy(merged + 10, cached_trigger_r, 11);
        if (cached_trigger_flags & 0x08)
            memcpy(merged + 10 + 11, cached_trigger_l, 11);
    }

    uint8_t buf[DS5_BT_OUTPUT_EXT_SIZE];
    build_bt_output_ext(merged, DS5_USB_OUTPUT_PAYLOAD_LEN, buf);
    bt_hid_host_send_output(buf, DS5_BT_OUTPUT_EXT_SIZE);
}

/* Called from l2cap_intr_connected (earliest possible moment, pre-HANDSHAKE).
 * Sends the 0x32 primer to the controller immediately when L2CAP opens. */
static void on_hid_primer(void)
{
    send_led_primer();
    LOG_INF("[MAIN] Primer sent from l2cap_intr_connected\n");
}

static void on_hid_input(const uint8_t *data, uint16_t len)
{
    if (len < 3) return;

    /* data[0] = report ID (0x31), data[1] = flags */
    if ((data[1] >> 1) & 1) {
        /* Bit1 set: mic Opus frame at data[3], length = len - 3 */
        if (len > 3)
            audio_mic_feed(data + 3, (uint16_t)(len - 3));
        return;
    }

    if (len >= DS5_BT_INPUT_REPORT_SIZE) {
        if (!first_input_logged) {
            first_input_logged = true;
            LOG_INF("[MAIN] First input report received (%d bytes)\n", len);
        }
        uint8_t report[DS5_BT_INPUT_REPORT_SIZE];
        memcpy(report, data, DS5_BT_INPUT_REPORT_SIZE);
        xQueueOverwrite(input_queue, report);
    }
}

static void on_hid_state(enum bt_hid_host_state state)
{
    switch (state) {
    case BT_HID_STATE_IDLE: {
        handshake_start_us = 0;
        connecting_start_tick = 0;
        stealth_primer_countdown = 0;
        bool was_connected = ds5_connected;
        if (was_connected) {
            uint64_t since_out = bflb_mtimer_get_time_us() - out_isr_ts_us;
            game_was_active_at_disconnect = (out_isr_ts_us != 0) &&
                                            (since_out < 5000000ULL);
        }
        if (ds5_connected) {
            ds5_connected = false;
            battery_low = false;
            cached_battery_level = 0xFF;
            cached_battery_state = 0;
            uint8_t neutral[DS5_USB_INPUT_PAYLOAD_LEN];
            memset(neutral, 0, sizeof(neutral));
            neutral[0] = 0x7F; /* LX center */
            neutral[1] = 0x7F; /* LY center */
            neutral[2] = 0x7F; /* RX center */
            neutral[3] = 0x7F; /* RY center */
            neutral[7] = DS5_DPAD_NONE; /* D-pad neutral */
            usb_gamepad_send_raw_input(neutral);
        }
        ds5_connected = false;
        usb_fwd_count = 0;
        first_input_logged = false;
        usb_wake_on_bt_disconnect();
        if (was_connected && !bt_hid_host_is_switching() &&
            !config_wake_enabled() && !usb_wake_host_suspended())
            usb_soft_disconnect();
        dse_reset();
        audio_reset();
        remap_on_disconnect();
        if (was_connected) {
            conn_led_start_us = bflb_mtimer_get_time_us();
            conn_led_off = false;
        }
        if (!conn_led_off)
            led_status_set(was_connected ? LED_RED_BLINK : LED_PURPLE_BLINK_SLOW);
        if (scan_after_disconnect) {
            scan_after_disconnect = false;
            LOG_INF("[MAIN] State: IDLE — button-triggered rescan\n");
            bt_hid_host_scan_start(0);
        } else {
            LOG_INF("[MAIN] State: IDLE\n");
        }
        break;
    }

    case BT_HID_STATE_SCANNING:
        if (!conn_led_off)
            led_status_set(LED_PURPLE_BLINK_FAST);
        LOG_INF("[MAIN] State: SCANNING\n");
        break;

    case BT_HID_STATE_CONNECTING:
    case BT_HID_STATE_SDP_QUERY:
        connecting_start_tick = xTaskGetTickCount();
        if (!conn_led_off)
            led_status_set(LED_PURPLE_BLINK_FAST);
        LOG_INF("[MAIN] State: CONNECTING (%d)\n", state);
        break;

    case BT_HID_STATE_L2CAP_CONTROL:
    case BT_HID_STATE_L2CAP_INTERRUPT:
        if (!conn_led_off)
            led_status_set(LED_PURPLE_BLINK_FAST);
        LOG_INF("[MAIN] State: CONNECTING (%d)\n", state);
        break;

    case BT_HID_STATE_HANDSHAKE: {
        connecting_start_tick = 0;
        /* primer_cb already fired from l2cap_intr_connected (earliest possible).
         * Re-send here in case the state machine order differs on some reconnects.
         * Also update audio active state. */
        send_led_primer();
        if (usb_audio_is_active())
            state_mgr_set_spk_active(true);
        handshake_start_us = bflb_mtimer_get_time_us();
        LOG_INF("[MAIN] State: HANDSHAKE (0x32 sent)\n");
        break;
    }

    case BT_HID_STATE_CONNECTED: {
        connecting_start_tick = 0;
        bool is_dse = bt_hid_host_is_dse();
        uint8_t mode = config_controller_mode();
        if (mode == 2) {
            bool need_dse = is_dse;
            if (need_dse != (bool)config_dse_detected()) {
                config_get()->dse_detected = need_dse ? 1 : 0;
                dse_mode_changed = true;
            }
        }

        handshake_start_us = 0;

        if (ever_connected) {
            /* Reconnection: USB was soft-disconnected in IDLE handler.
             * Do a full USB replug so the host re-enumerates and re-sends
             * all init (including adaptive triggers). */
            stealth_primer_countdown = 5;
            LOG_INF("[MAIN] Reconnect: USB replug + primer pending\n");
            usb_soft_connect();
        } else if (config_usb_stealth()) {
            /* First connection, stealth: USB was disconnected at boot.
             * Let Windows send blue init, then override with our primer. */
            stealth_primer_countdown = 5;
            LOG_INF("[MAIN] Stealth: primer pending after 5 USB output frames\n");
            usb_soft_connect();
        } else {
            /* First connection, non-stealth: USB already connected & enumerated.
             * Just send primer directly — game already has its session. */
            send_led_primer();
        }

        ds5_connected = true;
        ever_connected = true;
        battery_low = false;
        battery_warn = false;
        usb_wake_on_bt_connect();
        conn_led_start_us = bflb_mtimer_get_time_us();
        conn_led_off = false;
        led_status_set(LED_GREEN_SOLID);
        LOG_INF("[MAIN] State: CONNECTED%s\n", is_dse ? " (Edge)" : "");
        break;
    }

    default:
        LOG_INF("[MAIN] State: %d\n", state);
        break;
    }
}


/* ---- USB output callback (host → device) ----
 * Runs in USB interrupt context — only copy raw bytes, let bt_task
 * do the heavy lifting (CRC, BT report construction). */

static void on_usb_output(const uint8_t *data, uint16_t len)
{
    if (!ds5_connected || len < 2)
        return;

    if (data[0] == DS5_USB_REPORT_ID_OUTPUT) {
        uint8_t msg[USB_OUTPUT_BUF_SZ];
        uint16_t copy = (len <= USB_OUTPUT_BUF_SZ) ? len : USB_OUTPUT_BUF_SZ;
        memcpy(msg, data, copy);
        if (copy < USB_OUTPUT_BUF_SZ)
            memset(msg + copy, 0, USB_OUTPUT_BUF_SZ - copy);
        /* If queue full, drop oldest frame to make room (like DS5Dongle) */
        if (xQueueIsQueueFullFromISR(output_queue)) {
            uint8_t dummy[USB_OUTPUT_BUF_SZ];
            xQueueReceiveFromISR(output_queue, dummy, NULL);
            out_drop_count++;
        }
        out_isr_ts_us = bflb_mtimer_get_time_us();
        xQueueSendToBackFromISR(output_queue, msg, NULL);
    }
}

/* ---- FreeRTOS tasks ---- */

static struct bflb_device_s *btn_gpio;

static void boot_button_init(void)
{
    btn_gpio = bflb_device_get_by_name("gpio");
    /* GPIO2 is the bootstrap pin — the board's external pull-down keeps
     * it LOW for normal boot; pressing the BOOT button pulls it HIGH.
     * Do NOT add an internal pull-up (conflicts with external circuit). */
    bflb_gpio_init(btn_gpio, BOOT_BUTTON_PIN,
                   GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
    LOG_INF("[BTN] GPIO%d init, raw=%d\n", BOOT_BUTTON_PIN,
           (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN));
}

static bool boot_button_pressed(void)
{
    /* Active HIGH: BOOT button connects GPIO2 to VCC (HIGH = pressed) */
    return bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN) != 0;
}

static void bt_task(void *arg)
{
    (void)arg;
    LOG_INF("[MAIN] Board: %s\n", BOARD_NAME);

    LOG_INF("[BT_TASK] btble_controller_init...\n");
    btble_controller_init(configMAX_PRIORITIES - 1);
    LOG_INF("[BT_TASK] hci_driver_init...\n");
    hci_driver_init();
    LOG_INF("[BT_TASK] bt_hid_host_init...\n");

    struct bt_hid_host_config cfg = {
        .input_cb   = on_hid_input,
        .state_cb   = on_hid_state,
        .primer_cb  = on_hid_primer,
        .target_vid = DS5_VID,
        .target_pid = DS5_PID,
    };

    int err = bt_hid_host_init(&cfg);
    if (err) {
        LOG_ERR("[MAIN] BT init failed: %d\n", err);
        vTaskDelete(NULL);
        return;
    }

    /* BT init complete — re-enable USB peripheral clock that BT may have disabled */
    GLB_Set_USB_CLK_From_WIFIPLL(1);
    bt_init_done = true;
    LOG_INF("[BT_TASK] BT init done, USB clock re-enabled, signaling usb_task\n");

    LOG_INF("[BT_TASK] led -> PURPLE_BLINK_SLOW\n");
    led_status_set(LED_PURPLE_BLINK_SLOW);
    LOG_INF("[BT_TASK] led OK\n");

    LOG_INF("[BT_TASK] try_reconnect...\n");
    if (bt_hid_host_try_reconnect() == 0) {
        /* Passive reconnect: page scan is enabled, wait for the
         * controller to page us and create L2CAP channels. */
        LOG_INF("[MAIN] Waiting for controller to reconnect...\n");
        TickType_t start = xTaskGetTickCount();
        while (!ds5_connected &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!ds5_connected) {
            LOG_WRN("[MAIN] Reconnect timed out, starting scan\n");
            bt_hid_host_scan_start(0);
        }
    } else {
        LOG_INF("[MAIN] No bonded device, starting scan\n");
        bt_hid_host_scan_start(0);
    }
    LOG_INF("[BT_TASK] Entering main loop\n");

    uint8_t usb_out[USB_OUTPUT_BUF_SZ];
    uint16_t idle_ticks = 0;
    uint16_t loop_count = 0;
    uint64_t disconnecting_since = 0;
    uint64_t stale_conn_since = 0;
    uint16_t scan_retry_ticks = 0;   /* counts idle ticks since last scan */
    uint8_t  scan_fail_count = 0;    /* consecutive scans with 0 results */
#define MAX_SCAN_RETRIES 2           /* stop active inquiry after N failures;
                                      * bonded controllers reconnect via page scan
                                      * (like original DS5Dongle) */

    /*
     * Button gesture FSM (polled at 10 Hz, independent of loop speed).
     *   single click  → switch to next bonded controller
     *   double click  → disconnect + fresh scan (search new controller)
     *   hold >= 3s    → clear all bonds + blacklist + reset
     */
    enum { BTN_IDLE, BTN_PRESSING, BTN_HELD, BTN_WAIT } btn_fsm = BTN_IDLE;
    uint8_t  btn_press_samples = 0;
    uint8_t  btn_wait_samples  = 0;
    uint8_t  btn_click_count   = 0;
    uint64_t btn_last_poll_us  = 0;
    uint8_t  btn_debug_counter = 0;

    prev_led_disabled = config_led_disabled();
    conn_led_start_us = bflb_mtimer_get_time_us();

    for (;;) {
        loop_count++;
        if ((loop_count % 50) == 1) {
            LOG_DBG("[BT_TASK] loop #%u state=%d conn=%d\n",
                   loop_count, bt_hid_host_get_state(), ds5_connected);
        }

        /* ---- BOOT button gesture FSM (10 Hz cadence) ----
         * Placed BEFORE any continue statements so the button is
         * always responsive regardless of BT state machine activity. */
        uint64_t btn_now = bflb_mtimer_get_time_us();
        if (btn_now - btn_last_poll_us >= 100000ULL) {
            btn_last_poll_us = btn_now;
            bool pressed = boot_button_pressed();

            if (++btn_debug_counter >= 50) {
                btn_debug_counter = 0;
                LOG_DBG("[BTN] GPIO%d raw=%d fsm=%d\n",
                       BOOT_BUTTON_PIN,
                       (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN),
                       btn_fsm);
            }

            switch (btn_fsm) {
            case BTN_IDLE:
                if (pressed) {
                    btn_fsm = BTN_PRESSING;
                    btn_press_samples = 1;
                    LOG_DBG("[BTN] IO2 pressed (raw=%d)\n",
                           (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN));
                }
                break;

            case BTN_PRESSING:
                if (pressed) {
                    btn_press_samples++;
                    /* LED feedback while holding: fast red blink to show
                     * "keep holding" (revert to previous pattern on release) */
                    if (btn_press_samples == 3) {
                        LOG_DBG("[BTN] IO2 holding... (%d/%d)\n",
                               btn_press_samples, BOOT_HOLD_TICKS);
                        led_status_set(LED_RED_BLINK);
                    }
                    if (btn_press_samples >= BOOT_HOLD_TICKS) {
                        btn_click_count = 0;
                        btn_fsm = BTN_HELD;
                        LOG_INF("[BTN] IO2 held 3s — clearing all bonds\n");
                        bt_hid_host_clear_bonds();
                        led_status_set(LED_BLINK_TRIPLE);
                        idle_ticks = 0;
                    }
                } else {
                    btn_click_count++;
                    btn_fsm = BTN_WAIT;
                    btn_wait_samples = 0;
                    /* Restore LED to current BT state since hold was cancelled */
                    if (btn_press_samples >= 3) {
                        led_status_set(ds5_connected ? LED_GREEN_SOLID
                                                     : LED_PURPLE_BLINK_SLOW);
                    }
                    LOG_DBG("[BTN] IO2 released (click #%d)\n", btn_click_count);
                }
                break;

            case BTN_HELD:
                if (!pressed) {
                    btn_fsm = BTN_IDLE;
                    btn_press_samples = 0;
                    LOG_INF("[BTN] IO2 released after hold — resetting\n");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    GLB_SW_System_Reset();
                }
                break;

            case BTN_WAIT:
                if (pressed) {
                    btn_fsm = BTN_PRESSING;
                    btn_press_samples = 1;
                } else if (++btn_wait_samples >= BOOT_CLICK_WINDOW) {
                    uint8_t clicks = btn_click_count;
                    btn_click_count = 0;
                    btn_fsm = BTN_IDLE;
                    btn_press_samples = 0;

                    if (clicks == 1) {
                        if (bt_hid_host_get_bonded_count() > 1) {
                            int next = bt_hid_host_switch_next();
                            LOG_INF("[BTN] Single click — switching to controller %d\n", next);
                        } else {
                            LOG_INF("[BTN] Single click — only 1 bonded, fresh scan\n");
                            scan_fail_count = 0;
                            if (ds5_connected) {
                                scan_after_disconnect = true;
                                bt_hid_host_disconnect();
                            } else {
                                bt_hid_host_scan_start(0);
                            }
                        }
                        idle_ticks = 0;
                    } else if (clicks >= 2) {
                        LOG_INF("[BTN] Double click — disconnect + fresh scan\n");
                        scan_fail_count = 0;
                        if (ds5_connected) {
                            scan_after_disconnect = true;
                            bt_hid_host_disconnect();
                        } else {
                            bt_hid_host_scan_start(0);
                        }
                        idle_ticks = 0;
                    }
                }
                break;
            }
        }

        bt_hid_host_persist_if_dirty();

        /* Periodic RSSI read (~every 5 s) */
        {
            static uint16_t rssi_tick = 0;
            if (++rssi_tick >= 500 && ds5_connected) {
                rssi_tick = 0;
                bt_hid_host_read_rssi(NULL);
            }
        }

        /* Fallback: if DISCONNECTING for >1.5 seconds, the remote is dead.
         * Force cleanup to return to IDLE. */
        if (bt_hid_host_get_state() == BT_HID_STATE_DISCONNECTING) {
            if (disconnecting_since == 0)
                disconnecting_since = bflb_mtimer_get_time_us();
            else if ((bflb_mtimer_get_time_us() - disconnecting_since)
                     > 1500000ULL) {
                LOG_WRN("[BT_TASK] DISCONNECTING stuck >1.5s, forcing cleanup\n");
                bt_hid_host_force_disconnect();
                disconnecting_since = 0;
            }
        } else {
            disconnecting_since = 0;
        }

        if (bt_hid_host_poll_scan_early()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_connect()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_security()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_sdp()) {
            idle_ticks = 0;
            continue;
        }

        bt_hid_host_poll_security_watchdog();
        bt_hid_host_poll_incoming_l2cap_fallback();

        if (dse_mode_changed) {
            dse_mode_changed = false;
            config_save();
            LOG_WRN("[MAIN] Controller type changed, resetting...\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            GLB_SW_System_Reset();
        }

        /* React to disable_led config toggle in real-time */
        bool cur_led_disabled = config_led_disabled();
        if (cur_led_disabled != prev_led_disabled) {
            prev_led_disabled = cur_led_disabled;
            if (cur_led_disabled) {
                conn_led_start_us = bflb_mtimer_get_time_us();
                conn_led_off = false;
                LOG_DBG("[LED-DBG] toggle ON: start 1min timer, conn=%d\n",
                       ds5_connected);
            } else {
                conn_led_off = false;
                if (ds5_connected) {
                    if (battery_low)
                        led_status_set(LED_BLINK_BATTERY);
                    else if (battery_warn)
                        led_status_set(LED_BLINK_BATTERY_WARN);
                    else
                        led_status_set(LED_GREEN_SOLID);
                    LOG_DBG("[LED-DBG] toggle OFF: restored green, conn=1\n");
                } else {
                    enum bt_hid_host_state s = bt_hid_host_get_state();
                    if (s == BT_HID_STATE_SCANNING ||
                        s == BT_HID_STATE_CONNECTING ||
                        s == BT_HID_STATE_SDP_QUERY ||
                        s == BT_HID_STATE_L2CAP_CONTROL ||
                        s == BT_HID_STATE_L2CAP_INTERRUPT)
                        led_status_set(LED_PURPLE_BLINK_FAST);
                    else
                        led_status_set(LED_PURPLE_BLINK_SLOW);
                    LOG_DBG("[LED-DBG] toggle OFF: restored purple, conn=0\n");
                }
            }
        }

        /* 1-min auto-off: applies to any pattern in the whitelist */
        if (config_led_disabled() && !conn_led_off &&
            led_status_can_auto_off()) {
            uint64_t now_us = bflb_mtimer_get_time_us();
            uint64_t elapsed = now_us - conn_led_start_us;
            if (elapsed >= LED_DISABLE_OFF_US) {
                led_status_set(LED_OFF);
                conn_led_off = true;
                LOG_DBG("[LED-DBG] auto-off fired: elapsed=%llus\n",
                       (unsigned long long)(elapsed / 1000000ULL));
            }
        }

        if (ds5_connected) {
            /* Push UAC volume/mute changes to controller immediately */
            if (state_mgr_vol_dirty()) {
                state_mgr_vol_ack();
                uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
                state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);
                uint8_t bt_out[DS5_BT_OUTPUT_REPORT_SIZE];
                build_bt_output(merged, DS5_USB_OUTPUT_PAYLOAD_LEN,
                                output_seq, bt_out);
                output_seq = (output_seq + 1) & 0x0F;
                bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                state_mgr_clear_flags();
            }

            if (state_mgr_is_spk_active()) {
                /* Audio active: drain queue without blocking (prevent overflow),
                 * rate-limit BT game output to every 21ms (aligned with audio
                 * cycle) to avoid L2CAP contention and BT scheduler overload.
                 * Use 16ms delay to match the non-audio path's yield pattern. */
                int drained = 0;
                while (xQueueReceive(output_queue, usb_out, 0) == pdTRUE) {
                    state_mgr_update(usb_out + 1, DS5_USB_OUTPUT_PAYLOAD_LEN);
                    cache_output_state(usb_out + 1);
                    drained++;
                }
                uint64_t now_us = bflb_mtimer_get_time_us();
                if (drained > 0 && (now_us - out_last_send_us) >= 21000) {
                    uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
                    state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);
                    uint8_t bt_out[DS5_BT_OUTPUT_REPORT_SIZE];
                    build_bt_output(merged, DS5_USB_OUTPUT_PAYLOAD_LEN,
                                    output_seq, bt_out);
                    output_seq = (output_seq + 1) & 0x0F;
                    bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                    state_mgr_clear_flags();
                    out_last_send_us = now_us;
                }
                vTaskDelay(pdMS_TO_TICKS(16));
            } else if (xQueueReceive(output_queue, usb_out, pdMS_TO_TICKS(16)) == pdTRUE) {
                state_mgr_update(usb_out + 1, DS5_USB_OUTPUT_PAYLOAD_LEN);
                cache_output_state(usb_out + 1);

                /* Stealth mode: drain Windows' blue init frames without forwarding
                 * to BT, then fire primer so controller receives purple BEFORE
                 * any Windows output (same ordering as normal mode). */
                if (stealth_primer_countdown > 0) {
                    stealth_primer_countdown--;
                    if (stealth_primer_countdown == 0) {
                        LOG_INF("[MAIN] Stealth: primer fired, forwarding starts\n");
                        send_led_primer();
                    } else {
                        LOG_INF("[MAIN] Stealth: holding BT forward, countdown=%d\n",
                                (int)stealth_primer_countdown);
                    }
                    goto next_output_iter;
                }

                if (state_mgr_should_send(usb_out + 1)) {
                    uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
                    state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);
                    uint8_t bt_out[DS5_BT_OUTPUT_REPORT_SIZE];
                    build_bt_output(merged, DS5_USB_OUTPUT_PAYLOAD_LEN,
                                    output_seq, bt_out);
                    output_seq = (output_seq + 1) & 0x0F;
                    bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                    state_mgr_clear_flags();
                }
next_output_iter:;
            }
            idle_ticks = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
            idle_ticks++;

            /* CONNECTING timeout: L2CAP channels didn't complete in time */
            {
                static TickType_t l2cap_timeout_start = 0;
                enum bt_hid_host_state s = bt_hid_host_get_state();
                if ((s == BT_HID_STATE_CONNECTING ||
                     s == BT_HID_STATE_SDP_QUERY) &&
                    connecting_start_tick > 0) {
                    TickType_t elapsed = xTaskGetTickCount() - connecting_start_tick;
                    if (elapsed > CONNECTING_TIMEOUT_TICKS) {
                        LOG_WRN("[MAIN] Connecting timeout (%d) — disconnecting\n", s);
                        connecting_start_tick = 0;
                        l2cap_timeout_start = 0;
                        bt_hid_host_disconnect();
                    }
                } else if (s == BT_HID_STATE_L2CAP_CONTROL ||
                           s == BT_HID_STATE_L2CAP_INTERRUPT) {
                    if (l2cap_timeout_start == 0)
                        l2cap_timeout_start = xTaskGetTickCount();
                    TickType_t elapsed = xTaskGetTickCount() - l2cap_timeout_start;
                    if (elapsed > L2CAP_FALLBACK_TIMEOUT_TICKS) {
                        LOG_WRN("[MAIN] Connecting timeout (%d) — disconnecting\n", s);
                        connecting_start_tick = 0;
                        l2cap_timeout_start = 0;
                        bt_hid_host_disconnect();
                    }
                } else {
                    l2cap_timeout_start = 0;
                }
            }

            if (bt_hid_host_get_state() == BT_HID_STATE_HANDSHAKE) {
                bt_hid_host_handshake_tick();
                if (handshake_start_us > 0 &&
                    (bflb_mtimer_get_time_us() - handshake_start_us) >
                    HANDSHAKE_TIMEOUT_US) {
                    LOG_WRN("[MAIN] Handshake timeout — disconnecting\n");
                    handshake_start_us = 0;
                    bt_hid_host_disconnect();
                }
            }

            {
                enum bt_hid_host_state s = bt_hid_host_get_state();
                if (s != BT_HID_STATE_IDLE) {
                    stale_conn_since = 0;
                    scan_retry_ticks = 0;
                    if (s != BT_HID_STATE_SCANNING)
                        scan_fail_count = 0;
                }
            }

            if (bt_hid_host_get_state() == BT_HID_STATE_IDLE &&
                idle_ticks >= 20) {
                if (bt_hid_host_has_pending_conn()) {
                    if (stale_conn_since == 0)
                        stale_conn_since = bflb_mtimer_get_time_us();
                    else if ((bflb_mtimer_get_time_us() - stale_conn_since)
                             > 15ULL * 1000000ULL) {
                        LOG_WRN("[MAIN] conn_disconnected never fired, "
                               "dropping stale conn\n");
                        bt_hid_host_drop_stale_conn();
                        stale_conn_since = 0;
                    }
                    scan_retry_ticks = 0;
                } else {
                    stale_conn_since = 0;
                    scan_retry_ticks += idle_ticks;
                    uint16_t retry_interval = ever_connected ? 600 : 300;
                    if (scan_retry_ticks >= retry_interval) {
                        scan_retry_ticks = 0;
                        if (scan_fail_count < MAX_SCAN_RETRIES) {
                            scan_fail_count++;
                            bt_hid_host_scan_start(0);
                        }
                    }
                }
                idle_ticks = 0;
            }
        }
    }
}

/*
 * Check if the current BT input payload has meaningful user activity
 * (buttons, sticks, triggers differ from neutral).
 */
static bool has_user_activity(const uint8_t *payload)
{
    uint8_t lx = payload[0], ly = payload[1];
    uint8_t rx = payload[2], ry = payload[3];
    uint8_t l2 = payload[4], r2 = payload[5];
    uint8_t b7 = payload[7], b8 = payload[8], b9 = payload[9];

    bool stick_moved = (lx < 0x70 || lx > 0x90 ||
                        ly < 0x70 || ly > 0x90 ||
                        rx < 0x70 || rx > 0x90 ||
                        ry < 0x70 || ry > 0x90);
    bool trigger_pressed = (l2 > 5 || r2 > 5);
    bool buttons_pressed = ((b7 & 0xF0) != 0x00 || (b7 & 0x0F) != 0x08 ||
                             b8 != 0x00 || (b9 & 0x03) != 0x00);

    return stick_moved || trigger_pressed || buttons_pressed;
}

static bool should_use_dse(void)
{
    uint8_t mode = config_controller_mode();
    if (mode == 0) return false;
    if (mode == 1) return true;
    return config_dse_detected();
}

static void usb_task(void *arg)
{
    (void)arg;

    /* Wait for BT init to complete — it reconfigures clocks and kills USB */
    LOG_INF("[USB_TASK] Waiting for BT init to complete...\n");
    while (!bt_init_done) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    LOG_INF("[USB_TASK] BT init done, starting USB init\n");

    usb_gamepad_set_dse_mode(should_use_dse());
    usb_gamepad_set_polling_rate(config_polling_mode());
    usb_gamepad_init(on_usb_output);
    usb_gamepad_set_suspend_hooks(usb_wake_on_suspend,
                                  usb_wake_on_resume,
                                  usb_wake_on_configured);

    if (config_usb_stealth()) {
        vTaskDelay(pdMS_TO_TICKS(150));
        usb_soft_disconnect();
    }

    uint8_t raw_report[DS5_BT_INPUT_REPORT_SIZE];
    uint64_t last_activity_us = bflb_mtimer_get_time_us();
    uint64_t last_input_us    = bflb_mtimer_get_time_us();
    bool inactive_disconnected = false;
    uint32_t usb_check_cnt = 0;

#define CONN_WATCHDOG_US (3ULL * 1000000ULL)  /* 3 seconds without input → force disconnect */

    /* PS shortcut state (Win+G short / Win+Tab long, with debounce) */
    bool     ps_debounced    = false;
    bool     ps_was_pressed  = false;
    bool     ps_long_fired   = false;
    bool     ps_key_pending  = false;
    uint64_t ps_last_high_us = 0;
    uint64_t ps_press_us     = 0;
    uint64_t ps_key_rel_us   = 0;

    for (;;) {
        /* Periodic USB status check (every ~10 seconds) */
        if (++usb_check_cnt >= 10000) {
            usb_check_cnt = 0;
            volatile uint32_t *phy_tst  = (volatile uint32_t *)(USB_BASE + 0x114);
            volatile uint32_t *dev_ctl  = (volatile uint32_t *)(USB_BASE + 0x100);
            volatile uint32_t *glb_int  = (volatile uint32_t *)(USB_BASE + 0x0C4);
            LOG_DBG("[USB-MON] UNPLUG=%d GLINT=%d FORCE_FS=%d "
                   "DEV_CTL=0x%08lx GLB_INT=0x%08lx\n",
                   (int)(*phy_tst & 1),
                   (int)((*dev_ctl >> 2) & 1),
                   (int)((*dev_ctl >> 9) & 1),
                   (unsigned long)*dev_ctl,
                   (unsigned long)*glb_int);
        }
        usb_gamepad_process_deferred();

        /* Connection watchdog: if connected and we've received at least one
         * report, but then nothing for 3s → controller likely powered off.
         * Use force_disconnect to bypass HCI ACK (dead controller can't ACK). */
        if (ds5_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
            LOG_WRN("[MAIN] Watchdog: no input for 3s, force disconnect\n");
            bt_hid_host_force_disconnect();
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count = 0;
        }

        TickType_t wait = pdMS_TO_TICKS(10);
        if (xQueueReceive(input_queue, raw_report, wait) == pdTRUE) {
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count++;
            if (usb_fwd_count == 1)
                LOG_DBG("[USB-FWD] First BT→USB input forwarded\n");
            else if ((usb_fwd_count % 500) == 0)
                LOG_DBG("[USB-FWD] %lu reports forwarded\n",
                       (unsigned long)usb_fwd_count);

            remap_kbd_tick(raw_report + 2);
            remap_apply(raw_report + 2);
            usb_gamepad_send_raw_input(raw_report + 2);
            usb_wake_on_bt_input(raw_report + 2, DS5_USB_INPUT_PAYLOAD_LEN);

            /* Headset plug detection for audio tag switching (0x93 vs 0x96) */
            audio_set_headset((raw_report[2 + DS5_HEADSET_BYTE] & 1) != 0);

            /* Mic mute button → toggle MuteLight on controller */
            {
                static uint8_t prev_mic_bit = 0xFF;
                uint8_t cur_mic_bit = (raw_report[2 + DS5_HEADSET_BYTE] >> 2) & 1;
                if (prev_mic_bit != 0xFF && cur_mic_bit != prev_mic_bit) {
                    uint8_t mute_state[DS5_USB_OUTPUT_PAYLOAD_LEN];
                    memset(mute_state, 0, sizeof(mute_state));
                    mute_state[1] = 0x01;  /* flags1: AllowMuteLight */
                    mute_state[8] = cur_mic_bit ? 0x01 : 0x00; /* MuteLightMode */
                    uint8_t pkt[DS5_BT_OUTPUT_EXT_SIZE];
                    build_bt_output_ext(mute_state, DS5_USB_OUTPUT_PAYLOAD_LEN, pkt);
                    bt_hid_host_send_output(pkt, DS5_BT_OUTPUT_EXT_SIZE);
                }
                prev_mic_bit = cur_mic_bit;
            }

            if (ds5_connected) {
                if (has_user_activity(raw_report + 2)) {
                    last_activity_us = bflb_mtimer_get_time_us();
                    inactive_disconnected = false;
                }

                /* Battery monitoring: <=10% red blink, <=20% yellow blink */
                uint8_t batt = raw_report[2 + DS5_BATT_BYTE_OFFSET];
                uint8_t pct  = batt & DS5_BATT_LEVEL_MASK;
                uint8_t st   = (batt >> DS5_BATT_STATE_SHIFT) & 0x0F;
                cached_battery_level = (pct > 10) ? 100 : pct * 10;
                cached_battery_state = st;
                bool discharging = (st == DS5_BATT_STATE_DISCHARGE);
                bool is_low  = discharging && (pct <= DS5_BATT_LOW_THRESHOLD);
                bool is_warn = discharging && !is_low &&
                               (pct <= DS5_BATT_WARN_THRESHOLD);

                if (is_low && !battery_low) {
                    battery_low = true;
                    battery_warn = false;
                    led_status_set(LED_BLINK_BATTERY);
                    LOG_WRN("[MAIN] Battery critical (%d%%)\n", pct * 10);
                } else if (is_warn && !battery_warn && !battery_low) {
                    battery_warn = true;
                    led_status_set(LED_BLINK_BATTERY_WARN);
                    LOG_WRN("[MAIN] Battery warning (%d%%)\n", pct * 10);
                } else if (!is_low && !is_warn && (battery_low || battery_warn)) {
                    battery_low = false;
                    battery_warn = false;
                    if (config_led_disabled() && conn_led_off)
                        led_status_set(LED_OFF);
                    else
                        led_status_set(LED_GREEN_SOLID);
                }

                uint8_t inact_min = config_inactive_minutes();
                if (inact_min > 0 && !inactive_disconnected && !usb_audio_is_active()) {
                    uint64_t elapsed_us = bflb_mtimer_get_time_us() - last_activity_us;
                    uint64_t timeout_us = (uint64_t)inact_min * 60ULL * 1000000ULL;
                    if (elapsed_us >= timeout_us) {
                        LOG_INF("[MAIN] Idle timeout (%d min) — disconnecting\n",
                               inact_min);
                        bt_hid_host_disconnect();
                        inactive_disconnected = true;
                    }
                }

                /* PS key release runs unconditionally (prevents stuck key
                 * if config changes while a keystroke is in flight) */
                {
                    uint64_t now_us = bflb_mtimer_get_time_us();
                    if (ps_key_pending && now_us >= ps_key_rel_us) {
                        if (usb_gamepad_kbd_ready()) {
                            uint8_t up[8] = {0};
                            usb_gamepad_send_kbd_report(up, sizeof(up));
                            ps_key_pending = false;
                        }
                    }

                    /*
                     * PS shortcut (matches DS5Dongle ps_shortcut.cpp):
                     *   short press → Win+G  (Game Bar)
                     *   long press ≥ 750ms → Win+Tab (Task View)
                     *   50ms debounce, 30ms key hold before release
                     */
                    if (config_ps_shortcut()) {
                        bool raw_ps = (raw_report[2 + DS5_BTN_PS_BYTE] &
                                       DS5_BTN_PS_BIT) != 0;

                        if (raw_ps) {
                            ps_debounced = true;
                            ps_last_high_us = now_us;
                        } else if (now_us - ps_last_high_us > 50000ULL) {
                            ps_debounced = false;
                        }

                        if (ps_debounced && !ps_was_pressed) {
                            ps_press_us = now_us;
                            ps_was_pressed = true;
                            ps_long_fired = false;
                        } else if (ps_debounced && ps_was_pressed) {
                            if (!ps_long_fired &&
                                (now_us - ps_press_us >= 750000ULL) &&
                                usb_gamepad_kbd_ready()) {
                                uint8_t kbd[8] = {0x08, 0, 0x2B,
                                                  0, 0, 0, 0, 0};
                                usb_gamepad_send_kbd_report(kbd, sizeof(kbd));
                                LOG_INF("[PS] Hold -> Win+Tab\n");
                                ps_long_fired = true;
                                ps_key_pending = true;
                                ps_key_rel_us = now_us + 30000ULL;
                            }
                        } else if (!ps_debounced && ps_was_pressed) {
                            if (!ps_long_fired &&
                                usb_gamepad_kbd_ready()) {
                                if (now_us - ps_press_us >= 750000ULL) {
                                    uint8_t kbd[8] = {0x08, 0, 0x2B,
                                                      0, 0, 0, 0, 0};
                                    usb_gamepad_send_kbd_report(kbd,
                                                                sizeof(kbd));
                                    LOG_INF("[PS] Long press -> Win+Tab\n");
                                } else {
                                    uint8_t kbd[8] = {0x08, 0, 0x0A,
                                                      0, 0, 0, 0, 0};
                                    usb_gamepad_send_kbd_report(kbd,
                                                                sizeof(kbd));
                                    LOG_INF("[PS] Short press -> Win+G\n");
                                }
                                ps_key_pending = true;
                                ps_key_rel_us = now_us + 30000ULL;
                            }
                            ps_was_pressed = false;
                        }
                    } else if (ps_was_pressed) {
                        ps_was_pressed = false;
                        ps_debounced = false;
                    }
                }
            }
        }

        if (!ds5_connected) {
            last_activity_us = bflb_mtimer_get_time_us();
            inactive_disconnected = false;
            ps_debounced = false;
            if (ps_was_pressed)
                ps_was_pressed = false;
            if (ps_key_pending && usb_gamepad_kbd_ready()) {
                uint8_t up[8] = {0};
                usb_gamepad_send_kbd_report(up, sizeof(up));
                ps_key_pending = false;
            }
        }

        usb_wake_task();
        dse_task();
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        led_status_tick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

uint8_t get_battery_level(void)  { return cached_battery_level; }
uint8_t get_battery_state(void)  { return cached_battery_state; }

int main(void)
{
    board_init();

    printf("\n=== DS5Dongle BL618 ===\n");
    LOG_INF("DualSense Wireless Bridge\n");

    /* Flash speed diagnostic */
    {
        volatile uint32_t *glb_sf_cfg0 = (volatile uint32_t *)0x20000170;
        uint32_t sf_val  = *glb_sf_cfg0;
        uint8_t clk_sel  = (sf_val >> 12) & 0x3;
        uint8_t clk_sel2 = (sf_val >> 14) & 0x3;
        uint8_t clk_div  = (sf_val >> 8) & 0x7;
        unsigned mhz = 0;
        const char *src = "?";
        if (clk_sel == 0) {
            if (clk_sel2 == 1)      { src = "XTAL";   mhz = 40; }
            else if (clk_sel2 == 0) { src = "PLL120"; mhz = 120; }
            else if (clk_sel2 == 2) { src = "PLL96";  mhz = 96; }
        } else if (clk_sel == 1) { src = "PLL80";  mhz = 80; }
        else if (clk_sel == 2)   { src = "BCLK";   mhz = 0; }
        else if (clk_sel == 3)   { src = "PLL160"; mhz = 160; }
        if (clk_div > 0 && mhz > 0) mhz /= (clk_div + 1);
        LOG_INF("[B] Flash: %s/%u = %uMHz\n", src, clk_div, mhz);
    }

    /* Configure Exchange Memory for BT controller (required for BR/EDR) */
    {
        extern uint8_t __LD_CONFIG_EM_SEL;
        volatile uint32_t em_size = (uint32_t)&__LD_CONFIG_EM_SEL;
        if (em_size == 0)
            GLB_Set_EM_Sel(GLB_WRAM160KB_EM0KB);
        else if (em_size == 32 * 1024)
            GLB_Set_EM_Sel(GLB_WRAM128KB_EM32KB);
        else
            GLB_Set_EM_Sel(GLB_WRAM96KB_EM64KB);
        LOG_INF("[B] EM: %lu KB\n", em_size / 1024);
    }

    bflb_mtd_init();
    easyflash_init();
    if (rfparam_init(0, NULL, 0) != 0)
        LOG_ERR("[B] RF init FAIL\n");
    bflb_mtimer_delay_ms(3000);

    config_load();
    remap_init();
    remap_load();
    led_status_init();
    led_status_set(LED_PURPLE_BLINK_SLOW);
    boot_button_init();
    usb_wake_init();

    usb_audio_early_init();
    bool audio_ok = (audio_init() == 0);
    LOG_INF("[B] audio: %s\n", audio_ok ? "OK" : "FAIL");

    input_queue = xQueueCreate(1, INPUT_QUEUE_ITEM_SZ);
    /* Depth-1: USB ISR always delivers the latest report to usb_task.
     * The 10-deep app_tx_fifo in bt_hid_host.c provides the actual buffering
     * and CAN_SEND_NOW flow control (one BT packet in-flight at a time). */
    output_queue = xQueueCreate(5, USB_OUTPUT_BUF_SZ);
    if (!input_queue || !output_queue) {
        LOG_ERR("[B] queue alloc FAIL\n");
        while (1) {}
    }

    xTaskCreate(bt_task,    "bt",    BT_TASK_STACK_SIZE,
                NULL, BT_TASK_PRIORITY, NULL);
    xTaskCreate(usb_task,  "usb",   USB_TASK_STACK_SIZE,
                NULL, USB_TASK_PRIORITY, NULL);
    if (audio_ok) {
        xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK_SIZE,
                    NULL, AUDIO_TASK_PRIORITY, NULL);
        xTaskCreate(audio_mic_task, "mic", MIC_TASK_STACK_SIZE,
                    NULL, MIC_TASK_PRIORITY, NULL);
    }
    xTaskCreate(led_task,  "led",   LED_TASK_STACK_SIZE,
                NULL, LED_TASK_PRIORITY, NULL);
    LOG_INF("[B] init done, starting scheduler\n");
    vTaskStartScheduler();

    while (1) {}
    return 0;
}

void vApplicationIdleHook(void)
{
    __asm volatile("wfi");
}
