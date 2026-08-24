#include "ds5_usb_audio.h"
#include "audio.h"
#include "state_mgr.h"
#include "usbd_core.h"
#include "usbd_audio.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>
#include "debug_log.h"
#include "bflb_mtimer.h"

/* ---- UAC1 entity IDs ---- */
#define AUDIO_IT_SPK_ID     0x01
#define AUDIO_FU_SPK_ID     0x02
#define AUDIO_OT_SPK_ID     0x03
#define AUDIO_IT_MIC_ID     0x04
#define AUDIO_FU_MIC_ID     0x05
#define AUDIO_OT_MIC_ID     0x06

/* ---- Audio descriptor (AC + AS_OUT speaker + AS_IN mic) ----
 * AC_Intf(9)
 * + AC_Header(10) + IT_spk(12) + FU_spk(12) + OT_spk(9) + IT_mic(12) + FU_mic(9) + OT_mic(9)
 * + AS_OUT_Alt0(9) + AS_OUT_Alt1(9) + AS_General(7) + FormatType(11) + EP(9) + CS_EP(7)
 * + AS_IN_Alt0(9) + AS_IN_Alt1(9) + AS_General(7) + FormatType(11) + EP(9) + CS_EP(7)
 * = 186 bytes total
 * No IAD — matches DS5Dongle default and real DualSense dongle. */
#define AUDIO_DESC_SIZE 186

