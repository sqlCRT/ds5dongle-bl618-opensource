#include "audio.h"
#include "ds5_usb_audio.h"
#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "config.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"

#include "opus.h"
#include <string.h>
#include "debug_log.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Constants matching DS5Dongle audio.cpp ---- */
#define OPUS_FRAME_SAMPLES  480     /* 10ms at 48kHz */
#define OPUS_OUT_SIZE       200     /* CBR output size */
#define HAPTIC_BUF_SIZE     64      /* 32 stereo int8 pairs */
#define HAPTIC_DECIMATE     16      /* 48kHz / 3kHz */
#define ACCUM_SAMPLES       USB_AUDIO_ACCUM_SAMPLES  /* 1024 = 2 Opus frames */
/* Half-block size: the 1024-sample block is split into two 512-sample halves,
 * each resampled 512→480 and encoded as one Opus frame. */
#define HALF_ACCUM          (ACCUM_SAMPLES / 2)      /* 512 */

#define MIC_OPUS_SIZE       71      /* Opus encoded mic frame from DualSense */
#define MIC_CHANNELS        1       /* Controller sends mono mic */
#define MIC_QUEUE_DEPTH     4

/* Polyphase sinc resampler: 512→480 = 16:15 ratio
 * Matches DS5Dongle's WDL sinc resampler for anti-alias filtering.
 * gcd(512,480) = 32 → 15 unique phases, 8 taps per phase. */
#define SINC_HALF_TAPS  4
#define SINC_TAPS       (2 * SINC_HALF_TAPS)
#define RESAMP_PHASES   15

static uint8_t audio_seq;   /* sequence counter for 0x39 audio report */

/* Static buffers for Opus encoder/decoder — avoids 20+KB heap allocation.
 * Upper bounds verified at init via opus_encoder_get_size() / opus_decoder_get_size(). */
#define OPUS_ENC_MAX_SIZE 36864
#define OPUS_DEC_MAX_SIZE 24576
static __attribute__((aligned(8))) uint8_t encoder_mem[OPUS_ENC_MAX_SIZE];
static __attribute__((aligned(8))) uint8_t decoder_mem[OPUS_DEC_MAX_SIZE];
static OpusEncoder  *encoder;
static OpusDecoder  *decoder;
static uint8_t  packet_counter;
static volatile bool plug_headset;
static volatile bool mic_enabled;   /* host opened mic interface AND config allows */
static volatile bool mic_status_pending;  /* deferred: send 0x32 to controller */

static QueueHandle_t mic_queue;

static int16_t  pcm_block[ACCUM_SAMPLES * USB_AUDIO_CHANNELS];

static int16_t  spk_resamp[OPUS_FRAME_SAMPLES * 2];
static bool     mic_first_frame;
static volatile bool encoding_in_progress;
static volatile bool encoder_reset_pending;

/* Double-frame buffers for 0x39 report (2x haptics + 2x opus per packet) */
static uint8_t  opus_slots[2][OPUS_OUT_SIZE];
static int8_t   haptic_slots[2][HAPTIC_BUF_SIZE];

/* Pre-computed polyphase sinc filter: [phase][tap] */
static float sinc_coeff[RESAMP_PHASES][SINC_TAPS];

static void resamp_sinc_init(void)
{
    const float cutoff = 480.0f / 512.0f; /* anti-alias at output Nyquist */

    for (int p = 0; p < RESAMP_PHASES; p++) {
        float frac = (float)p / (float)RESAMP_PHASES;
        float sum = 0.0f;

        for (int t = 0; t < SINC_TAPS; t++) {
            float x = (float)(t - (SINC_HALF_TAPS - 1)) - frac;

            /* sinc(x * cutoff) * cutoff */
            float sx = x * cutoff;
            float s;
            if (fabsf(sx) < 1e-6f)
                s = cutoff;
            else
                s = cutoff * sinf((float)M_PI * sx) / ((float)M_PI * sx);

            /* Hann window over kernel span */
            float wn = ((float)t + 0.5f) / (float)SINC_TAPS;
            float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * wn));

            sinc_coeff[p][t] = s * w;
            sum += sinc_coeff[p][t];
        }

        if (fabsf(sum) > 1e-6f) {
            for (int t = 0; t < SINC_TAPS; t++)
                sinc_coeff[p][t] /= sum;
        }
    }
}

