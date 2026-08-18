#include "usb_gamepad.h"
#include "compiler/compiler_ld.h"
#include "ds5_usb_audio.h"
#include "audio.h"
#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "dse.h"
#include "config.h"
#include "state_mgr.h"
#include "remap.h"
#include "usbd_core.h"
#include "usbd_hid.h"
#include "bl616_glb.h"
#include "bflb_efuse.h"
#include "bflb_mtimer.h"
#include "board.h"
#include <string.h>
#include "board_config.h"
#include "debug_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#if defined(BOARD_LCTECH_616)
  #ifdef FORCE_FS_MODE
    #define FIRMWARE_VERSION "LCT616-DS5 3.17"
  #else
    #define FIRMWARE_VERSION "LCT616-DS5 3.17H"
  #endif
#elif defined(BOARD_M0S_DOCK)
#define FIRMWARE_VERSION "M0S-DS5 3.5"
#else
#define FIRMWARE_VERSION "BL618-DS5 3.5"
#endif

#define USBD_MAX_POWER      250
#define USBD_LANGID         0x0409

/* Config(9) + Audio(186) + Gamepad(9+9+7) + Keyboard(9+9+7) = 245 */
#define USB_KBD_DESC_SIZE  25   /* intf(9) + HID(9) + EP_IN(7) */
#define USB_HID_ONLY_SIZE (9 + 9 + 7 + USB_KBD_DESC_SIZE)
#define USB_AUDIO_DESC_SIZE 186
#define USB_HID_CONFIG_SIZE (9 + USB_AUDIO_DESC_SIZE + USB_HID_ONLY_SIZE)
#define HID_REPORT_DESC_SIZE_DS  329
#define HID_REPORT_DESC_SIZE_DSE 445
#define KBD_REPORT_DESC_SIZE 80

static bool current_dse_mode = false;

static volatile bool usb_config_save_pending = false;
static volatile bool usb_reset_pending = false;
static volatile bool usb_remap_save_pending  = false;
static volatile uint8_t usb_remap_save_profile = 0;
static uint8_t remap_read_profile = 0;
static bool first_usb_send_logged = false;

#define SET_REPORT_MAX_DATA    64
#define SET_REPORT_QUEUE_DEPTH 4

typedef struct {
    uint8_t  report_id;
    uint16_t len;
    bool     is_dse;
    uint8_t  data[SET_REPORT_MAX_DATA];
} set_report_entry_t;

static QueueHandle_t set_report_queue = NULL;

/* Pending feature GET request, deferred from ISR to task context.
 * 0 = no pending request. Written by ISR, consumed by process_deferred(). */
static volatile uint8_t pending_feature_rid = 0;

/* DualSense USB HID Report Descriptor (321 bytes) */
static const uint8_t hid_report_desc_ds[HID_REPORT_DESC_SIZE_DS] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop Ctrls) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xA1, 0x01,       /* Collection (Application) */

    /* Input Report 0x01 -------------------------------------------- */
    0x85, 0x01,       /*   Report ID (1) */
    0x09, 0x30,       /*   Usage (X)  — LX */
    0x09, 0x31,       /*   Usage (Y)  — LY */
    0x09, 0x32,       /*   Usage (Z)  — RX */
    0x09, 0x35,       /*   Usage (Rz) — RY */
    0x09, 0x33,       /*   Usage (Rx) — L2 trigger */
    0x09, 0x34,       /*   Usage (Ry) — R2 trigger */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x06,       /*   Report Count (6) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x06, 0x00, 0xFF, /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x20,       /*   Usage (0x20) — counter byte */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x05, 0x01,       /*   Usage Page (Generic Desktop Ctrls) */
    0x09, 0x39,       /*   Usage (Hat switch) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x07,       /*   Logical Maximum (7) */
    0x35, 0x00,       /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01, /*   Physical Maximum (315) */
    0x65, 0x14,       /*   Unit (Degrees) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x42,       /*   Input (Data,Var,Abs,Null) */
    0x65, 0x00,       /*   Unit (None) */
    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Minimum (0x01) */
    0x29, 0x0F,       /*   Usage Maximum (0x0F) — 15 buttons */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x0F,       /*   Report Count (15) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x06, 0x00, 0xFF, /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x21,       /*   Usage (0x21) — 13 vendor bits (padding+flags) */
    0x95, 0x0D,       /*   Report Count (13) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x06, 0x00, 0xFF, /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x22,       /*   Usage (0x22) — sensor/touch/battery blob */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x34,       /*   Report Count (52) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */

    /* Output Report 0x02 (47 bytes) -------------------------------- */
    0x85, 0x02,       /*   Report ID (2) */
    0x09, 0x23,       /*   Usage (0x23) */
    0x95, 0x2F,       /*   Report Count (47) */
    0x91, 0x02,       /*   Output (Data,Var,Abs) */

    /* Feature Reports ---------------------------------------------- */
    0x85, 0x05,       /*   Report ID (5) — calibration */
    0x09, 0x33,
    0x95, 0x28,       /*   Report Count (40) */
    0xB1, 0x02,
    0x85, 0x08,       /*   Report ID (8) */
    0x09, 0x34,
    0x95, 0x2F,       /*   Report Count (47) */
    0xB1, 0x02,
    0x85, 0x09,       /*   Report ID (9) — pairing info */
    0x09, 0x24,
    0x95, 0x13,       /*   Report Count (19) */
    0xB1, 0x02,
    0x85, 0x0A,       /*   Report ID (10) */
    0x09, 0x25,
    0x95, 0x1A,       /*   Report Count (26) */
    0xB1, 0x02,
    0x85, 0x0B,       /*   Report ID (11) */
    0x09, 0x41,
    0x95, 0x29,       /*   Report Count (41) */
    0xB1, 0x02,
    0x85, 0x0C,       /*   Report ID (12) */
    0x09, 0x42,
    0x95, 0x29,       /*   Report Count (41) */
    0xB1, 0x02,
    0x85, 0x20,       /*   Report ID (32) — firmware info */
    0x09, 0x26,
    0x95, 0x3F,       /*   Report Count (63) */
    0xB1, 0x02,
    0x85, 0x21,       /*   Report ID (33) */
    0x09, 0x27,
    0x95, 0x04,       /*   Report Count (4) */
    0xB1, 0x02,
    0x85, 0x22,       /*   Report ID (34) */
    0x09, 0x40,
    0x95, 0x3F,       /*   Report Count (63) */
    0xB1, 0x02,
    0x85, 0x80,       /*   Report ID (128) */
    0x09, 0x28,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x81,       /*   Report ID (129) */
    0x09, 0x29,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x82,       /*   Report ID (130) */
    0x09, 0x2A,
    0x95, 0x09,       /*   Report Count (9) */
    0xB1, 0x02,
    0x85, 0x83,       /*   Report ID (131) */
    0x09, 0x2B,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x84,       /*   Report ID (132) */
    0x09, 0x2C,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x85,       /*   Report ID (133) */
    0x09, 0x2D,
    0x95, 0x02,       /*   Report Count (2) */
    0xB1, 0x02,
    0x85, 0xA0,       /*   Report ID (160) */
    0x09, 0x2E,
    0x95, 0x01,       /*   Report Count (1) */
    0xB1, 0x02,
    0x85, 0xE0,       /*   Report ID (224) */
    0x09, 0x2F,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF0,       /*   Report ID (240) */
    0x09, 0x30,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF1,       /*   Report ID (241) */
    0x09, 0x31,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF2,       /*   Report ID (242) */
    0x09, 0x32,
    0x95, 0x0F,       /*   Report Count (15) */
    0xB1, 0x02,
    0x85, 0xF4,       /*   Report ID (244) */
    0x09, 0x35,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF5,       /*   Report ID (245) */
    0x09, 0x36,
    0x95, 0x03,       /*   Report Count (3) */
    0xB1, 0x02,
    0x85, 0xF6,       /*   Report ID (246) */
    0x09, 0x37,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF7,       /*   Report ID (247) */
    0x09, 0x38,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF8,       /*   Report ID (248) */
    0x09, 0x39,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF9,       /*   Report ID (249) */
    0x09, 0x3A,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xFB,       /*   Report ID (251) — button remap table */
    0x09, 0x3C,
    0x95, 0x3F,
    0xB1, 0x02,

    0xC0,             /* End Collection */
};

