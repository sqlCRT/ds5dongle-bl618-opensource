#include "audio.h"
#include "ds5_usb_audio.h"
#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "config.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "bflb_mtimer.h"

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

#if LOG_LEVEL >= 3
/* Aggregate codec timings so UART logging does not perturb every frame. */
#define OPUS_TIMING_WINDOW  1280U

typedef struct {
    uint32_t count;
    uint32_t total_us;
    uint32_t min_us;
    uint32_t max_us;
} opus_timing_stats_t;

static void opus_timing_record(opus_timing_stats_t *stats,
                               const char *name, uint32_t elapsed_us)
{
    if (stats->count == 0 || elapsed_us < stats->min_us)
        stats->min_us = elapsed_us;
    if (elapsed_us > stats->max_us)
        stats->max_us = elapsed_us;

    stats->total_us += elapsed_us;
    stats->count++;

    if (stats->count >= OPUS_TIMING_WINDOW) {
        LOG_DBG("[OPUS] %s avg=%lu us min=%lu us max=%lu us n=%lu\n",
                name,
                (unsigned long)(stats->total_us / stats->count),
                (unsigned long)stats->min_us,
                (unsigned long)stats->max_us,
                (unsigned long)stats->count);
        memset(stats, 0, sizeof(*stats));
    }
}
#endif

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
static int encoder_force_channels;

/* Double-frame buffers for 0x39 report (2x haptics + 2x opus per packet) */
static uint8_t  opus_slots[2][OPUS_OUT_SIZE];
static int8_t   haptic_slots[2][HAPTIC_BUF_SIZE];

/* Pre-computed polyphase sinc filter in q15 fixed-point.
 * Init uses float for precision; hot path uses int32 MAC only.
 * Coefficients per phase are normalized to sum = 32768 (1.0 in q15),
 * so max accumulator value = 32768 * 32768 = 1,073,741,824 < INT32_MAX. */
static int16_t sinc_q15[RESAMP_PHASES][SINC_TAPS];

static void resamp_sinc_init(void)
{
    const float cutoff = 480.0f / 512.0f;

    for (int p = 0; p < RESAMP_PHASES; p++) {
        float frac = (float)p / (float)RESAMP_PHASES;
        float coeffs_f[SINC_TAPS];
        float sum = 0.0f;

        for (int t = 0; t < SINC_TAPS; t++) {
            float x = (float)(t - (SINC_HALF_TAPS - 1)) - frac;
            float sx = x * cutoff;
            float s;
            if (fabsf(sx) < 1e-6f)
                s = cutoff;
            else
                s = cutoff * sinf((float)M_PI * sx) / ((float)M_PI * sx);

            float wn = ((float)t + 0.5f) / (float)SINC_TAPS;
            float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * wn));

            coeffs_f[t] = s * w;
            sum += coeffs_f[t];
        }

        if (fabsf(sum) > 1e-6f) {
            for (int t = 0; t < SINC_TAPS; t++)
                coeffs_f[t] /= sum;
        }

        /* Float -> q15, then adjust largest tap so sum == 32768 exactly */
        int32_t qsum = 0;
        int max_idx = 0;
        float max_val = 0.0f;
        for (int t = 0; t < SINC_TAPS; t++) {
            int32_t q = (int32_t)(coeffs_f[t] * 32768.0f +
                                  (coeffs_f[t] >= 0 ? 0.5f : -0.5f));
            if (q > 32767) q = 32767;
            if (q < -32768) q = -32768;
            sinc_q15[p][t] = (int16_t)q;
            qsum += q;
            if (fabsf(coeffs_f[t]) > max_val) {
                max_val = fabsf(coeffs_f[t]);
                max_idx = t;
            }
        }
        int32_t fix = 32768 - qsum;
        int32_t corrected = sinc_q15[p][max_idx] + fix;
        if (corrected > 32767) corrected = 32767;
        if (corrected < -32768) corrected = -32768;
        sinc_q15[p][max_idx] = (int16_t)corrected;
    }
}