static const uint8_t audio_desc[AUDIO_DESC_SIZE] = {
    /* ---- Audio Control Interface (Interface 0) ---- */
    0x09, 0x04,
    USB_AUDIO_INTF_CTRL,            /* bInterfaceNumber */
    0x00, 0x00,                     /* bAlternateSetting, bNumEndpoints */
    0x01, 0x01, 0x00,               /* Audio, AudioControl, none */
    0x00,                           /* iInterface */

    /* AC Header: bInCollection=2 (speaker + mic) */
    0x0A,                           /* bLength: 8 + 2 */
    0x24, 0x01,                     /* CS_INTERFACE, HEADER */
    0x00, 0x01,                     /* bcdADC: 1.00 */
    0x49, 0x00,                     /* wTotalLength: 73 (10+12+12+9+12+9+9) */
    0x02,                           /* bInCollection: 2 streaming interfaces */
    USB_AUDIO_INTF_STREAM,          /* baInterfaceNr(1): speaker */
    USB_AUDIO_INTF_MIC,             /* baInterfaceNr(2): mic */

    /* ---- Speaker topology (same as before) ---- */

    /* Input Terminal (ID 1): USB Streaming, 4ch */
    0x0C, 0x24, 0x02,
    AUDIO_IT_SPK_ID,                /* bTerminalID: 1 */
    0x01, 0x01,                     /* wTerminalType: USB Streaming */
    AUDIO_OT_MIC_ID,                /* bAssocTerminal: 6 (paired with USB OUT) */
    0x04,                           /* bNrChannels: 4 */
    0x33, 0x00,                     /* wChannelConfig: FL+FR+SL+SR */
    0x00, 0x00,                     /* iChannelNames, iTerminal */

    /* Feature Unit (ID 2): Master mute+volume */
    0x0C, 0x24, 0x06,
    AUDIO_FU_SPK_ID,                /* bUnitID: 2 */
    AUDIO_IT_SPK_ID,                /* bSourceID: 1 */
    0x01,                           /* bControlSize: 1 byte */
    0x03,                           /* bmaControls[0] master: Mute+Volume */
    0x00, 0x00, 0x00, 0x00,         /* bmaControls[1..4]: no per-ch */
    0x00,                           /* iFeature */

    /* Output Terminal (ID 3): Speaker */
    0x09, 0x24, 0x03,
    AUDIO_OT_SPK_ID,                /* bTerminalID: 3 */
    0x01, 0x03,                     /* wTerminalType: Speaker (0x0301) */
    AUDIO_IT_MIC_ID,                /* bAssocTerminal: 4 (paired with mic input) */
    AUDIO_FU_SPK_ID,                /* bSourceID: 2 */
    0x00,                           /* iTerminal */

    /* ---- Microphone topology ---- */

    /* Input Terminal (ID 4): Headset Mic, 2ch */
    0x0C, 0x24, 0x02,
    AUDIO_IT_MIC_ID,                /* bTerminalID: 4 */
    0x02, 0x04,                     /* wTerminalType: Headset (0x0402) */
    AUDIO_OT_SPK_ID,                /* bAssocTerminal: 3 (paired with speaker) */
    0x02,                           /* bNrChannels: 2 (mono dup'd to stereo) */
    0x03, 0x00,                     /* wChannelConfig: FL+FR */
    0x00, 0x00,                     /* iChannelNames, iTerminal */

    /* Feature Unit (ID 5): Master mute+volume, 2ch */
    0x09, 0x24, 0x06,
    AUDIO_FU_MIC_ID,                /* bUnitID: 5 */
    AUDIO_IT_MIC_ID,                /* bSourceID: 4 */
    0x01,                           /* bControlSize: 1 byte */
    0x03,                           /* bmaControls[0] master: Mute+Volume */
    0x00,                           /* bmaControls[1] ch1: none */
    0x00,                           /* iFeature */

    /* Output Terminal (ID 6): USB Streaming */
    0x09, 0x24, 0x03,
    AUDIO_OT_MIC_ID,                /* bTerminalID: 6 */
    0x01, 0x01,                     /* wTerminalType: USB Streaming (0x0101) */
    AUDIO_IT_SPK_ID,                /* bAssocTerminal: 1 (paired with USB IN) */
    AUDIO_FU_MIC_ID,                /* bSourceID: 5 */
    0x00,                           /* iTerminal */

    /* ---- Audio Streaming OUT Interface (Interface 1: Speaker) ---- */
    /* Alt 0: idle */
    0x09, 0x04,
    USB_AUDIO_INTF_STREAM,
    0x00, 0x00,                     /* bAlternateSetting, bNumEndpoints */
    0x01, 0x02, 0x00, 0x00,

    /* Alt 1: active (1 ISO OUT endpoint) */
    0x09, 0x04,
    USB_AUDIO_INTF_STREAM,
    0x01,                           /* bAlternateSetting: 1 */
    0x01,                           /* bNumEndpoints: 1 */
    0x01, 0x02, 0x00, 0x00,

    /* AS General */
    0x07, 0x24, 0x01,
    AUDIO_IT_SPK_ID,                /* bTerminalLink: 1 */
    0x01,                           /* bDelay: 1 frame */
    0x01, 0x00,                     /* wFormatTag: PCM */

    /* Format Type I: 4ch, 16-bit, 48kHz */
    0x0B, 0x24, 0x02,
    0x01, 0x04, 0x02, 0x10,
    0x01, 0x80, 0xBB, 0x00,         /* 48000 Hz */

    /* ISO OUT Endpoint */
    0x09, 0x05,
    USB_AUDIO_EP_OUT,
    0x09,                           /* bmAttributes: Isochronous, Adaptive */
    (USB_AUDIO_OUT_MPS & 0xFF),
    (USB_AUDIO_OUT_MPS >> 8),
    0x04,                           /* bInterval: HS 2^(4-1)=8 µf = 1ms */
    0x00, 0x00,

    /* CS Endpoint: General */
    0x07, 0x25, 0x01,
    0x00, 0x00, 0x00, 0x00,

    /* ---- Audio Streaming IN Interface (Interface 2: Mic) ---- */
    /* Alt 0: idle */
    0x09, 0x04,
    USB_AUDIO_INTF_MIC,
    0x00, 0x00,                     /* bAlternateSetting, bNumEndpoints */
    0x01, 0x02, 0x00, 0x00,

    /* Alt 1: active (1 ISO IN endpoint) */
    0x09, 0x04,
    USB_AUDIO_INTF_MIC,
    0x01,                           /* bAlternateSetting: 1 */
    0x01,                           /* bNumEndpoints: 1 */
    0x01, 0x02, 0x00, 0x00,

    /* AS General */
    0x07, 0x24, 0x01,
    AUDIO_OT_MIC_ID,                /* bTerminalLink: 6 (OT → USB Streaming) */
    0x01,                           /* bDelay: 1 frame */
    0x01, 0x00,                     /* wFormatTag: PCM */

    /* Format Type I: 2ch, 16-bit, 48kHz */
    0x0B, 0x24, 0x02,
    0x01, 0x02, 0x02, 0x10,
    0x01, 0x80, 0xBB, 0x00,         /* 48000 Hz */

    /* ISO IN Endpoint */
    0x09, 0x05,
    USB_AUDIO_MIC_EP_IN,
    0x05,                           /* bmAttributes: Isochronous, Asynchronous */
    (USB_AUDIO_MIC_MPS & 0xFF),
    (USB_AUDIO_MIC_MPS >> 8),
    0x04,                           /* bInterval: HS 2^(4-1)=8 µf = 1ms */
    0x00, 0x00,

    /* CS Endpoint: General */
    0x07, 0x25, 0x01,
    0x00, 0x00, 0x00, 0x00,
};