/* DualSense Edge USB HID Report Descriptor (437 bytes)
 * Differences from DS: Output 0x02 count 63 (vs 47), Feature 0xF2 count 52
 * (vs 15), plus DSE-specific Feature Reports 0x60-0x65, 0x68, 0x70-0x7B. */
static const uint8_t hid_report_desc_dse[HID_REPORT_DESC_SIZE_DSE] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39,
    0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x65, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x22,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02,
    /* Output Report 0x02 — 63 bytes (DSE) */
    0x85, 0x02, 0x09, 0x23, 0x95, 0x3F, 0x91, 0x02,
    /* Feature Reports (shared with DS) */
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1, 0x02,
    0x85, 0x08, 0x09, 0x34, 0x95, 0x2F, 0xB1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xB1, 0x02,
    0x85, 0x0A, 0x09, 0x25, 0x95, 0x1A, 0xB1, 0x02,
    0x85, 0x0B, 0x09, 0x41, 0x95, 0x29, 0xB1, 0x02,
    0x85, 0x0C, 0x09, 0x42, 0x95, 0x29, 0xB1, 0x02,
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xB1, 0x02,
    0x85, 0x22, 0x09, 0x40, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x80, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x82, 0x09, 0x2A, 0x95, 0x09, 0xB1, 0x02,
    0x85, 0x83, 0x09, 0x2B, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x84, 0x09, 0x2C, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x85, 0x09, 0x2D, 0x95, 0x02, 0xB1, 0x02,
    0x85, 0xA0, 0x09, 0x2E, 0x95, 0x01, 0xB1, 0x02,
    0x85, 0xE0, 0x09, 0x2F, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF0, 0x09, 0x30, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF1, 0x09, 0x31, 0x95, 0x3F, 0xB1, 0x02,
    /* Feature 0xF2 — 52 bytes in DSE (vs 15 in DS) */
    0x85, 0xF2, 0x09, 0x32, 0x95, 0x34, 0xB1, 0x02,
    0x85, 0xF4, 0x09, 0x35, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF5, 0x09, 0x36, 0x95, 0x03, 0xB1, 0x02,
    /* DSE-specific Feature Reports */
    0x85, 0x60, 0x09, 0x41, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x61, 0x09, 0x42, 0xB1, 0x02,
    0x85, 0x62, 0x09, 0x43, 0xB1, 0x02,
    0x85, 0x63, 0x09, 0x44, 0xB1, 0x02,
    0x85, 0x64, 0x09, 0x45, 0xB1, 0x02,
    0x85, 0x65, 0x09, 0x46, 0xB1, 0x02,
    0x85, 0x68, 0x09, 0x47, 0xB1, 0x02,
    0x85, 0x70, 0x09, 0x48, 0xB1, 0x02,
    0x85, 0x71, 0x09, 0x49, 0xB1, 0x02,
    0x85, 0x72, 0x09, 0x4A, 0xB1, 0x02,
    0x85, 0x73, 0x09, 0x4B, 0xB1, 0x02,
    0x85, 0x74, 0x09, 0x4C, 0xB1, 0x02,
    0x85, 0x75, 0x09, 0x4D, 0xB1, 0x02,
    0x85, 0x76, 0x09, 0x4E, 0xB1, 0x02,
    0x85, 0x77, 0x09, 0x4F, 0xB1, 0x02,
    0x85, 0x78, 0x09, 0x50, 0xB1, 0x02,
    0x85, 0x79, 0x09, 0x51, 0xB1, 0x02,
    0x85, 0x7A, 0x09, 0x52, 0xB1, 0x02,
    0x85, 0x7B, 0x09, 0x53, 0xB1, 0x02,
    /* Dongle config Feature Reports (shared) */
    0x85, 0xF6, 0x09, 0x37, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF7, 0x09, 0x38, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF8, 0x09, 0x39, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF9, 0x09, 0x3A, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xFB, 0x09, 0x3C, 0x95, 0x3F, 0xB1, 0x02,  /* button remap table */
    0xC0,
};

/* Multi-TLC HID report descriptor: Keyboard (Report ID 1) + Consumer Control (Report ID 2) */
static const uint8_t kbd_report_desc[KBD_REPORT_DESC_SIZE] = {
    /* ── Collection 1: Keyboard (Report ID 1, 9 bytes total) ── */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,       /*   Usage Minimum (Left Control) */
    0x29, 0xE7,       /*   Usage Maximum (Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) — modifier byte */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Const) — reserved byte */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x00, /*   Usage Maximum (255) */
    0x81, 0x00,       /*   Input (Data,Array) — 6 keycodes */
    0xC0,             /* End Collection */

    /* ── Collection 2: Consumer Control (Report ID 2, 1 byte bitmap) ── */
    0x05, 0x0C,       /* Usage Page (Consumer) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x02,       /*   Report ID (2) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x03,       /*   Report Count (3) — 3 bits: VolUp, VolDn, Mute */
    0x09, 0xE9,       /*   Usage (Volume Increment) — bit 0 */
    0x09, 0xEA,       /*   Usage (Volume Decrement) — bit 1 */
    0x09, 0xE2,       /*   Usage (Mute)             — bit 2 */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x05,       /*   Report Size (5) — padding */
    0x81, 0x01,       /*   Input (Const) */
    0xC0,             /* End Collection */
};