/* ---- Polyphase sinc resample 512 -> 480 (4ch USB PCM -> stereo, q15) ---- */
static void resample_512_480(const int16_t *in, int16_t *out)
{
    for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
        uint32_t src_pos = (uint32_t)i * 16;
        int center = (int)(src_pos / RESAMP_PHASES);
        int phase  = (int)(src_pos % RESAMP_PHASES);

        const int16_t *c = sinc_q15[phase];
        int32_t sum_l = (1 << 14);  /* +0.5 LSB rounding bias before >>15 */
        int32_t sum_r = (1 << 14);

        /* All but six outputs are away from an input edge. Keeping that hot
           path branch-free avoids 16 clamps and the inner-loop branch for
           every sample. Input is the original 4-channel USB buffer, so this
           also removes the temporary stereo de-interleave pass. */
        if (center >= SINC_HALF_TAPS - 1 &&
            center <= HALF_ACCUM - SINC_HALF_TAPS - 1) {
            const int16_t *s = in +
                (center - (SINC_HALF_TAPS - 1)) * USB_AUDIO_CHANNELS;
#define RESAMP_TAP(t) do { \
                int16_t coeff = c[(t)]; \
                sum_l += (int32_t)s[(t) * USB_AUDIO_CHANNELS] * coeff; \
                sum_r += (int32_t)s[(t) * USB_AUDIO_CHANNELS + 1] * coeff; \
            } while (0)
            RESAMP_TAP(0);
            RESAMP_TAP(1);
            RESAMP_TAP(2);
            RESAMP_TAP(3);
            RESAMP_TAP(4);
            RESAMP_TAP(5);
            RESAMP_TAP(6);
            RESAMP_TAP(7);
#undef RESAMP_TAP
        } else {
            for (int t = 0; t < SINC_TAPS; t++) {
                int idx = center + t - (SINC_HALF_TAPS - 1);
                if (idx < 0) idx = 0;
                if (idx >= HALF_ACCUM) idx = HALF_ACCUM - 1;

                int16_t coeff = c[t];
                sum_l += (int32_t)in[idx * USB_AUDIO_CHANNELS] * coeff;
                sum_r += (int32_t)in[idx * USB_AUDIO_CHANNELS + 1] * coeff;
            }
        }

        int32_t l = sum_l >> 15;
        int32_t r = sum_r >> 15;
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

    /* haptics_gain [1.0,2.0] → fixed-point 8.8: 256..512
     * Cached: recompute only when config value changes. */
    static float prev_gain_f = -1.0f;
    static int32_t gain_fp = 256;
    float cur_gain = config_get()->haptics_gain;
    if (cur_gain != prev_gain_f) {
        prev_gain_f = cur_gain;
        if (cur_gain < 1.0f) cur_gain = 1.0f;
        if (cur_gain > 2.0f) cur_gain = 2.0f;
        gain_fp = (int32_t)(cur_gain * 256.0f);
    }

    for (uint32_t i = 0; i < out_pairs; i++) {
        uint32_t idx = (i * HAPTIC_DECIMATE) * USB_AUDIO_CHANNELS;
        int32_t val_l = in[idx + 2];
        int32_t val_r = in[idx + 3];
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
                            OPUS_APPLICATION_RESTRICTED_CELT);
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Opus encoder init failed: %d\n", err);
        encoder = NULL;
        return -1;
    }

    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    err = opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1));
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Opus force mono failed: %d\n", err);
        encoder = NULL;
        return -1;
    }
    encoder_force_channels = 1;

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

