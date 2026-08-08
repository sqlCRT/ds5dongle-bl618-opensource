#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "bflb_gpio.h"

/*
 * Board-level hardware abstraction.
 *
 * Supported boards:
 *   1. Ai-M61-32S-Kit (BL618) — default
 *   2. Sipeed M0S Dock (BL616) — enabled by -DBOARD_M0S_DOCK=1
 *   3. LCTech BL616   (BL616) — enabled by -DBOARD_LCTECH_616=1
 *
 * LED_STYLE decouples LED behavior from board identity.
 * Boards with the same LED layout share the same style; a new layout
 * only requires adding a new LED_STYLE_xxx value and a matching
 * #elif branch in led_status.c.
 *
 *   LED_AIM61   — 4 LEDs: R + G + B + white, active-high
 *   LED_M0SDOCK — 2 single-color LEDs, active-low
 *   LED_SINGLE  — 1 user LED, active-low (power LED hardwired, not controllable)
 */

#define LED_AIM61           1
#define LED_M0SDOCK         2
#define LED_SINGLE          3

#define BOOT_BUTTON_PIN     GPIO_PIN_2

#if defined(BOARD_LCTECH_616)

/* ── LCTech BL616 (BL616) ── */
#define BOARD_NAME          "LCTech-616"
#define LED_STYLE           LED_SINGLE
#define LED_ACTIVE_LOW      1

#define LED_PIN             GPIO_PIN_27  /* blue user LED; power LED is hardwired */

#define USB_NATIVE_TYPEC    1

#elif defined(BOARD_M0S_DOCK)

/* ── Sipeed M0S Dock (BL616 QFN32) ── */
#define BOARD_NAME          "M0S-Dock"
#define LED_STYLE           LED_M0SDOCK
#define LED_ACTIVE_LOW      1

#define LED_PIN_0           GPIO_PIN_27
#define LED_PIN_1           GPIO_PIN_28

#define USB_NATIVE_TYPEC    1

#else /* Ai-M61-32S-Kit (BL618 QFN56) — default */

#define BOARD_NAME          "Ai-M61"
#define LED_STYLE           LED_AIM61
#define LED_ACTIVE_LOW      0

#define LED_WHITE_PIN       GPIO_PIN_29
#define LED_RED_PIN         GPIO_PIN_12
#define LED_GREEN_PIN       GPIO_PIN_14
#define LED_BLUE_PIN        GPIO_PIN_15

#define USB_NATIVE_TYPEC    0

#endif

#endif /* BOARD_CONFIG_H */