static uint8_t device_desc[] = {
    0x12,                    /* bLength */
    0x01,                    /* bDescriptorType: Device */
    0x00, 0x02,              /* bcdUSB: 2.00 */
    0x00,                    /* bDeviceClass */
    0x00,                    /* bDeviceSubClass */
    0x00,                    /* bDeviceProtocol */
    0x40,                    /* bMaxPacketSize0 */
    (USB_GAMEPAD_VID & 0xFF), (USB_GAMEPAD_VID >> 8),
    (USB_GAMEPAD_PID & 0xFF), (USB_GAMEPAD_PID >> 8),
    0x00, 0x01,              /* bcdDevice: 1.00 */
    0x01,                    /* iManufacturer */
    0x02,                    /* iProduct */
    0x00,                    /* iSerialNumber: none */
    0x01,                    /* bNumConfigurations */
};

#define DEVICE_DESC_PID_OFF         10   /* idProduct low byte in device_desc */

/* Byte offsets within config_desc for dynamic modification.
 * Audio desc = 186 bytes at offset 9, HID starts at 9+186 = 195.
 * Gamepad intf(9) at 195, HID desc(9) at 204, wDescriptorLength at byte 7 of HID = 211. */
#define AUDIO_DESC_OFFSET           9    /* audio descriptor starts after config header */
#define HID_SECTION_OFFSET          (AUDIO_DESC_OFFSET + USB_AUDIO_DESC_SIZE)  /* 195 */
#define CONFIG_DESC_WDLEN_OFF       (HID_SECTION_OFFSET + 9 + 7)  /* 211: gamepad HID wDescriptorLength */
#define BINTERVAL_GAMEPAD_EP_IN_OFF (HID_SECTION_OFFSET + 9 + 9 + 6) /* 219 */
#define BINTERVAL_GAMEPAD_EP_OUT_OFF (BINTERVAL_GAMEPAD_EP_IN_OFF + 7) /* 226 */
#define KBD_INTF_OFF  (BINTERVAL_GAMEPAD_EP_OUT_OFF + 1) /* 227: keyboard intf descriptor */
#define KBD_SUBCLASS_OFF (KBD_INTF_OFF + 6)  /* bInterfaceSubClass in kbd intf */
#define KBD_PROTOCOL_OFF (KBD_INTF_OFF + 7)  /* bInterfaceProtocol in kbd intf */

static uint8_t config_desc[USB_HID_CONFIG_SIZE] = {
    /* Configuration Descriptor (9 bytes) */
    0x09, 0x02,
    USB_HID_CONFIG_SIZE & 0xFF, (USB_HID_CONFIG_SIZE >> 8) & 0xFF,
    0x05,                    /* bNumInterfaces: AC + AS_SPK + AS_MIC + Gamepad + Kbd */
    0x01,                    /* bConfigurationValue */
    0x00,                    /* iConfiguration */
    0xE0,                    /* bmAttributes: self-powered + remote wakeup */
    USBD_MAX_POWER,          /* bMaxPower: 500mA */

    /* ---- Audio descriptors (194 bytes) injected at init ---- */
    /* Placeholder: filled by memcpy from usb_audio_get_desc() in init */
#define AUDIO_PLACEHOLDER_BYTES 186
    [AUDIO_DESC_OFFSET ... AUDIO_DESC_OFFSET + AUDIO_PLACEHOLDER_BYTES - 1] = 0,

    /* ---- Interface 2: DualSense Gamepad ---- */
    0x09, 0x04,
    USB_INTF_GAMEPAD,        /* bInterfaceNumber: 2 */
    0x00,                    /* bAlternateSetting */
    0x01,                    /* bNumEndpoints: IN only (OUT via EP0 SET_REPORT) */
    0x03,                    /* bInterfaceClass: HID */
    0x00,                    /* bInterfaceSubClass */
    0x00,                    /* bInterfaceProtocol */
    0x00,                    /* iInterface */

    /* HID Descriptor (Gamepad) */
    0x09, 0x21,
    0x11, 0x01,              /* bcdHID: 1.11 */
    0x00,                    /* bCountryCode */
    0x01,                    /* bNumDescriptors */
    0x22,                    /* bDescriptorType: Report */
    HID_REPORT_DESC_SIZE_DS & 0xFF, (HID_REPORT_DESC_SIZE_DS >> 8) & 0xFF,

    /* EP IN (Gamepad) */
    0x07, 0x05,
    USB_GAMEPAD_EP_IN,       /* bEndpointAddress */
    0x03,                    /* bmAttributes: Interrupt */
    USB_GAMEPAD_EP_MPS, 0x00,
    USB_GAMEPAD_INTERVAL_MS,

    /* ---- Interface 4: Keyboard (PS shortcut / remap / wake key) ---- */
    0x09, 0x04,
    USB_INTF_KBD,            /* bInterfaceNumber: 4 */
    0x00,                    /* bAlternateSetting */
    0x01,                    /* bNumEndpoints: IN only */
    0x03,                    /* bInterfaceClass: HID */
    0x01,                    /* bInterfaceSubClass: Boot Interface Subclass */
    0x01,                    /* bInterfaceProtocol: Keyboard — forces kbdhid.sys on Windows */
    0x00,                    /* iInterface */

    /* HID Descriptor (Keyboard) */
    0x09, 0x21,
    0x11, 0x01,              /* bcdHID: 1.11 */
    0x00,                    /* bCountryCode */
    0x01,                    /* bNumDescriptors */
    0x22,                    /* bDescriptorType: Report */
    KBD_REPORT_DESC_SIZE & 0xFF, (KBD_REPORT_DESC_SIZE >> 8) & 0xFF,

    /* EP IN (Keyboard) */
    0x07, 0x05,
    USB_KBD_EP_IN,           /* bEndpointAddress */
    0x03,                    /* bmAttributes: Interrupt */
    USB_KBD_EP_MPS, 0x00,
    USB_KBD_INTERVAL_MS,
};

/* ---- Advance-mode descriptor callbacks ---- */

static const uint8_t *desc_device_cb(uint8_t speed)
{
    (void)speed;
    device_desc[16] = config_get()->enable_usb_sn ? 0x03 : 0x00;
    return device_desc;
}

static const uint8_t *desc_config_cb(uint8_t speed)
{
    (void)speed;
    return config_desc;
}

static const uint8_t device_qualifier_desc[] = {
    0x0A,        /* bLength */
    0x06,        /* bDescriptorType: Device Qualifier */
    0x00, 0x02,  /* bcdUSB: 2.00 */
    0x00, 0x00, 0x00,
    0x40,        /* bMaxPacketSize0: 64 */
    0x01, 0x00,
};

static const uint8_t *desc_qualifier_cb(uint8_t speed)
{
    (void)speed;
    return device_qualifier_desc;
}

static uint8_t other_speed_desc[USB_HID_CONFIG_SIZE];

