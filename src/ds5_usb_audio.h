#ifndef DS5_USB_AUDIO_H
#define DS5_USB_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define USB_AUDIO_EP_OUT        0x01
#define USB_AUDIO_MIC_EP_IN     0x82
#define USB_AUDIO_INTF_CTRL     0   /* Audio Control interface */
#define USB_AUDIO_INTF_STREAM   1   /* Audio Streaming OUT (speaker) */
#define USB_AUDIO_INTF_MIC      2   /* Audio Streaming IN  (mic) */

/* Speaker: 4ch, 16-bit, 48kHz: max (48+1)*4*2 = 392 bytes per 1ms frame */
#define USB_AUDIO_OUT_MPS       392
#define USB_AUDIO_SAMPLE_RATE   48000
#define USB_AUDIO_CHANNELS      4
#define USB_AUDIO_BITS          16

/* Mic: 2ch (mono duplicated to stereo), 16-bit, 48kHz: (48+1)*2*2 = 196 */
#define USB_AUDIO_MIC_CHANNELS  2
#define USB_AUDIO_MIC_MPS       196

/* 1024 samples per channel accumulated before processing.
 * 1024/48000 = 21.33ms = exactly 2 Opus frames per BT report.
 * Using 1024 instead of 512 means ONE semaphore per BT send cycle,
 * eliminating back-to-back semaphore bursts that cause stutter on Linux. */
#define USB_AUDIO_ACCUM_SAMPLES 1024

/* Mic ring buffer: 4 Opus frames of stereo samples (was 2, expanded for
 * more USB ISO IN jitter tolerance to prevent underflow/pop artifacts) */
#define USB_AUDIO_MIC_RING_SAMPLES (480 * 4)

/**
 * Register Audio Control + Audio Streaming OUT interfaces and endpoint.
 * Must be called BEFORE HID interface registration in usb_gamepad_init().
 */
void usb_audio_early_init(void);
void usb_audio_register(uint8_t busid);

/**
 * Get the audio portion of the config descriptor.
 * Returns pointer and length for embedding in the composite config descriptor.
 */
const uint8_t *usb_audio_get_desc(uint16_t *len);

/**
 * Check if audio streaming is active (USB host opened the speaker interface).
 */
bool usb_audio_is_active(void);

/**
 * Read accumulated PCM data when ready. Returns true if a full 512-sample
 * block is available. Output buffer must be at least
 * USB_AUDIO_ACCUM_SAMPLES * USB_AUDIO_CHANNELS * 2 bytes.
 * Called from audio_task context.
 */
bool usb_audio_read(int16_t *out);

/**
 * Reset audio streaming state (stream_active, PCM buffers, spk_active).
 * Called on USB RESET to avoid stale flags suppressing 0x31 output.
 */
void usb_audio_stop(void);

/**
 * Semaphore handle for audio task synchronization.
 * Given from ISR when a full PCM block is ready.
 */
void *usb_audio_get_semaphore(void);

/**
 * Write decoded stereo PCM from mic into USB ring buffer.
 * Called from audio_task after Opus decode.
 * @param samples  pointer to stereo int16 samples
 * @param count    number of stereo sample pairs (480 for one Opus frame)
 */
void usb_audio_mic_write(const int16_t *samples, uint32_t count);

/**
 * Check if mic streaming is active (host opened mic interface alt=1).
 */
bool usb_audio_mic_is_active(void);

/**
 * Reset mic streaming state (ring buffer, active flag).
 * Called together with usb_audio_stop() on disconnect.
 */
void usb_audio_mic_stop(void);

#endif /* DS5_USB_AUDIO_H */