/* ---- Polyphase sinc resample 512 → 480 (stereo int16) ---- */
static void resample_512_480(const int16_t *in, int16_t *out)
{
    for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
        uint32_t src_pos = (uint32_t)i * 16;  /* i * 512/480 scaled by RESAMP_PHASES */
        int center = (int)(src_pos / RESAMP_PHASES);
        int phase  = (int)(src_pos % RESAMP_PHASES);

        const float *c = sinc_coeff[phase];
        float sum_l = 0.0f, sum_r = 0.0f;

        for (int t = 0; t < SINC_TAPS; t++) {
            int idx = center + t - (SINC_HALF_TAPS - 1);
            if (idx < 0) idx = 0;
            if (idx >= HALF_ACCUM) idx = HALF_ACCUM - 1;

            sum_l += (float)in[idx * 2]     * c[t];
            sum_r += (float)in[idx * 2 + 1] * c[t];
        }

        int32_t l = (int32_t)(sum_l + (sum_l >= 0 ? 0.5f : -0.5f));
        int32_t r = (int32_t)(sum_r + (sum_r >= 0 ? 0.5f : -0.5f));
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;

        out[i * 2]     = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}

/* ---- Haptics: 16:1 point-sample decimation (stereo int16 → stereo int8) ----
 * No low-pass filter — matches wired DS5 behavior (internal haptics also
 * run at 3 kHz without filtering). */
static void decimate_haptics(const int16_t *in, int8_t *out, uint32_t in_samples)
{
    uint32_t out_pairs = in_samples / HAPTIC_DECIMATE;
    if (out_pairs > HAPTIC_BUF_SIZE / 2)
        out_pairs = HAPTIC_BUF_SIZE / 2;

    /* haptics_gain [1.0,2.0] → fixed-point 8.8: 256..512 */
    float gain_f = config_get()->haptics_gain;
    if (gain_f < 1.0f) gain_f = 1.0f;
    if (gain_f > 2.0f) gain_f = 2.0f;
    int32_t gain_fp = (int32_t)(gain_f * 256.0f);

    for (uint32_t i = 0; i < out_pairs; i++) {
        uint32_t idx = (i * HAPTIC_DECIMATE) * 2;
        int32_t val_l = in[idx];
        int32_t val_r = in[idx + 1];
        val_l = (val_l * gain_fp) >> 16;
        val_r = (val_r * gain_fp) >> 16;
        if (val_l > 127) val_l = 127;
        if (val_l < -128) val_l = -128;
        if (val_r > 127) val_r = 127;
        if (val_r < -128) val_r = -128;
        out[i * 2]     = (int8_t)val_l;
        out[i * 2 + 1] = (int8_t)val_r;
    }
}

/* ---- Build and send BT report 0x39 (547 bytes, double-frame) ---- */
static void send_audio_report(void)
{
    static uint8_t pkt[DS5_BT_AUDIO_REPORT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = DS5_BT_AUDIO_REPORT_ID;
    pkt[1] = (audio_seq & 0x0F) << 4;
    audio_seq = (audio_seq + 1) & 0x0F;

    /* Audio control header (tag 0x91, 6 fields) */
    pkt[2] = DS5_AUDIO_TAG_HEADER;
    pkt[3] = 6;
    pkt[4] = mic_enabled ? 0x7F : 0x7E;

    uint8_t buf_len = config_audio_buf_len();
    pkt[5] = buf_len;
    pkt[6] = buf_len;
    pkt[7] = buf_len;
    pkt[8] = buf_len;
    packet_counter += 2;
    pkt[9] = packet_counter;

    /* Haptics (tag 0xD2, 2x 64-byte blocks) */
    pkt[10] = DS5_AUDIO_TAG_HAPTICS;
    pkt[11] = DS5_AUDIO_SAMPLE_SIZE;
    memcpy(pkt + 12, haptic_slots[0], HAPTIC_BUF_SIZE);
    memcpy(pkt + 12 + HAPTIC_BUF_SIZE, haptic_slots[1], HAPTIC_BUF_SIZE);

    /* Speaker Opus (tag 0xD3/0xD6, 2x 200-byte blocks) */
    bool speaker_enabled = !config_get()->disable_speaker;
    if (speaker_enabled) {
        pkt[140] = plug_headset ? DS5_AUDIO_TAG_HEADSET : DS5_AUDIO_TAG_SPEAKER;
        pkt[141] = OPUS_OUT_SIZE;
        memcpy(pkt + 142, opus_slots[0], OPUS_OUT_SIZE);
        memcpy(pkt + 142 + OPUS_OUT_SIZE, opus_slots[1], OPUS_OUT_SIZE);
    }

    /* CRC32 */
    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, pkt,
                             DS5_BT_AUDIO_REPORT_SIZE - 4);
    ds5_write_le32(&pkt[DS5_BT_AUDIO_REPORT_SIZE - 4], crc);

    int ret = bt_hid_host_send_output(pkt, DS5_BT_AUDIO_REPORT_SIZE);
    if (ret)
        LOG_ERR("[AUDIO] BT send failed: %d\n", ret);
}

/* ---- Public API ---- */

