#include "led_status.h"
#include "bflb_gpio.h"
#include "board.h"
#include "board_config.h"
#include <stdint.h>
#include "debug_log.h"

#define BLINK_SLOW_PERIOD   10   /* 500ms per toggle → 1Hz */
#define BLINK_FAST_PERIOD   3    /* 150ms per toggle → ~3Hz */
#define BLINK_BATT_PERIOD   6    /* 300ms */

/* After disconnect blink, auto-transition to slow blink
 * after ~3 seconds (60 ticks at 50ms/tick). */
#define RED_BLINK_DURATION  60

static struct bflb_device_s *gpio_dev;
static enum led_pattern current_pattern = LED_OFF;
static uint32_t tick_count = 0;
static uint32_t pattern_age = 0;
static volatile bool led_locked = false;

/* ── Polarity-aware GPIO helpers ── */

static inline void led_pin_on(uint32_t pin)
{
#if LED_ACTIVE_LOW
    bflb_gpio_reset(gpio_dev, pin);
#else
    bflb_gpio_set(gpio_dev, pin);
#endif
}

static inline void led_pin_off(uint32_t pin)
{
#if LED_ACTIVE_LOW
    bflb_gpio_set(gpio_dev, pin);
#else
    bflb_gpio_reset(gpio_dev, pin);
#endif
}

#if LED_STYLE == LED_AIM61
/* ══════════════════════════════════════════════════════════════
 * LED_AIM61: 4 LEDs (RGB + white), active-high
 * Board: Ai-M61-32S-Kit
 * ══════════════════════════════════════════════════════════════ */

static bool red_on   = false;
static bool green_on = false;
static bool blue_on  = false;
static bool white_on = false;

static void set_red(bool on)   { red_on = on;   if (on) led_pin_on(LED_RED_PIN);   else led_pin_off(LED_RED_PIN); }
static void set_green(bool on) { green_on = on;  if (on) led_pin_on(LED_GREEN_PIN); else led_pin_off(LED_GREEN_PIN); }
static void set_blue(bool on)  { blue_on = on;   if (on) led_pin_on(LED_BLUE_PIN);  else led_pin_off(LED_BLUE_PIN); }
static void set_white(bool on) { white_on = on;  if (on) led_pin_on(LED_WHITE_PIN); else led_pin_off(LED_WHITE_PIN); }

static void all_off(void)
{
    set_red(false);
    set_green(false);
    set_blue(false);
    set_white(false);
}

static void set_yellow(bool on) { set_blue(false); set_red(on); set_green(on); }
static void set_purple(bool on) { set_green(false); set_red(on); set_blue(on); }

void led_status_init(void)
{
    gpio_dev = bflb_device_get_by_name("gpio");
    const uint32_t cfg = GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0;
    bflb_gpio_init(gpio_dev, LED_WHITE_PIN, cfg);
    bflb_gpio_init(gpio_dev, LED_RED_PIN,   cfg);
    bflb_gpio_init(gpio_dev, LED_GREEN_PIN, cfg);
    bflb_gpio_init(gpio_dev, LED_BLUE_PIN,  cfg);
    all_off();
}

void led_status_set(enum led_pattern pattern)
{
    if (led_locked && pattern != LED_BLUE_SOLID)
        return;

    current_pattern = pattern;
    tick_count = 0;
    pattern_age = 0;
    all_off();

    switch (pattern) {
    case LED_PURPLE_BLINK_SLOW:
    case LED_PURPLE_BLINK_FAST:
        set_purple(true);
        break;
    case LED_GREEN_SOLID:
        set_green(true);
        break;
    case LED_BLUE_SOLID:
        set_blue(true);
        break;
    case LED_RED_BLINK:
        set_red(true);
        break;
    case LED_BLINK_BATTERY:
        set_red(true);
        break;
    case LED_BLINK_BATTERY_WARN:
        set_yellow(true);
        break;
    default:
        break;
    }
}

