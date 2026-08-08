#ifndef STATE_MGR_H
#define STATE_MGR_H

#include <stdbool.h>
#include <stdint.h>

#define SET_STATE_SIZE 47

/**
 * Initialize state manager with default SetStateData.
 * Called on each new controller connection.
 */
void state_mgr_init(const uint8_t *init_data, uint8_t len);

/**
 * Selectively merge incoming USB SetStateData into persistent state.
 * Only fields whose Allow flags are set in the incoming data are updated;
 * unflagged fields retain their previous values.
 * Matches DS5Dongle's state_update logic.
 */
void state_mgr_update(const uint8_t *data, uint8_t len);

/**
 * Copy the current merged state into the output buffer.
 * The output always has all Allow flags set so the controller processes
 * every field.
 */
void state_mgr_get(uint8_t *out, uint8_t size);

/**
 * Apply saved config values to the initial state.
 * Called once after state_mgr_init to override default volumes/gain
 * with user-configured values (matches DS5Dongle's state_init).
 */
void state_mgr_apply_config(uint8_t spk_vol, uint8_t hp_vol, uint8_t spk_gain,
                            uint8_t trigger_reduce);

/**
 * Set the speaker-active flag. When true, non-rumble output reports are
 * suppressed to avoid conflicting with audio-driven haptic feedback.
 * Called from USB Audio Class callbacks when the host opens/closes
 * the speaker streaming interface.
 */
void state_mgr_set_spk_active(bool active);

/**
 * Check if the speaker streaming interface is active.
 */
bool state_mgr_is_spk_active(void);

/**
 * Set speaker/headphone volume directly (from UAC SET_CUR).
 * Values are DualSense range [0, 127].
 */
void state_mgr_set_volume(uint8_t spk_vol, uint8_t hp_vol);

/**
 * Set speaker+headphone mute (from UAC SET_CUR).
 */
void state_mgr_set_mute(bool mute);

/**
 * Force the state volume/mute back to config values and mark dirty.
 * Called when lock_volume is enabled to immediately override
 * any game-set volume with the user's configured preference.
 */
void state_mgr_restore_config_volume(void);

/**
 * Check if a UAC volume/mute change is pending.
 * If true, the caller should send a BT output report with state_mgr_get()
 * to push the update to the controller, then call state_mgr_vol_ack().
 */
bool state_mgr_vol_dirty(void);

/**
 * Clear the volume-dirty flag after the update has been sent.
 */
void state_mgr_vol_ack(void);

/**
 * Clear accumulated flags (state[0], state[1]) so that subsequent
 * game frames start accumulating from zero.  Called after primer send.
 */
void state_mgr_clear_flags(void);

/**
 * Clear only one-shot action flags (like ResetLights) that must not
 * persist across BT sends.  Called after each regular BT output send
 * to prevent one-shot flags from accumulating indefinitely.
 */
void state_mgr_clear_oneshot_flags(void);

/**
 * Check whether the given SetStateData should be forwarded to the
 * controller right now.
 * When the speaker is active, only reports with UseRumbleNotHaptics
 * or UseRumbleNotHaptics2 are allowed through; other reports are
 * suppressed because the haptic actuators are driven by the audio
 * stream. state_mgr_update() must still be called unconditionally
 * so that the merged state is up-to-date when audio stops.
 * @param data  SetStateData (47 bytes, same pointer passed to update)
 * @return true if the report should be sent to the controller
 */
bool state_mgr_should_send(const uint8_t *data);

/**
 * Persist custom LED color into internal state so subsequent
 * state_mgr_get() calls (e.g. volume updates) keep the color.
 */
void state_mgr_set_led_color(uint8_t r, uint8_t g, uint8_t b);

#endif /* STATE_MGR_H */