int audio_init(void)
{
    resamp_sinc_init();

    int err;
    int enc_size = opus_encoder_get_size(2);
    int dec_size = opus_decoder_get_size(MIC_CHANNELS);

    if (enc_size > OPUS_ENC_MAX_SIZE) {
        LOG_ERR("[AUDIO] Opus encoder needs %d bytes, buffer is %d\n",
                enc_size, OPUS_ENC_MAX_SIZE);
        return -1;
    }
    if (dec_size > OPUS_DEC_MAX_SIZE) {
        LOG_ERR("[AUDIO] Opus decoder needs %d bytes, buffer is %d\n",
                dec_size, OPUS_DEC_MAX_SIZE);
        return -1;
    }

    encoder = (OpusEncoder *)encoder_mem;
    err = opus_encoder_init(encoder, 48000, 2,
                            OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Opus encoder init failed: %d\n", err);
        encoder = NULL;
        return -1;
    }

    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

    decoder = (OpusDecoder *)decoder_mem;
    err = opus_decoder_init(decoder, 48000, MIC_CHANNELS);
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Opus decoder init failed: %d\n", err);
        decoder = NULL;
    }

    mic_queue = xQueueCreate(MIC_QUEUE_DEPTH, MIC_OPUS_SIZE);
    if (!mic_queue) {
        LOG_ERR("[AUDIO] mic_queue create failed\n");
    }

    if (!decoder || !mic_queue) {
        LOG_ERR("[AUDIO] Mic path unavailable (decoder=%p queue=%p)\n",
               (void *)decoder, (void *)mic_queue);
    }

    packet_counter = 0;
    plug_headset = false;
    mic_enabled = false;
    mic_status_pending = false;
    mic_first_frame = false;
    memset(opus_slots, 0, sizeof(opus_slots));
    memset(haptic_slots, 0, sizeof(haptic_slots));

    LOG_INF("[AUDIO] Opus static init (enc=%d/%d dec=%d/%d mic_q=%p)\n",
           enc_size, OPUS_ENC_MAX_SIZE, dec_size, OPUS_DEC_MAX_SIZE,
           (void *)mic_queue);
    return 0;
}

/* ---- Send a 0x32 status report to toggle controller mic streaming ---- */
static void send_mic_status(void)
{
    if (bt_hid_host_get_state() != BT_HID_STATE_CONNECTED)
        return;

    static uint8_t mic_seq = 0;
    uint8_t pkt[DS5_BT_OUTPUT_EXT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = DS5_BT_OUTPUT_REPORT_ID_EXT;
    pkt[1] = (mic_seq & 0x0F) << 4;
    mic_seq = (mic_seq + 1) & 0x0F;

    pkt[2] = DS5_AUDIO_TAG_HEADER;
    pkt[3] = 1;
    pkt[4] = mic_enabled ? 0x03 : 0x02;

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, pkt,
                             DS5_BT_OUTPUT_EXT_SIZE - 4);
    ds5_write_le32(&pkt[DS5_BT_OUTPUT_EXT_SIZE - 4], crc);

    bt_hid_host_send_output(pkt, DS5_BT_OUTPUT_EXT_SIZE);
}

void audio_task(void *arg)
{
    (void)arg;

    SemaphoreHandle_t sem = (SemaphoreHandle_t)usb_audio_get_semaphore();
    LOG_INF("[AUDIO] Task started\n");

    for (;;) {
        if (xSemaphoreTake(sem, pdMS_TO_TICKS(25)) == pdTRUE) {
            if (usb_audio_is_active() &&
                usb_audio_read(pcm_block) &&
                bt_hid_host_get_state() == BT_HID_STATE_CONNECTED)
            {
                static int16_t spk_raw[HALF_ACCUM * 2];
                static int16_t hap_raw[HALF_ACCUM * 2];

                bool speaker_on = !config_get()->disable_speaker;

                for (int slot = 0; slot < 2; slot++) {
                    const uint32_t base = (uint32_t)slot * HALF_ACCUM;

                    for (uint32_t i = 0; i < HALF_ACCUM; i++) {
                        uint32_t src = base + i;
                        spk_raw[i * 2]     = pcm_block[src * USB_AUDIO_CHANNELS];
                        spk_raw[i * 2 + 1] = pcm_block[src * USB_AUDIO_CHANNELS + 1];
                        hap_raw[i * 2]     = pcm_block[src * USB_AUDIO_CHANNELS + 2];
                        hap_raw[i * 2 + 1] = pcm_block[src * USB_AUDIO_CHANNELS + 3];
                    }

                    decimate_haptics(hap_raw, haptic_slots[slot], HALF_ACCUM);

                    if (speaker_on) {
                        resample_512_480(spk_raw, spk_resamp);
                        encoding_in_progress = true;
                        int encoded = opus_encode(encoder, spk_resamp, OPUS_FRAME_SAMPLES,
                                                  opus_slots[slot], OPUS_OUT_SIZE);
                        encoding_in_progress = false;

                        if (encoder_reset_pending) {
                            encoder_reset_pending = false;
                            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
                        }
                        if (encoded <= 0) {
                            LOG_ERR("[AUDIO] Opus encode error: %d\n", encoded);
                            memset(opus_slots[slot], 0, OPUS_OUT_SIZE);
                        } else if (encoded < OPUS_OUT_SIZE) {
                            memset(opus_slots[slot] + encoded, 0,
                                   OPUS_OUT_SIZE - encoded);
                        }
                    } else {
                        memset(opus_slots[slot], 0, OPUS_OUT_SIZE);
                    }
                }

                send_audio_report();
            }
        }

        if (mic_status_pending &&
            bt_hid_host_get_state() == BT_HID_STATE_CONNECTED) {
            mic_status_pending = false;
            LOG_INF("[AUDIO] Mic %s\n", mic_enabled ? "enabled" : "disabled");
            send_mic_status();
        }
    }
}