static const uint8_t *desc_other_speed_cb(uint8_t speed)
{
    (void)speed;
    memcpy(other_speed_desc, config_desc, sizeof(other_speed_desc));
    other_speed_desc[1] = 0x07;
    uint16_t total = other_speed_desc[2] | (other_speed_desc[3] << 8);
    for (uint16_t i = 0; i + 2 < total && i + 2 < sizeof(other_speed_desc); ) {
        uint8_t len = other_speed_desc[i];
        if (len < 2) break;
        if (other_speed_desc[i + 1] == 0x05 && len >= 7) {
            uint8_t bmAttr = other_speed_desc[i + 3];
            if ((bmAttr & 0x03) == 0x01)
                other_speed_desc[i + 6] = 1;   /* ISO: FS bInterval=1 (1ms) */
            else if ((bmAttr & 0x03) == 0x03)
                other_speed_desc[i + 6] = other_speed_desc[i + 6]; /* INT: keep as-is */
        }
        i += len;
    }
    return other_speed_desc;
}

static char serial_str[24]; /* "xxxxxxxxxxxx xxxx" max 16 hex + nul */
static bool serial_init = false;

static const char *desc_string_cb(uint8_t speed, uint8_t index)
{
    (void)speed;
    switch (index) {
    case 0: return "\x09\x04";
    case 1: return "Sony Interactive Entertainment";
    case 2: return current_dse_mode
                 ? "DualSense Edge Wireless Controller"
                 : "DualSense Wireless Controller";
    case 3:
        if (!serial_init) {
            uint8_t chipid[8];
            bflb_efuse_get_chipid(chipid);
            snprintf(serial_str, sizeof(serial_str),
                     "%02X%02X%02X%02X%02X%02X%02X%02X",
                     chipid[0], chipid[1], chipid[2], chipid[3],
                     chipid[4], chipid[5], chipid[6], chipid[7]);
            serial_init = true;
            LOG_INF("[USB] Serial: %s\n", serial_str);
        }
        return serial_str;
    default: return NULL;
    }
}

static const struct usb_descriptor usb_desc = {
    .device_descriptor_callback          = desc_device_cb,
    .config_descriptor_callback          = desc_config_cb,
    .device_quality_descriptor_callback  = desc_qualifier_cb,
    .other_speed_descriptor_callback     = desc_other_speed_cb,
    .string_descriptor_callback          = desc_string_cb,
    .msosv1_descriptor                   = NULL,
    .msosv2_descriptor                   = NULL,
    .webusb_url_descriptor               = NULL,
    .bos_descriptor                      = NULL,
};

/* Gamepad interface / endpoints */
static struct usbd_interface hid_intf;
static struct usbd_endpoint  hid_ep_in;
/* EP OUT removed: output reports arrive via EP0 SET_REPORT to avoid
 * FIFO F2 sharing conflict with keyboard EP3 IN. */

/* Keyboard interface / endpoint */
static struct usbd_interface kbd_intf;
static struct usbd_endpoint  kbd_ep_in;

static volatile bool usb_configured = false;
static volatile bool ep_in_busy = false;
static volatile bool kbd_ep_busy = false;
static volatile uint64_t kbd_ep_busy_since_us = 0;
static bool kbd_registered = false;
static usb_gamepad_output_cb_t output_callback = NULL;

/* Suspend/resume hooks for wake module */
static void (*hook_suspend)(void)    = NULL;
static void (*hook_resume)(void)     = NULL;
static void (*hook_configured)(void) = NULL;

static USB_NOCACHE_RAM_SECTION uint8_t usb_in_buf[64];
static USB_NOCACHE_RAM_SECTION uint8_t kbd_buf[USB_KBD_EP_MPS];

static volatile uint8_t pending_payload[DS5_USB_INPUT_PAYLOAD_LEN];
static volatile bool    pending_active = false;

ATTR_TCM_SECTION
static void try_send_pending(void)
{
    if (!pending_active || !usb_configured || ep_in_busy)
         return;
    usb_in_buf[0] = DS5_USB_REPORT_ID_INPUT;
    memcpy(usb_in_buf + 1, (const void *)pending_payload,
           DS5_USB_INPUT_PAYLOAD_LEN);
    ep_in_busy = true;
    int ret = usbd_ep_start_write(0, USB_GAMEPAD_EP_IN, usb_in_buf, 64);
    if (ret < 0) {
        ep_in_busy = false;
    } else if (!first_usb_send_logged) {
        first_usb_send_logged = true;
        LOG_INF("[USB] First input: [%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x]\n",
               usb_in_buf[0], usb_in_buf[1], usb_in_buf[2], usb_in_buf[3],
               usb_in_buf[4], usb_in_buf[5], usb_in_buf[6], usb_in_buf[7],
               usb_in_buf[8], usb_in_buf[9]);
    }
}

ATTR_TCM_SECTION
static void hid_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    ep_in_busy = false;
    try_send_pending();
}

static void kbd_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    kbd_ep_busy = false;
    kbd_ep_busy_since_us = 0;
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_CONFIGURED:
        usb_configured = true;
        ep_in_busy = false;
        kbd_ep_busy = false;
        LOG_INF("[USB-EVT] CONFIGURED — host enumeration complete!\n");
        if (hook_configured)
            hook_configured();
        break;
    case USBD_EVENT_RESET:
        usb_configured = false;
        ep_in_busy = false;
        kbd_ep_busy = false;
        first_usb_send_logged = false;
        usb_audio_stop();
        usb_audio_mic_stop();
        audio_set_mic_active(false);
        audio_reset_encoder();
        LOG_INF("[USB-EVT] RESET — host detected device, bus reset sent\n");
        break;
    case USBD_EVENT_SUSPEND:
        usb_configured = false;
        ep_in_busy = false;
        kbd_ep_busy = false;
        usb_audio_stop();
        usb_audio_mic_stop();
        audio_set_mic_active(false);
        LOG_INF("[USB-EVT] SUSPEND\n");
        if (hook_suspend)
            hook_suspend();
        break;
    case USBD_EVENT_RESUME:
        usb_configured = true;
        ep_in_busy = false;
        kbd_ep_busy = false;
        LOG_INF("[USB-EVT] RESUME\n");
        if (hook_resume)
            hook_resume();
        break;
    case USBD_EVENT_CONNECTED:
        LOG_INF("[USB-EVT] CONNECTED (VBUS detected)\n");
        break;
    case USBD_EVENT_DISCONNECTED:
        LOG_INF("[USB-EVT] DISCONNECTED (VBUS lost)\n");
        break;
    case USBD_EVENT_SET_REMOTE_WAKEUP:
        LOG_INF("[USB-EVT] SET_REMOTE_WAKEUP\n");
        break;
    case USBD_EVENT_CLR_REMOTE_WAKEUP:
        LOG_INF("[USB-EVT] CLR_REMOTE_WAKEUP\n");
        break;
    default:
        LOG_INF("[USB-EVT] UNKNOWN event=%d\n", event);
        break;
    }
}

