#ifndef USB_GAMEPAD_H
#define USB_GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>

#define USB_GAMEPAD_EP_IN       0x84
#define USB_KBD_EP_IN           0x83
#define USB_GAMEPAD_EP_MPS      64
#define USB_KBD_EP_MPS          9     /* Report ID (1) + 8 bytes keyboard */
#define USB_GAMEPAD_INTERVAL_MS 1
#define USB_KBD_INTERVAL_MS     10

#define USB_GAMEPAD_VID         0x054C
#define USB_GAMEPAD_PID         0x0CE6

#define USB_INTF_GAMEPAD        3   /* shifted: 0=AC, 1=AS_SPK, 2=AS_MIC, 3=Gamepad */
#define USB_INTF_KBD            4

#define DS5_USB_REPORT_ID_INPUT   0x01
#define DS5_USB_REPORT_ID_OUTPUT  0x02
#define DS5_USB_INPUT_PAYLOAD_LEN  63
#define DS5_USB_OUTPUT_PAYLOAD_LEN 47
#define FEATURE_DATA_MAX           256

typedef void (*usb_gamepad_output_cb_t)(const uint8_t *data, uint16_t len);

int usb_gamepad_init(usb_gamepad_output_cb_t output_cb);

int usb_gamepad_send_raw_input(const uint8_t *payload);
bool usb_gamepad_is_ready(void);
int  usb_gamepad_send_kbd_report(const uint8_t *report, uint8_t len);
int  usb_gamepad_send_consumer_report(uint16_t usage);
int  usb_gamepad_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
bool usb_gamepad_kbd_ready(void);

void usb_gamepad_set_suspend_hooks(void (*on_suspend)(void),
                                   void (*on_resume)(void),
                                   void (*on_configured)(void));

void usb_gamepad_set_polling_rate(uint8_t mode);

void usb_gamepad_set_dse_mode(bool dse);

void usb_gamepad_process_deferred(void);

void usb_soft_disconnect(void);
void usb_soft_connect(void);

#endif /* USB_GAMEPAD_H */