/* ---- Double buffer for ISO PCM accumulation ---- */
#define PCM_BUF_SAMPLES  USB_AUDIO_ACCUM_SAMPLES
#define PCM_BUF_BYTES    (PCM_BUF_SAMPLES * USB_AUDIO_CHANNELS * sizeof(int16_t))

static int16_t pcm_buf[2][PCM_BUF_SAMPLES * USB_AUDIO_CHANNELS];
static volatile uint32_t pcm_write_pos = 0;
static volatile uint8_t  pcm_write_idx = 0;
static volatile bool     pcm_ready = false;
static uint8_t           pcm_read_idx = 0;

static SemaphoreHandle_t audio_sem;
static StaticSemaphore_t audio_sem_buf;

static volatile bool     stream_active = false;
static volatile uint64_t audio_last_active_us = 0;

static uint8_t USB_NOCACHE_RAM_SECTION iso_rx_buf[USB_AUDIO_OUT_MPS];

/* ---- Entity table for CherryUSB audio class ---- */
static struct audio_entity_info entity_table[] = {
    { .bEntityId = AUDIO_FU_SPK_ID,
      .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
      .ep = USB_AUDIO_EP_OUT },
    { .bEntityId = AUDIO_FU_MIC_ID,
      .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
      .ep = USB_AUDIO_MIC_EP_IN },
};

static struct usbd_interface audio_intf_ctrl;
static struct usbd_interface audio_intf_stream;
static struct usbd_interface audio_intf_mic;
static struct usbd_endpoint  audio_out_ep;
static struct usbd_endpoint  audio_mic_ep;

/* ---- Mic ring buffer ---- */
#define MIC_RING_SIZE   USB_AUDIO_MIC_RING_SAMPLES
static int16_t mic_ring[MIC_RING_SIZE * USB_AUDIO_MIC_CHANNELS];
static volatile uint32_t mic_ring_wr = 0;
static volatile uint32_t mic_ring_rd = 0;
static volatile bool     mic_active  = false;

static uint8_t USB_NOCACHE_RAM_SECTION iso_mic_tx_buf[USB_AUDIO_MIC_MPS];

/* ---- ISO OUT endpoint callback (ISR context) ---- */
static void audio_ep_out_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    if (nbytes == 0 || !stream_active) {
        if (stream_active) {
            usbd_ep_start_read(busid, USB_AUDIO_EP_OUT, iso_rx_buf, sizeof(iso_rx_buf));
        }
        return;
    }

    audio_last_active_us = bflb_mtimer_get_time_us();
    uint32_t samples = nbytes / (USB_AUDIO_CHANNELS * sizeof(int16_t));
    const int16_t *src = (const int16_t *)iso_rx_buf;
    uint32_t total = samples * USB_AUDIO_CHANNELS;
    uint32_t consumed = 0;

    while (consumed < total) {
        int16_t *dst = pcm_buf[pcm_write_idx];
        uint32_t pos = pcm_write_pos;
        uint32_t space = (PCM_BUF_SAMPLES * USB_AUDIO_CHANNELS) - pos;
        uint32_t chunk = total - consumed;
        if (chunk > space) chunk = space;

        memcpy(&dst[pos], &src[consumed], chunk * sizeof(int16_t));
        pos += chunk;
        consumed += chunk;

        if (pos >= PCM_BUF_SAMPLES * USB_AUDIO_CHANNELS) {
            pcm_write_pos = 0;
            pcm_write_idx ^= 1;
            pcm_ready = true;
            BaseType_t woken = pdFALSE;
            xSemaphoreGiveFromISR(audio_sem, &woken);
            portYIELD_FROM_ISR(woken);
        } else {
            pcm_write_pos = pos;
        }
    }

    usbd_ep_start_read(busid, USB_AUDIO_EP_OUT, iso_rx_buf, sizeof(iso_rx_buf));
}