__attribute__((section(".tcm_code")))
void audio_task(void *arg)
{
    (void)arg;

    SemaphoreHandle_t sem = (SemaphoreHandle_t)usb_audio_get_semaphore();
#if LOG_LEVEL >= 3
    opus_timing_stats_t encode_timing = {0};
#endif
    LOG_INF("[AUDIO] Task started\n");

    for (;;) {
        if (xSemaphoreTake(sem, pdMS_TO_TICKS(25)) == pdTRUE) {
            if (usb_audio_is_active() &&
                usb_audio_read(pcm_block) &&
                bt_hid_host_get_state() == BT_HID_STATE_CONNECTED)
            {
                bool speaker_on = !config_get()->disable_speaker;
                int target_channels = plug_headset ? 2 : 1;

                /* Opus supports changing the encoded channel count between
                 * frames. Keep the CTL in the owning audio task so headset
                 * reports cannot race an active opus_encode() call. */
                if (target_channels != encoder_force_channels) {
                    int ctl_err = opus_encoder_ctl(
                        encoder, OPUS_SET_FORCE_CHANNELS(target_channels));
                    if (ctl_err == OPUS_OK) {
                        encoder_force_channels = target_channels;
                    } else {
                        LOG_ERR("[AUDIO] Opus force %s failed: %d\n",
                                target_channels == 1 ? "mono" : "stereo",
                                ctl_err);
                    }
                }

                for (int slot = 0; slot < 2; slot++) {
                    if (slot == 1)
                        taskYIELD();

                    const uint32_t base = (uint32_t)slot * HALF_ACCUM;
                    const int16_t *slot_pcm =
                        &pcm_block[base * USB_AUDIO_CHANNELS];

                    decimate_haptics(slot_pcm, haptic_slots[slot], HALF_ACCUM);

                    if (speaker_on) {
                        resample_512_480(slot_pcm, spk_resamp);
                        encoding_in_progress = true;
#if LOG_LEVEL >= 3
                        uint64_t encode_start_us = bflb_mtimer_get_time_us();
#endif
                        int encoded = opus_encode(encoder, spk_resamp, OPUS_FRAME_SAMPLES,
                                                  opus_slots[slot], OPUS_OUT_SIZE);
                        encoding_in_progress = false;
#if LOG_LEVEL >= 3
                        uint32_t encode_elapsed_us = (uint32_t)
                            (bflb_mtimer_get_time_us() - encode_start_us);
                        opus_timing_record(&encode_timing, "enc",
                                           encode_elapsed_us);
#endif

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
__attribute__((section(".tcm_code")))
void audio_mic_task(void *arg)
{
    (void)arg;
    static uint8_t  mic_opus_buf[MIC_OPUS_SIZE];
    static int16_t  mic_mono[OPUS_FRAME_SAMPLES];
    static int16_t  mic_stereo[OPUS_FRAME_SAMPLES * 2];
#if LOG_LEVEL >= 3
    opus_timing_stats_t decode_timing = {0};
#endif

    for (;;) {
        if (!mic_queue) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xQueueReceive(mic_queue, mic_opus_buf, portMAX_DELAY) != pdTRUE)
            continue;

        if (!decoder || !mic_enabled)
            continue;

#if LOG_LEVEL >= 3
        uint64_t decode_start_us = bflb_mtimer_get_time_us();
#endif
        int decoded = opus_decode(decoder, mic_opus_buf, MIC_OPUS_SIZE,
                                  mic_mono, OPUS_FRAME_SAMPLES, 0);
#if LOG_LEVEL >= 3
        uint32_t decode_elapsed_us = (uint32_t)
            (bflb_mtimer_get_time_us() - decode_start_us);
        opus_timing_record(&decode_timing, "dec", decode_elapsed_us);
#endif
        if (decoded <= 0) {
            LOG_ERR("[MIC] Opus decode error: %d\n", decoded);
            continue;
        }
        if (!mic_first_frame) {
            mic_first_frame = true;
            LOG_INF("[AUDIO] First mic frame decoded (%d samples)\n", decoded);
        }

        /* Pack mono → stereo via uint32: one 32-bit write per sample (LE). */
        uint32_t *out32 = (uint32_t *)mic_stereo;
        for (int i = 0; i < decoded; i++) {
            uint16_t s = (uint16_t)mic_mono[i];
            out32[i] = (uint32_t)s | ((uint32_t)s << 16);
        }
        usb_audio_mic_write(mic_stereo, (uint32_t)decoded);
    }
}
