#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>

enum led_pattern {
    LED_OFF,
    LED_PURPLE_BLINK_SLOW, /* Purple slow blink — idle / waiting */
    LED_PURPLE_BLINK_FAST, /* Purple fast blink — scanning */
    LED_GREEN_SOLID,       /* Green solid — connected */
    LED_BLUE_SOLID,        /* Blue solid */
    LED_RED_BLINK,         /* Red blink — just disconnected, auto→purple */
    LED_BLINK_ONCE,        /* Single flash — event acknowledge */
    LED_BLINK_DOUBLE,      /* Double flash — remap profile 2 */
    LED_BLINK_TRIPLE,      /* Triple flash — bonds cleared */
    LED_BLINK_BATTERY,     /* Red blink — critical battery (<=10%) */
    LED_BLINK_BATTERY_WARN,/* Yellow blink — low battery (<=20%) */
};

void led_status_init(void);
void led_status_set(enum led_pattern pattern);
enum led_pattern led_status_get(void);
bool led_status_can_auto_off(void);
void led_status_tick(void);
void led_status_lock(void);
void led_status_unlock(void);

#endif /* LED_STATUS_H */
