#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize the audio processing pipeline (Opus encoder, buffers).
 * Must be called once from main before starting audio_task.
 * Returns 0 on success, -1 on Opus init failure.
 */
int audio_init(void);

/**
 * Audio processing task entry point (FreeRTOS).
 * Blocks on USB audio semaphore, processes PCM, encodes Opus,
 * accumulates 2 frames and sends BT report 0x39 (double-frame).
 */
void audio_task(void *arg);

/**
 * Set headset plug state (parsed from BT input report byte[56] bit0).
 * When plugged, speaker tag switches from 0x93 to 0x96.
 */
void audio_set_headset(bool plugged);

/**
 * Reset audio state (called on controller disconnect).
 */
void audio_reset(void);

/**
 * Reset speaker encoder state (called on USB speaker stream close/open).
 * Clears Opus encoder internal prediction and frame_slot to prevent
 * quality degradation across stream close/open cycles.
 */
void audio_reset_encoder(void);

/**
 * Feed a mic Opus frame from the controller (called from BT callback context).
 * @param opus_data  pointer to the Opus-encoded frame
 * @param len        available byte length (must be >= 71)
 */
void audio_mic_feed(const uint8_t *opus_data, uint16_t len);

/**
 * Set mic active state (called when USB host opens/closes mic interface).
 * Sends a 0x32 status report to the controller to start/stop mic streaming.
 */
void audio_set_mic_active(bool active);

/**
 * Check if mic streaming is active.
 */
bool audio_mic_active(void);

/**
 * Mic decode task entry point (FreeRTOS).
 * Blocks on mic_queue, decodes Opus frames, writes to USB mic ring buffer.
 * Must run at lower priority than audio_task to avoid blocking speaker path.
 */
void audio_mic_task(void *arg);

#endif /* AUDIO_H */