/* ---- Mic EP IN: feed next packet from ring buffer ---- */
static void mic_send_next(uint8_t busid)
{
    uint32_t rd = mic_ring_rd;
    uint32_t wr = mic_ring_wr;
    uint32_t avail = (wr >= rd) ? (wr - rd) : (MIC_RING_SIZE - rd + wr);
    uint32_t samples_per_pkt = 48;  /* 48 stereo pairs per 1ms frame */
    int16_t *tx = (int16_t *)iso_mic_tx_buf;
    uint32_t to_send = (avail >= samples_per_pkt) ? samples_per_pkt : avail;

    for (uint32_t i = 0; i < to_send; i++) {
        uint32_t idx = ((rd + i) % MIC_RING_SIZE) * USB_AUDIO_MIC_CHANNELS;
        tx[i * 2]     = mic_ring[idx];
        tx[i * 2 + 1] = mic_ring[idx + 1];
    }
    for (uint32_t i = to_send; i < samples_per_pkt; i++) {
        tx[i * 2]     = 0;
        tx[i * 2 + 1] = 0;
    }
    mic_ring_rd = (rd + to_send) % MIC_RING_SIZE;

    int ret = usbd_ep_start_write(busid, USB_AUDIO_MIC_EP_IN,
                                  iso_mic_tx_buf, samples_per_pkt * 2 * sizeof(int16_t));
    if (ret < 0)
        LOG_ERR("[AUDIO] mic EP write failed: %d\n", ret);
}

static void audio_mic_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep; (void)nbytes;
    if (mic_active)
        mic_send_next(busid);
}

/* ---- CherryUSB weak callback overrides ---- */

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    if (intf == USB_AUDIO_INTF_STREAM) {
        LOG_INF("[AUDIO] Speaker stream opened\n");
        stream_active = true;
        pcm_write_pos = 0;
        pcm_write_idx = 0;
        pcm_ready = false;
        state_mgr_set_spk_active(true);
        int r = usbd_ep_start_read(busid, USB_AUDIO_EP_OUT, iso_rx_buf, sizeof(iso_rx_buf));
        if (r < 0)
            LOG_ERR("[AUDIO] initial ep_start_read fail=%d\n", r);
    } else if (intf == USB_AUDIO_INTF_MIC) {
        LOG_INF("[AUDIO] Mic stream opened\n");
        mic_active = true;
        mic_ring_wr = 0;
        mic_ring_rd = 0;
        audio_set_mic_active(true);
        mic_send_next(busid);
    }
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    if (intf == USB_AUDIO_INTF_STREAM) {
        usbd_ep_close(busid, USB_AUDIO_EP_OUT);
        LOG_INF("[AUDIO] Speaker stream closed\n");
        stream_active = false;
        audio_reset_encoder();
        state_mgr_set_spk_active(false);
    } else if (intf == USB_AUDIO_INTF_MIC) {
        usbd_ep_close(busid, USB_AUDIO_MIC_EP_IN);
        LOG_INF("[AUDIO] Mic stream closed\n");
        mic_active = false;
        audio_set_mic_active(false);
    }
}

void usbd_audio_set_volume(uint8_t busid, uint8_t ep, uint8_t ch, int volume_db)
{
    (void)busid; (void)ch;
    if (ep == USB_AUDIO_MIC_EP_IN) {
        LOG_INF("[AUDIO] Mic volume %d dB (ignored)\n", volume_db);
        return;
    }
    /* Map Windows dB range [-100, 0] to DualSense [0, 127].
     * Previous mapping (vol = volume_db + 100) capped at 100/127 = 79%.
     * Correct mapping: -100dB → 0, 0dB → 127. */
    int vol = (int)((float)(volume_db + 100) * 127.0f / 100.0f + 0.5f);
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    state_mgr_set_volume((uint8_t)vol, (uint8_t)vol);
}