void led_status_tick(void)
{
    tick_count++;
    pattern_age++;

    switch (current_pattern) {
    case LED_OFF:
    case LED_GREEN_SOLID:
    case LED_BLUE_SOLID:
        break;

    case LED_PURPLE_BLINK_SLOW:
        if (tick_count >= BLINK_SLOW_PERIOD) {
            tick_count = 0;
            set_purple(!red_on);
        }
        break;

    case LED_PURPLE_BLINK_FAST:
        if (tick_count >= BLINK_FAST_PERIOD) {
            tick_count = 0;
            set_purple(!red_on);
        }
        break;

    case LED_RED_BLINK:
        if (pattern_age >= RED_BLINK_DURATION) {
            LOG_INF("[LED] Red blink done, switching to purple\n");
            led_status_set(LED_PURPLE_BLINK_SLOW);
            return;
        }
        if (tick_count >= BLINK_SLOW_PERIOD) {
            tick_count = 0;
            set_red(!red_on);
        }
        break;

    case LED_BLINK_ONCE:
        if (tick_count == 1)
            set_blue(true);
        else if (tick_count >= 4) {
            all_off();
            current_pattern = LED_OFF;
        }
        break;

    case LED_BLINK_TRIPLE:
        if (tick_count <= 12) {
            bool on = ((tick_count % 4) < 2);
            set_blue(on);
        } else {
            all_off();
            current_pattern = LED_OFF;
        }
        break;

    case LED_BLINK_BATTERY:
        if (tick_count >= BLINK_BATT_PERIOD) {
            tick_count = 0;
            set_red(!red_on);
        }
        break;

    case LED_BLINK_BATTERY_WARN:
        if (tick_count >= BLINK_BATT_PERIOD) {
            tick_count = 0;
            bool next = !green_on;
            set_blue(false);
            set_green(next);
            set_red(next);
        } else if (green_on) {
            set_red(!red_on);
        }
        break;
    }
}

#elif LED_STYLE == LED_M0SDOCK
/* ══════════════════════════════════════════════════════════════
 * LED_M0SDOCK: 2 red LEDs (GPIO27/28), active-low
 * Board: Sipeed M0S Dock
 *
 * Pattern mapping (no color, use position + blink):
 *   LED0 (GPIO27, near Type-C) = primary indicator
 *   LED1 (GPIO28, far side)    = secondary indicator
 *
 *   Waiting/Pairing (purple slow) → LED0 slow blink
 *   Scanning (purple fast)        → LED0 fast blink
 *   Connected (green)             → LED1 solid
 *   Disconnect (red blink)        → LED0 + LED1 sync blink
 *   Event ack (blink once)        → LED1 flash once
 *   Bonds cleared (triple)        → LED1 triple flash
 *   Battery critical              → LED0 + LED1 alternating
 *   Battery warning               → LED0 fast blink
 * ══════════════════════════════════════════════════════════════ */

static bool led0_on = false;
static bool led1_on = false;

static void set_led0(bool on)
{
    led0_on = on;
    if (on) led_pin_on(LED_PIN_0);
    else    led_pin_off(LED_PIN_0);
}

static void set_led1(bool on)
{
    led1_on = on;
    if (on) led_pin_on(LED_PIN_1);
    else    led_pin_off(LED_PIN_1);
}

static void all_off(void)
{
    set_led0(false);
    set_led1(false);
}

void led_status_init(void)
{
    gpio_dev = bflb_device_get_by_name("gpio");
    const uint32_t cfg = GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0;
    bflb_gpio_init(gpio_dev, LED_PIN_0, cfg);
    bflb_gpio_init(gpio_dev, LED_PIN_1, cfg);
    all_off();
}

void led_status_set(enum led_pattern pattern)
{
    if (led_locked && pattern != LED_BLUE_SOLID)
        return;

    current_pattern = pattern;
    tick_count = 0;
    pattern_age = 0;
    all_off();

    switch (pattern) {
    case LED_PURPLE_BLINK_SLOW:
    case LED_PURPLE_BLINK_FAST:
        set_led0(true);
        break;
    case LED_GREEN_SOLID:
        set_led1(true);
        break;
    case LED_BLUE_SOLID:
        set_led0(true);
        set_led1(true);
        break;
    case LED_RED_BLINK:
        set_led0(true);
        set_led1(true);
        break;
    case LED_BLINK_BATTERY:
        set_led0(true);
        break;
    case LED_BLINK_BATTERY_WARN:
        set_led0(true);
        break;
    default:
        break;
    }
}

void led_status_tick(void)
{
    tick_count++;
    pattern_age++;

    switch (current_pattern) {
    case LED_OFF:
    case LED_GREEN_SOLID:
    case LED_BLUE_SOLID:
        break;

    case LED_PURPLE_BLINK_SLOW:
        if (tick_count >= BLINK_SLOW_PERIOD) {
            tick_count = 0;
            set_led0(!led0_on);
        }
        break;

    case LED_PURPLE_BLINK_FAST:
        if (tick_count >= BLINK_FAST_PERIOD) {
            tick_count = 0;
            set_led0(!led0_on);
        }
        break;

    case LED_RED_BLINK:
        if (pattern_age >= RED_BLINK_DURATION) {
            LOG_INF("[LED] Disconnect blink done, switching to slow blink\n");
            led_status_set(LED_PURPLE_BLINK_SLOW);
            return;
        }
        if (tick_count >= BLINK_SLOW_PERIOD) {
            tick_count = 0;
            bool next = !led0_on;
            set_led0(next);
            set_led1(next);
        }
        break;

    case LED_BLINK_ONCE:
        if (tick_count == 1)
            set_led1(true);
        else if (tick_count >= 4) {
            all_off();
            current_pattern = LED_OFF;
        }
        break;

    case LED_BLINK_TRIPLE:
        if (tick_count <= 12) {
            bool on = ((tick_count % 4) < 2);
            set_led1(on);
        } else {
            all_off();
            current_pattern = LED_OFF;
        }
        break;

    case LED_BLINK_BATTERY:
        if (tick_count >= BLINK_BATT_PERIOD) {
            tick_count = 0;
            set_led0(!led0_on);
            set_led1(!led1_on);
        }
        break;

    case LED_BLINK_BATTERY_WARN:
        if (tick_count >= BLINK_BATT_PERIOD) {
            tick_count = 0;
            set_led0(!led0_on);
        }
        break;
    }
}