void usb_gamepad_set_dse_mode(bool dse)
{
    current_dse_mode = dse;

    uint16_t pid = dse ? DS5_EDGE_PID : DS5_PID;
    device_desc[DEVICE_DESC_PID_OFF]     = pid & 0xFF;
    device_desc[DEVICE_DESC_PID_OFF + 1] = (pid >> 8) & 0xFF;

    uint16_t rdl = dse ? HID_REPORT_DESC_SIZE_DSE : HID_REPORT_DESC_SIZE_DS;
    config_desc[CONFIG_DESC_WDLEN_OFF]     = rdl & 0xFF;
    config_desc[CONFIG_DESC_WDLEN_OFF + 1] = (rdl >> 8) & 0xFF;

    LOG_INF("[USB] DSE mode: %s (PID=0x%04X, rdl=%u)\n",
           dse ? "Edge" : "Standard", pid, rdl);
}

ATTR_TCM_SECTION
void usb_gamepad_process_deferred(void)
{
    if (usb_reset_pending) {
        usb_reset_pending = false;
        config_save();
        LOG_INF("[USB] CMD 0x03: USB reconnect via reset\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        GLB_SW_System_Reset();
    }
    if (usb_config_save_pending) {
        usb_config_save_pending = false;
        config_save();
        LOG_INF("[USB] CMD 0x02: config saved\n");
    }
    if (usb_remap_save_pending) {
        usb_remap_save_pending = false;
        remap_save_profile(usb_remap_save_profile);
        LOG_INF("[USB] Remap profile %d saved to flash\n", usb_remap_save_profile);
    }

    uint8_t frid = pending_feature_rid;
    if (frid != 0) {
        pending_feature_rid = 0;
        bt_hid_host_get_feature(frid);
        LOG_INF("[USB] Deferred GET_REPORT(Feature 0x%02x) → BT\n", frid);
    }

    set_report_entry_t entry;
    while (set_report_queue &&
           xQueueReceive(set_report_queue, &entry, 0) == pdTRUE) {
        LOG_INF("[USB] Deferred SET_REPORT(0x%02x) → BT, %u bytes%s\n",
               entry.report_id, entry.len, entry.is_dse ? " (DSE)" : "");
        bt_hid_host_set_feature_crc(entry.report_id, entry.data, entry.len);
    }
}

int usb_gamepad_init(usb_gamepad_output_cb_t output_cb)
{
    output_callback = output_cb;
    memset(usb_in_buf, 0, sizeof(usb_in_buf));
    memset(kbd_buf, 0, sizeof(kbd_buf));

    if (!set_report_queue)
        set_report_queue = xQueueCreate(SET_REPORT_QUEUE_DEPTH,
                                        sizeof(set_report_entry_t));

    LOG_INF("[USB-INIT] === USB Initialization Start ===\n");

    /* Inject audio descriptor bytes into config_desc */
    uint16_t audio_len;
    const uint8_t *audio_bytes = usb_audio_get_desc(&audio_len);
    memcpy(&config_desc[AUDIO_DESC_OFFSET], audio_bytes, audio_len);
    LOG_INF("[USB-INIT] Audio desc injected: %u bytes at offset %d\n",
           audio_len, AUDIO_DESC_OFFSET);

    /* Include keyboard interface ONLY when ps_shortcut needs to send key combos.
     * enable_wake now uses USB remote-wake signal only (F15 key removed) —
     * no keyboard interface needed. remap KBD type is disabled for now.
     * REMOTE-WAKEUP bit stays in bmAttributes when wake is enabled. */
    const bool need_kbd = remap_has_kbd_targets() ||
                          config_get()->ps_shortcut_enabled;
    const bool need_remote_wake = config_get()->enable_wake || need_kbd;
    if (need_kbd) {
        config_desc[KBD_SUBCLASS_OFF] = 0x00; /* No subclass (multi-TLC, not pure boot) */
        config_desc[KBD_PROTOCOL_OFF] = 0x00; /* None — keyboard + consumer control */
        LOG_INF("[USB-INIT] Keyboard+CC intf: INCLUDED\n");
        config_desc[7] = need_remote_wake ? 0xE0 : 0xC0;
        /* wTotalLength and bNumInterfaces stay at compiled-in max */
    } else {
        /* Shrink descriptor to exclude keyboard — adjust header only,
         * the trailing 25 bytes are simply invisible to the host. */
        uint16_t total = USB_HID_CONFIG_SIZE - USB_KBD_DESC_SIZE;
        config_desc[2] = total & 0xFF;
        config_desc[3] = (total >> 8) & 0xFF;
        config_desc[4] = 0x04; /* bNumInterfaces: AC + AS_SPK + AS_MIC + Gamepad */
        config_desc[7] = need_remote_wake ? 0xE0 : 0xC0;
        LOG_INF("[USB-INIT] Keyboard intf: EXCLUDED (wake=%d ps=%d)\n",
               config_get()->enable_wake, config_get()->ps_shortcut_enabled);
    }

    usbd_desc_register(0, &usb_desc);
    LOG_INF("[USB-INIT] Descriptors registered (dev=%uB, cfg=%uB)\n",
           (unsigned)sizeof(device_desc), (unsigned)sizeof(config_desc));

    /* Interface 0,1,2: Audio Control + Streaming OUT + Streaming IN */
    usb_audio_register(0);
    LOG_INF("[USB-INIT] Audio interfaces registered\n");

    /* Interface 3: Gamepad — select descriptor based on DSE mode */
    const uint8_t *rd  = current_dse_mode ? hid_report_desc_dse : hid_report_desc_ds;
    uint16_t rd_len    = current_dse_mode ? sizeof(hid_report_desc_dse) : sizeof(hid_report_desc_ds);
    usbd_add_interface(0, usbd_hid_init_intf(0, &hid_intf, rd, rd_len));
    LOG_INF("[USB-INIT] Gamepad HID intf added (report_desc=%uB, DSE=%d)\n",
           rd_len, current_dse_mode);

    /* Interface 4: Keyboard — only register when needed (DS5Dongle style) */
    kbd_registered = need_kbd;
    if (need_kbd) {
        usbd_add_interface(0, usbd_hid_init_intf(
            0, &kbd_intf, kbd_report_desc, sizeof(kbd_report_desc)));
        LOG_INF("[USB-INIT] Keyboard HID intf added\n");
    }

    hid_ep_in.ep_addr = USB_GAMEPAD_EP_IN;
    hid_ep_in.ep_cb = hid_ep_in_handler;
    usbd_add_endpoint(0, &hid_ep_in);


    if (need_kbd) {
        kbd_ep_in.ep_addr = USB_KBD_EP_IN;
        kbd_ep_in.ep_cb = kbd_ep_in_handler;
        usbd_add_endpoint(0, &kbd_ep_in);
    }
    LOG_INF("[USB-INIT] Endpoints: GP_IN=0x%02x KBD_IN=0x%02x (OUT via EP0)\n",
           USB_GAMEPAD_EP_IN, need_kbd ? USB_KBD_EP_IN : 0);

    /* Read registers BEFORE usbd_initialize */
    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
        LOG_INF("[USB-INIT] PRE-init: DEV_CTL=0x%08lx PHY_TST=0x%08lx\n",
               (unsigned long)*dev_ctl, (unsigned long)*phy_tst);
    }

    LOG_INF("[USB-INIT] Calling usbd_initialize(0, 0x%08lx, handler)...\n",
           (unsigned long)USB_BASE);
    usbd_initialize(0, USB_BASE, usbd_event_handler);
    LOG_INF("[USB-INIT] usbd_initialize returned\n");

    /* Read registers AFTER usbd_initialize (before fixup) */
    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
        LOG_INF("[USB-INIT] POST-init (raw): DEV_CTL=0x%08lx PHY_TST=0x%08lx\n",
               (unsigned long)*dev_ctl, (unsigned long)*phy_tst);
        LOG_INF("[USB-INIT]   GLINT_EN=%d CHIP_EN=%d FORCE_FS=%d UNPLUG=%d\n",
               (int)((*dev_ctl >> 2) & 1), (int)((*dev_ctl >> 5) & 1),
               (int)((*dev_ctl >> 9) & 1), (int)(*phy_tst & 1));
    }

    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        uint32_t v = *dev_ctl;
        v |= (1 << 2);  /* USB_GLINT_EN_HOV */
        *dev_ctl = v;
    }

    /* Final register state after fixup */
    {
        volatile uint32_t *pds_usb_ctl  = (volatile uint32_t *)(0x2000e000 + 0x500);
        volatile uint32_t *usb_dev_ctl  = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *usb_phy_tst  = (volatile uint32_t *)(USB_BASE + 0x114);
        volatile uint32_t *usb_glb_int  = (volatile uint32_t *)(USB_BASE + 0x0C4);
        volatile uint32_t *usb_otg_csr  = (volatile uint32_t *)(USB_BASE + 0x080);

        LOG_INF("[USB-INIT] POST-fixup final state:\n");
        LOG_INF("[USB-INIT]   PDS_USB_CTL=0x%08lx\n", (unsigned long)*pds_usb_ctl);
        LOG_INF("[USB-INIT]   DEV_CTL=0x%08lx (GLINT_EN=%d CHIP_EN=%d FORCE_FS=%d)\n",
               (unsigned long)*usb_dev_ctl,
               (int)((*usb_dev_ctl >> 2) & 1), (int)((*usb_dev_ctl >> 5) & 1),
               (int)((*usb_dev_ctl >> 9) & 1));
        LOG_INF("[USB-INIT]   PHY_TST=0x%08lx (UNPLUG=%d) — expect 0\n",
               (unsigned long)*usb_phy_tst, (int)(*usb_phy_tst & 1));
        LOG_INF("[USB-INIT]   GLB_INT=0x%08lx OTG_CSR=0x%08lx\n",
               (unsigned long)*usb_glb_int, (unsigned long)*usb_otg_csr);
    }

    LOG_INF("[USB-INIT] === USB Ready. Waiting for host to send RESET ===\n");
