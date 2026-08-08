#include "usb_wake.h"
#include "usb_gamepad.h"
#include "bt_hid_host.h"
#include "config.h"
#include "usbd_core.h"
#include "bflb_mtimer.h"
#include <string.h>
#include "debug_log.h"

#define WAKE_KEYCODE_F15        0x68

#define WAKE_SETTLE_US          150000ULL
#define WAKE_KEY_HOLD_US         80000ULL
#define WAKE_KEY_UP_SETTLE_US   200000ULL
#define WAKE_REQUEST_TIMEOUT_US 5000000ULL
#define WAKE_KEY_ATTEMPTS       2
#define WAKE_POWEROFF_DEBOUNCE_US 5000000ULL

typedef enum {
    WAKE_IDLE,
    WAKE_PENDING_PRESS,
    WAKE_REQUESTED,
    WAKE_KEY_DOWN,
    WAKE_KEY_UP_SENT,
    WAKE_DONE,
} wake_state_t;

static volatile bool     host_suspended   = false;
static volatile bool     host_resumed     = false;
static volatile wake_state_t state        = WAKE_IDLE;
static volatile uint64_t state_entered_us = 0;
static uint8_t           key_attempts     = 0;
static volatile uint64_t suspend_at_us    = 0;
static volatile bool     poweroff_sent    = false;

/*
 * Last-seen DualSense button bytes (USB payload offsets 7/8/9).
 * Defaults match idle controller: D-pad released (0x08), no buttons.
 */
static uint8_t prev_b7 = 0x08;
static uint8_t prev_b8 = 0x00;
static uint8_t prev_b9 = 0x00;

static uint64_t now_us(void)
{
    return bflb_mtimer_get_time_us();
}

static void enter_state(wake_state_t s)
{
    state = s;
    state_entered_us = now_us();
}

static void bt_power_off_controller(void)
{
    uint8_t payload[47];
    memset(payload, 0, sizeof(payload));
    payload[0] = 0x02;
    bt_hid_host_set_feature_crc(0x08, payload, sizeof(payload));
}

static void request_host_wake(const char *reason)
{
    if (!config_wake_enabled())
        return;

    int ret = usbd_send_remote_wakeup(0);
    if (ret == 0) {
        enter_state(WAKE_REQUESTED);
        LOG_INF("[WAKE] %s -> REQUESTED\n", reason);
    } else if (host_suspended) {
        usbd_set_remote_wakeup(0);
        enter_state(WAKE_REQUESTED);
        LOG_WRN("[WAKE] %s -> REQUESTED (forced)\n", reason);
    }
}

/* ---- Public API called from USB event handler (ISR context) ---- */

void usb_wake_init(void)
{
    state = WAKE_IDLE;
    host_suspended = false;
    host_resumed = false;
    suspend_at_us = 0;
    poweroff_sent = false;
}

void usb_wake_on_suspend(void)
{
    host_suspended = true;
    host_resumed = false;
    suspend_at_us = now_us();
    poweroff_sent = false;

    state = WAKE_PENDING_PRESS;
    state_entered_us = now_us();
    prev_b7 = 0x08;
    prev_b8 = 0x00;
    prev_b9 = 0x00;
    key_attempts = 0;
    LOG_INF("[WAKE] USB suspended -> PENDING_PRESS\n");
}

void usb_wake_on_resume(void)
{
    host_suspended = false;
    host_resumed = true;
    suspend_at_us = 0;
    LOG_INF("[WAKE] USB resumed\n");
}

void usb_wake_on_configured(void)
{
    host_suspended = false;
    host_resumed = true;
    suspend_at_us = 0;
}

/* ---- Public API called from FreeRTOS task context ---- */

void usb_wake_on_bt_connect(void)
{
    bool should_wake = host_suspended &&
        (state == WAKE_IDLE || state == WAKE_DONE ||
         state == WAKE_PENDING_PRESS);

    if (should_wake)
        request_host_wake("BT reconnect while suspended");
}

void usb_wake_on_bt_disconnect(void)
{
    state = WAKE_IDLE;
    prev_b7 = 0x08;
    prev_b8 = 0x00;
    prev_b9 = 0x00;
}

bool usb_wake_host_suspended(void)
{
    return host_suspended;
}

void usb_wake_on_bt_input(const uint8_t *payload, uint16_t len)
{
    if (len < 10)
        return;

    uint8_t b7 = payload[7];
    uint8_t b8 = payload[8];
    uint8_t b9 = payload[9];

    bool changed = (b7 != prev_b7) || (b8 != prev_b8) || (b9 != prev_b9);
    bool armable = (state == WAKE_IDLE || state == WAKE_DONE ||
                    state == WAKE_PENDING_PRESS);
    prev_b7 = b7;
    prev_b8 = b8;
    prev_b9 = b9;

    if (changed && armable && host_suspended)
        request_host_wake("button event");
}

void usb_wake_task(void)
{
    const uint64_t now = now_us();

    /*
     * Controller power-off after sustained suspend (battery-save).
     * Runs independently of the wake-UP FSM: if the host has been
     * suspended for longer than the debounce window, power off the
     * controller so it doesn't drain its battery during sleep/shutdown.
     * Transient hub-induced suspends are cancelled by resume/configured
     * clearing suspend_at_us before the timer fires.
     */
    if (suspend_at_us != 0 && host_suspended && !poweroff_sent &&
        now - suspend_at_us >= WAKE_POWEROFF_DEBOUNCE_US) {
        bt_power_off_controller();
        poweroff_sent = true;
        LOG_INF("[WAKE] Suspend debounce elapsed -> controller power off\n");
    }

    switch (state) {
    case WAKE_IDLE:
    case WAKE_DONE:
        return;

    case WAKE_PENDING_PRESS:
        if (!host_suspended) {
            enter_state(WAKE_DONE);
            LOG_INF("[WAKE] Host resumed externally -> DONE\n");
        }
        return;

    case WAKE_REQUESTED: {
        if (host_resumed || !host_suspended) {
            enter_state(WAKE_DONE);
            LOG_INF("[WAKE] Host resumed -> DONE\n");
        } else if (now - state_entered_us > WAKE_REQUEST_TIMEOUT_US) {
            enter_state(WAKE_DONE);
            LOG_INF("[WAKE] REQUESTED timeout -> DONE\n");
        }
        return;
    }

    case WAKE_KEY_DOWN:
    case WAKE_KEY_UP_SENT: {
        /* F15 key path removed — USB resume signal is sufficient on Windows 10/11 */
        enter_state(WAKE_DONE);
        return;
    }
    }
}