void audio_set_headset(bool plugged)
{
    if (plugged != plug_headset)
        LOG_INF("[AUDIO] Headset %s\n", plugged ? "plugged" : "unplugged");
    plug_headset = plugged;
}

void audio_reset(void)
{
    packet_counter = 0;
    plug_headset = false;
    mic_enabled = false;
    mic_status_pending = false;
    mic_first_frame = false;
    memset(opus_slots, 0, sizeof(opus_slots));
    memset(haptic_slots, 0, sizeof(haptic_slots));
    if (encoding_in_progress) {
        encoder_reset_pending = true;
    } else if (encoder) {
        opus_encoder_ctl(encoder, OPUS_RESET_STATE);
    }
    if (decoder)
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    if (mic_queue)
        xQueueReset(mic_queue);
    usb_audio_mic_stop();
}

void audio_reset_encoder(void)
{
    memset(opus_slots, 0, sizeof(opus_slots));
    memset(haptic_slots, 0, sizeof(haptic_slots));
    if (encoding_in_progress) {
        encoder_reset_pending = true;
    } else if (encoder) {
        opus_encoder_ctl(encoder, OPUS_RESET_STATE);
    }
}

void audio_mic_feed(const uint8_t *opus_data, uint16_t len)
{
    if (!mic_enabled || !mic_queue) return;
    if (len < MIC_OPUS_SIZE) return;

    uint8_t frame[MIC_OPUS_SIZE];
    memcpy(frame, opus_data, MIC_OPUS_SIZE);
    if (xQueueSend(mic_queue, frame, 0) != pdTRUE) {
        uint8_t discard[MIC_OPUS_SIZE];
        xQueueReceive(mic_queue, discard, 0);
        xQueueSend(mic_queue, frame, 0);
    }
}

void audio_set_mic_active(bool active)
{
    mic_enabled = active && !config_get()->disable_mic;
    mic_status_pending = true;  /* deferred to audio_task (called from USB ISR) */
}

bool audio_mic_active(void)
{
    return mic_enabled;
}

/* ---- Mic decode task: runs independently at lower priority than audio_task ----
 * Blocks on mic_queue so it doesn't burn CPU when mic is inactive.
 * Decodes one Opus frame per wakeup → writes to USB mic ring buffer.
 * Keeps audio_task cycle at ~21ms regardless of mic decoding cost. */
void audio_mic_task(void *arg)
{
    (void)arg;
    static uint8_t  mic_opus_buf[MIC_OPUS_SIZE];
    static int16_t  mic_mono[OPUS_FRAME_SAMPLES];
    static int16_t  mic_stereo[OPUS_FRAME_SAMPLES * 2];

    for (;;) {
        if (!mic_queue) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xQueueReceive(mic_queue, mic_opus_buf, portMAX_DELAY) != pdTRUE)
            continue;

        if (!decoder || !mic_enabled)
            continue;

        int decoded = opus_decode(decoder, mic_opus_buf, MIC_OPUS_SIZE,
                                  mic_mono, OPUS_FRAME_SAMPLES, 0);
        if (decoded <= 0) {
            LOG_ERR("[MIC] Opus decode error: %d\n", decoded);
            continue;
        }
        if (!mic_first_frame) {
            mic_first_frame = true;
            LOG_INF("[AUDIO] First mic frame decoded (%d samples)\n", decoded);
        }

        for (int i = 0; i < decoded; i++) {
            mic_stereo[i * 2]     = mic_mono[i];
            mic_stereo[i * 2 + 1] = mic_mono[i];
        }
        usb_audio_mic_write(mic_stereo, (uint32_t)decoded);
    }
}