#if USB_NATIVE_TYPEC
    LOG_INF("[USB-INIT] USB Type-C native — no wiring needed\n");
#else
    LOG_INF("[USB-INIT] If no [USB-EVT] RESET appears, check wiring:\n");
    LOG_INF("[USB-INIT]   Kit Pin 37 (USB_DM) -> cable D- (white)\n");
    LOG_INF("[USB-INIT]   Kit Pin 38 (USB_DP) -> cable D+ (green)\n");
    LOG_INF("[USB-INIT]   Kit Pin 21/22 (GND) -> cable GND (black)\n");
#endif
    return 0;
}

void usb_gamepad_set_suspend_hooks(void (*on_suspend)(void),
                                   void (*on_resume)(void),
                                   void (*on_configured)(void))
{
    hook_suspend    = on_suspend;
    hook_resume     = on_resume;
    hook_configured = on_configured;
}

void usb_gamepad_set_polling_rate(uint8_t mode)
{
    uint8_t interval;
#ifdef FORCE_FS_MODE
    /* FS: bInterval = N ms (linear), USB polls 2x target for headroom */
    switch (mode) {
    case 0:  interval = 2; break;  /* FS 2ms = 500Hz USB → ~250Hz effective */
    case 1:  interval = 1; break;  /* FS 1ms = 1000Hz USB → ~500Hz effective */
    default: interval = 1; break;  /* FS 1ms = 1000Hz USB (BT-limited ~750Hz) */
    }
#else
    /* HS: bInterval = 2^(N-1) microframes */
    switch (mode) {
    case 0:  interval = 5; break;  /* HS 2^4=16µf = 2ms → 500Hz, effective ~250Hz */
    case 1:  interval = 4; break;  /* HS 2^3=8µf  = 1ms → 1000Hz, effective ~500Hz */
    default: interval = 3; break;  /* HS 2^2=4µf  = 0.5ms → 2000Hz, effective ~750Hz */
    }
#endif
    config_desc[BINTERVAL_GAMEPAD_EP_IN_OFF]  = interval;
    config_desc[BINTERVAL_GAMEPAD_EP_OUT_OFF] = interval;
    LOG_INF("[USB] Polling rate: mode=%d bInterval=%d\n", mode, interval);
}

ATTR_TCM_SECTION
int usb_gamepad_send_raw_input(const uint8_t *payload)
{
    memcpy((void *)pending_payload, payload, DS5_USB_INPUT_PAYLOAD_LEN);
    pending_active = true;
    try_send_pending();
    return 0;
}

bool usb_gamepad_is_ready(void)
{
    return usb_configured && !ep_in_busy;
}


int usb_gamepad_send_kbd_report(const uint8_t *report, uint8_t len)
{
    if (!usb_configured || !kbd_registered || kbd_ep_busy)
        return -1;
    kbd_buf[0] = 0x01; /* Report ID 1 = Keyboard */
    uint8_t copy = (len > 8) ? 8 : len;
    memcpy(kbd_buf + 1, report, copy);
    if (copy < 8)
        memset(kbd_buf + 1 + copy, 0, 8 - copy);
    kbd_ep_busy = true;
    kbd_ep_busy_since_us = bflb_mtimer_get_time_us();
    int ret = usbd_ep_start_write(0, USB_KBD_EP_IN, kbd_buf, 9);
    if (ret < 0)
        kbd_ep_busy = false;
    return ret;
}

int usb_gamepad_send_consumer_report(uint16_t bits)
{
    if (!usb_configured || !kbd_registered || kbd_ep_busy)
        return -1;
    kbd_buf[0] = 0x02; /* Report ID 2 = Consumer Control */
    kbd_buf[1] = bits & 0xFF; /* bit0=VolUp, bit1=VolDn, bit2=Mute */
    kbd_ep_busy = true;
    kbd_ep_busy_since_us = bflb_mtimer_get_time_us();
    int ret = usbd_ep_start_write(0, USB_KBD_EP_IN, kbd_buf, 2);
    if (ret < 0)
        kbd_ep_busy = false;
    return ret;
}