#elif LED_STYLE == LED_SINGLE
/* ══════════════════════════════════════════════════════════════
 * LED_SINGLE: 1 user LED (GPIO27), active-low
 * Board: LCTech BL616
 *
 * Pattern mapping (single LED, differentiated by blink cadence):
 *   Waiting/Pairing (purple slow) → slow blink
 *   Scanning (purple fast)        → fast blink
 *   Connected (green)             → solid
 *   Disconnect (red blink)        → medium blink → auto slow blink
 *   Event ack (blink once)        → single flash
 *   Bonds cleared (triple)        → triple flash
 *   Battery critical              → rapid blink
 *   Battery warning               → medium blink
 * ══════════════════════════════════════════════════════════════ */

static bool led_on = false;

static void set_led(bool on)
{
    led_on = on;
    if (on) led_pin_on(LED_PIN);
    else    led_pin_off(LED_PIN);
}

static void all_off(void)
{
    set_led(false);
}

void led_status_init(void)
{
    gpio_dev = bflb_device_get_by_name("gpio");
    const uint32_t cfg = GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0;
    bflb_gpio_init(gpio_dev, LED_PIN, cfg);
    all_off();
}

void led_status_set(enum led_pattern pattern)
{
    if (led_locked && pattern != LED_BLUE_SOLID)
        return;

    current_pattern = pattern;
    tick_count = 0;
    pattern_age = 0;
    all_off();

    switch (pattern) {
    case LED_PURPLE_BLINK_SLOW:
    case LED_PURPLE_BLINK_FAST:
    case LED_RED_BLINK:
    case LED_BLINK_BATTERY:
    case LED_BLINK_BATTERY_WARN:
        set_led(true);
        break;
    case LED_GREEN_SOLID:
    case LED_BLUE_SOLID:
        set_led(true);
        break;
    default:
        break;
    }
}

void led_status_tick(void)
{
    tick_count++;
    pattern_age++;

    switch (current_pattern) {
    case LED_OFF:
    case LED_GREEN_SOLID:
    case LED_BLUE_SOLID:
        break;

    case LED_PURPLE_BLINK_SLOW:
        if (tick_count >= BLINK_SLOW_PERIOD) {
            tick_count = 0;
            set_led(!led_on);
        }
        break;

    case LED_PURPLE_BLINK_FAST:
        if (tick_count >= BLINK_FAST_PERIOD) {
            tick_count = 0;
            set_led(!led_on);
        }
        break;

    case LED_RED_BLINK:
        if (pattern_age >= RED_BLINK_DURATION) {
            LOG_INF("[LED] Disconnect blink done, switching to slow blink\n");
            led_status_set(LED_PURPLE_BLINK_SLOW);
            return;
        }
        if (tick_count >= BLINK_SLOW_PERIOD) {
            tick_count = 0;
            set_led(!led_on);
        }
        break;

    case LED_BLINK_ONCE:
        if (tick_count == 1)
            set_led(true);
        else if (tick_count >= 4) {
            all_off();
            current_pattern = LED_OFF;
        }
        break;

    case LED_BLINK_TRIPLE:
        if (tick_count <= 12) {
            bool on = ((tick_count % 4) < 2);
            set_led(on);
        } else {
            all_off();
            current_pattern = LED_OFF;
        }
        break;

    case LED_BLINK_BATTERY:
        if (tick_count >= BLINK_FAST_PERIOD) {
            tick_count = 0;
            set_led(!led_on);
        }
        break;

    case LED_BLINK_BATTERY_WARN:
        if (tick_count >= BLINK_BATT_PERIOD) {
            tick_count = 0;
            set_led(!led_on);
        }
        break;
    }
}

#else
#error "Unknown LED_STYLE — add a new branch in led_status.c for your board"
#endif /* LED_STYLE */

/* ── Shared API (board-independent) ── */

enum led_pattern led_status_get(void)
{
    return current_pattern;
}

bool led_status_can_auto_off(void)
{
    return current_pattern == LED_GREEN_SOLID ||
           current_pattern == LED_PURPLE_BLINK_SLOW ||
           current_pattern == LED_PURPLE_BLINK_FAST;
}

void led_status_lock(void)
{
    led_locked = true;
}

void led_status_unlock(void)
{
    led_locked = false;
}