void usbd_audio_set_mute(uint8_t busid, uint8_t ep, uint8_t ch, bool mute)
{
    (void)busid; (void)ch;
    if (ep == USB_AUDIO_MIC_EP_IN) {
        LOG_INF("[AUDIO] Mic mute %d (ignored)\n", mute);
        return;
    }
    state_mgr_set_mute(mute);
    LOG_INF("[AUDIO] Mute = %d\n", mute);
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep)
{
    (void)busid; (void)ep;
    return USB_AUDIO_SAMPLE_RATE;
}

/* ---- Public API ---- */

void usb_audio_early_init(void)
{
    if (!audio_sem)
        audio_sem = xSemaphoreCreateCountingStatic(2, 0, &audio_sem_buf);
}

void usb_audio_register(uint8_t busid)
{
    usb_audio_early_init();

    usbd_add_interface(busid, usbd_audio_init_intf(
        busid, &audio_intf_ctrl, 0x0100, entity_table,
        sizeof(entity_table) / sizeof(entity_table[0])));
    usbd_add_interface(busid, usbd_audio_init_intf(
        busid, &audio_intf_stream, 0x0100, entity_table,
        sizeof(entity_table) / sizeof(entity_table[0])));
    usbd_add_interface(busid, usbd_audio_init_intf(
        busid, &audio_intf_mic, 0x0100, entity_table,
        sizeof(entity_table) / sizeof(entity_table[0])));

    audio_out_ep.ep_addr = USB_AUDIO_EP_OUT;
    audio_out_ep.ep_cb = audio_ep_out_handler;
    usbd_add_endpoint(busid, &audio_out_ep);

    audio_mic_ep.ep_addr = USB_AUDIO_MIC_EP_IN;
    audio_mic_ep.ep_cb = audio_mic_ep_in_handler;
    usbd_add_endpoint(busid, &audio_mic_ep);

    LOG_INF("[AUDIO] UAC1 interfaces registered (spk+mic)\n");
}

const uint8_t *usb_audio_get_desc(uint16_t *len)
{
    if (len) *len = AUDIO_DESC_SIZE;
    return audio_desc;
}

bool usb_audio_is_active(void)
{
    if (stream_active)
        return true;
    if (audio_last_active_us == 0)
        return false;
    uint64_t elapsed = bflb_mtimer_get_time_us() - audio_last_active_us;
    return elapsed < 5000000ULL;
}

void usb_audio_stop(void)
{
    if (stream_active)
        LOG_INF("[AUDIO] usb_audio_stop\n");
    stream_active = false;
    pcm_write_pos = 0;
    pcm_write_idx = 0;
    pcm_ready = false;
    state_mgr_set_spk_active(false);
}

bool usb_audio_read(int16_t *out)
{
    if (!pcm_ready)
        return false;

    pcm_read_idx = pcm_write_idx ^ 1;
    memcpy(out, pcm_buf[pcm_read_idx], PCM_BUF_BYTES);
    pcm_ready = false;
    return true;
}

void *usb_audio_get_semaphore(void)
{
    return (void *)audio_sem;
}

/* ---- Mic ring buffer write (called from audio_task) ---- */
void usb_audio_mic_write(const int16_t *samples, uint32_t count)
{
    if (!mic_active) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t wr = mic_ring_wr;
        uint32_t next = (wr + 1) % MIC_RING_SIZE;
        if (next == mic_ring_rd)
            break;  /* ring full, drop remainder of this frame */
        uint32_t idx = wr * USB_AUDIO_MIC_CHANNELS;
        mic_ring[idx]     = samples[i * 2];
        mic_ring[idx + 1] = samples[i * 2 + 1];
        mic_ring_wr = next;
    }
}

bool usb_audio_mic_is_active(void)
{
    return mic_active;
}

void usb_audio_mic_stop(void)
{
    mic_active  = false;
    mic_ring_wr = 0;
    mic_ring_rd = 0;
}