bool usb_gamepad_kbd_ready(void)
{
    /* Timeout: if kbd_ep_busy has been set for >20ms without completion,
     * the host is not reading the endpoint. Force-clear to keep reporting. */
    if (kbd_ep_busy && kbd_ep_busy_since_us &&
        (bflb_mtimer_get_time_us() - kbd_ep_busy_since_us) > 20000ULL) {
        kbd_ep_busy = false;
        kbd_ep_busy_since_us = 0;
    }
    return usb_configured && kbd_registered && !kbd_ep_busy;
}

/* ---------- HID class request callbacks (Feature Report forwarding) ---------- */

/* +1 for Report ID prefix required by CherryUSB */
static uint8_t feature_resp_buf[FEATURE_DATA_MAX + 1];
static uint8_t kbd_idle_report[USB_KBD_EP_MPS];

static bool is_dongle_cmd(uint8_t report_id)
{
    return (report_id >= 0xF6 && report_id <= 0xF9) || report_id == 0xFB;
}

/*
 * Static fallback for DualSense feature reports 0x09/0x20/0x05.
 * Used when BT cache is empty (controller not yet connected) so that
 * the Linux hid-playstation driver can probe successfully.
 * Returns true if a fallback was provided.
 */
static bool ds5_feature_fallback(uint8_t report_id, uint8_t *buf, uint32_t *len)
{
    switch (report_id) {

    case DS5_FEATURE_REPORT_PAIRING: {
        /* Report 0x09 — Pairing info (20 bytes)
         * bytes[0]   = report ID
         * bytes[1:6] = controller BT MAC (placeholder)
         * bytes[7:19]= reserved */
        memset(buf, 0, 20);
        buf[0] = 0x09;
        buf[1] = 0x00; buf[2] = 0x05; buf[3] = 0xD5;
        buf[4] = 0xBE; buf[5] = 0x18; buf[6] = 0x61;
        *len = 20;
        return true;
    }

    case DS5_FEATURE_REPORT_FIRMWARE: {
        /* Report 0x20 — Firmware info (64 bytes)
         * bytes[0]     = report ID
         * bytes[24:27] = hw_version  (uint32 LE)
         * bytes[28:31] = fw_version  (uint32 LE)
         * bytes[44:45] = update_version (uint16 LE, >= 0x0215 for vibration_v2) */
        memset(buf, 0, 64);
        buf[0] = 0x20;
        buf[24] = 0x00; buf[25] = 0x02; buf[26] = 0x00; buf[27] = 0x00;
        buf[28] = 0x56; buf[29] = 0x02; buf[30] = 0x00; buf[31] = 0x00;
        buf[44] = 0x21; buf[45] = 0x02;
        *len = 64;
        return true;
    }

    case DS5_FEATURE_REPORT_CALIBRATION: {
        /* Report 0x05 — Calibration data (41 bytes)
         * Typical DualSense factory values; the kernel driver has
         * division-by-zero protection and will use its own defaults
         * if any denominator is zero. */
        memset(buf, 0, 41);
        buf[0] = 0x05;
        int16_t *cal = (int16_t *)(buf + 1);
        cal[0]  = 0;       /* gyro_pitch_bias */
        cal[1]  = 0;       /* gyro_yaw_bias   */
        cal[2]  = 0;       /* gyro_roll_bias  */
        cal[3]  = 8192;    /* gyro_pitch_plus  */
        cal[4]  = -8192;   /* gyro_pitch_minus */
        cal[5]  = 8192;    /* gyro_yaw_plus    */
        cal[6]  = -8192;   /* gyro_yaw_minus   */
        cal[7]  = 8192;    /* gyro_roll_plus   */
        cal[8]  = -8192;   /* gyro_roll_minus  */
        cal[9]  = 1024;    /* gyro_speed_plus  */
        cal[10] = -1024;   /* gyro_speed_minus */
        cal[11] = 8192;    /* acc_x_plus   */
        cal[12] = -8192;   /* acc_x_minus  */
        cal[13] = 8192;    /* acc_y_plus   */
        cal[14] = -8192;   /* acc_y_minus  */
        cal[15] = 8192;    /* acc_z_plus   */
        cal[16] = -8192;   /* acc_z_minus  */
        *len = 41;
        return true;
    }

    default:
        if (report_id >= 0x70 && report_id <= 0x7B) {
            /* DSE profile reports — return factory defaults (zeroed)
             * while BT-side prefetch is still in progress. */
            memset(buf, 0, 64);
            buf[0] = report_id;
            *len = 64;
            return true;
        }
        return false;
    }
}

ATTR_TCM_SECTION
void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;

    if (intf == USB_INTF_KBD) {
        memset(kbd_idle_report, 0, sizeof(kbd_idle_report));
        *data = kbd_idle_report;
        *len  = (report_id == 0x02) ? 1 : 8; /* CC=1B bitmap, Kbd=8B (no Report ID prefix) */
        return;
    }

    if (report_type != 0x03) {
        *len = 0;
        return;
    }

    /* Dongle config reports 0xF6-0xF9 */
    if (is_dongle_cmd(report_id)) {
        feature_resp_buf[0] = report_id;

        if (report_id == 0xF7) {
            struct config_body *cfg = config_get();
            uint16_t cfg_len = sizeof(struct config_body);
            if (cfg_len > FEATURE_DATA_MAX)
                cfg_len = FEATURE_DATA_MAX;
            memcpy(feature_resp_buf + 1, cfg, cfg_len);
            *data = feature_resp_buf;
            *len  = 1 + cfg_len;
            LOG_INF("[USB] GET_REPORT(0xF7) → config %u bytes\n", cfg_len);
        } else if (report_id == 0xF8) {
            uint16_t ver_len = strlen(FIRMWARE_VERSION);
            if (ver_len > FEATURE_DATA_MAX)
                ver_len = FEATURE_DATA_MAX;
            memcpy(feature_resp_buf + 1, FIRMWARE_VERSION, ver_len);
            *data = feature_resp_buf;
            *len  = 1 + ver_len;
            LOG_INF("[USB] GET_REPORT(0xF8) → firmware version\n");
        } else if (report_id == 0xF9) {
            extern int8_t bt_hid_host_get_cached_rssi(void);
            extern uint8_t get_battery_level(void);
            extern uint8_t get_battery_state(void);

            feature_resp_buf[1] = (uint8_t)bt_hid_host_get_cached_rssi();
            uint8_t aflags = 0x80;
            if (state_mgr_is_spk_active())        aflags |= 0x02;
            if (usb_audio_mic_is_active())         aflags |= 0x01;
            feature_resp_buf[2] = aflags;
            feature_resp_buf[3] = get_battery_level();
            feature_resp_buf[4] = get_battery_state();
            *data = feature_resp_buf;
            *len  = 5;
        } else if (report_id == 0xFB) {
            feature_resp_buf[0] = 0xFB;
            feature_resp_buf[1] = remap_get_active_profile();
            feature_resp_buf[2] = remap_read_profile;
            memcpy(feature_resp_buf + 3,
                   remap_get_profile_table(remap_read_profile),
                   REMAP_BTN_COUNT * sizeof(remap_entry_t));
            *data = feature_resp_buf;
            *len  = 3 + REMAP_BTN_COUNT * (int)sizeof(remap_entry_t);
            LOG_INF("[USB] GET_REPORT(0xFB) → profile %d (%d bytes)\n",
                    remap_read_profile, *len);
        } else {
            *len = 0;
        }
        return;
    }

    /* DSE profile reports (0x70-0x7B): NAK while prefetch in progress
     * so the PS Accessories app retries instead of caching empty data. */
    if (dse_is_profile_report(report_id) && !dse_profiles_ready()) {
        *len = 0;
        return;
    }

    /* DualSense Feature Reports — forwarded to/from BT */
    const uint8_t *cached;
    uint16_t cached_len;
    if (bt_hid_host_get_cached_feature(report_id, &cached, &cached_len)) {
        if (cached_len > FEATURE_DATA_MAX)
            cached_len = FEATURE_DATA_MAX;
        feature_resp_buf[0] = report_id;
        memcpy(feature_resp_buf + 1, cached, cached_len);
        *data = feature_resp_buf;
        *len  = 1 + cached_len;
        LOG_INF("[USB] GET_REPORT(Feature 0x%02x) → %u bytes from cache\n",
               report_id, cached_len);
    } else if (ds5_feature_fallback(report_id, feature_resp_buf, len)) {
        *data = feature_resp_buf;
        LOG_INF("[USB] GET_REPORT(Feature 0x%02x) → %u bytes from fallback\n",
               report_id, *len);
    } else {
        pending_feature_rid = report_id;
        *len = 0;
        LOG_INF("[USB] GET_REPORT(Feature 0x%02x) → not cached, deferred to task\n",
               report_id);
    }
}

ATTR_TCM_SECTION
void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report,
                         uint32_t report_len)
{
    (void)busid;

    if (intf == USB_INTF_KBD) {
        return;
    }

    const uint8_t *payload = report;
    uint32_t payload_len = report_len;
    if (report_len > 0 && report[0] == report_id) {
        payload++;
        payload_len--;
    }

    /* Dongle config command (0xF6 SET) */
    if (report_type == 0x03 && report_id == 0xF6 && payload_len > 0) {
        uint8_t cmd = payload[0];
        if (cmd == 0x01 && payload_len > 1) {
            config_set(payload + 1, payload_len - 1);
            usb_gamepad_set_polling_rate(config_get()->polling_rate_mode);
            if (config_get()->lock_volume)
                state_mgr_restore_config_volume();
            LOG_INF("[USB] CMD 0x01: config updated\n");
        } else if (cmd == 0x02) {
            usb_config_save_pending = true;
        } else if (cmd == 0x03) {
            usb_reset_pending = true;
        }
        return;
    }

    /* Button remap command (0xFB SET) */
    if (report_type == 0x03 && report_id == 0xFB && payload_len > 0) {
        uint8_t cmd = payload[0];
        if (cmd == 0x01) {
            /* 0x01 [profile] [data...] — set remap for profile */
            if (payload_len >= 2 + REMAP_BTN_COUNT * sizeof(remap_entry_t)) {
                uint8_t prof = payload[1];
                if (prof >= REMAP_PROFILE_COUNT) prof = 0;
                remap_set_profile(prof, payload + 2, (uint8_t)(payload_len - 2));
                usb_remap_save_profile = prof;
                usb_remap_save_pending = true;
                LOG_INF("[USB] CMD 0xFB/0x01: profile %d updated\n", prof);
            } else if (payload_len >= 1 + REMAP_BTN_COUNT * sizeof(remap_entry_t)) {
                /* backward compat: no profile byte → profile 0 */
                remap_set_profile(0, payload + 1, (uint8_t)(payload_len - 1));
                usb_remap_save_profile = 0;
                usb_remap_save_pending = true;
                LOG_INF("[USB] CMD 0xFB/0x01: profile 0 updated (compat)\n");
            }
        } else if (cmd == 0x02) {
            /* 0x02 [profile] — reset profile (optional byte) */
            uint8_t prof = (payload_len >= 2) ? payload[1] : remap_get_active_profile();
            if (prof >= REMAP_PROFILE_COUNT) prof = 0;
            remap_reset_profile(prof);
            usb_remap_save_profile = prof;
            usb_remap_save_pending = true;
            LOG_INF("[USB] CMD 0xFB/0x02: profile %d reset\n", prof);
        } else if (cmd == 0x03 && payload_len >= 2) {
            /* 0x03 [profile] — select profile to read on next GET_REPORT */
            remap_read_profile = payload[1] < REMAP_PROFILE_COUNT ? payload[1] : 0;
            LOG_INF("[USB] CMD 0xFB/0x03: read_profile → %d\n", remap_read_profile);
        } else if (cmd == 0x05 && payload_len >= 2) {
            /* 0x05 [profile] — switch active profile */
            uint8_t prof = payload[1];
            if (prof < REMAP_PROFILE_COUNT) {
                remap_switch_profile(prof);
                LOG_INF("[USB] CMD 0xFB/0x05: active profile → %d\n", prof);
            }
        }
        return;
    }

    /* DSE profile writes (0x60-0x62) and unlock (0x80) — forward + notify */
    if (report_type == 0x03 && payload_len > 0 &&
        (report_id == 0x60 || report_id == 0x61 ||
         report_id == 0x62 || report_id == 0x80)) {
        set_report_entry_t entry;
        entry.report_id = report_id;
        entry.is_dse    = true;
        entry.len = (payload_len <= SET_REPORT_MAX_DATA)
                        ? payload_len : SET_REPORT_MAX_DATA;
        memcpy(entry.data, payload, entry.len);
        BaseType_t ok = xQueueSendFromISR(set_report_queue, &entry, NULL);
        LOG_ISR("[USB-ISR] SET_REPORT(DSE 0x%02x) queued=%d len=%lu\n",
               report_id, (int)(ok == pdTRUE), (unsigned long)payload_len);
        dse_on_profile_write(report_id);
        return;
    }

    /* DS5Dongle only forwards DSE profile reports (0x60-0x62, 0x80) to BT.
     * All other Feature SET_REPORTs (0x08, 0x09 etc.) are dropped — forwarding
     * 0x08 (BT control) would cause the controller to power-off or re-pair. */
    if (report_type == 0x02 && payload_len > 0) {
        if (output_callback)
            output_callback(report, report_len);
    }
}

/* ---- USB soft plug/unplug via PHY UNPLUG bit ---- */

void usb_soft_disconnect(void)
{
    volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
    *phy_tst |= 1u;
    LOG_INF("[USB] Soft disconnect (stealth)\n");
}

void usb_soft_connect(void)
{
    volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
    *phy_tst &= ~1u;
    LOG_INF("[USB] Soft connect (stealth)\n");
}
