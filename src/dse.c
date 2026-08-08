#include "dse.h"
#include "bt_hid_host.h"
#include "debug_log.h"
#include <string.h>
#include "bflb_mtimer.h"

static int      unlock_phase      = 0;
static uint64_t unlock_started_us = 0;

static bool     profiles_ready       = true;

static uint64_t profile_written_us   = 0;
static int      post_save_round      = 0;

static uint8_t  prefetch_next        = 0;
static uint64_t prefetch_last_us     = 0;
static bool     prefetch_mark_ready  = false;

static void prefetch_start(bool mark_ready_after)
{
    prefetch_next       = 0x70;
    prefetch_last_us    = 0;
    prefetch_mark_ready = mark_ready_after;
}

bool dse_is_profile_report(uint8_t report_id)
{
    return report_id >= 0x70 && report_id <= 0x7B;
}

bool dse_profiles_ready(void)
{
    return profiles_ready;
}

void dse_on_edge_detected(void)
{
    /* 1) SET 0x65: verbatim echo of the 0x20 firmware report body
     *    (no CRC recompute, matches native host behaviour). */
    const uint8_t *fw_body;
    uint16_t fw_len;
    if (bt_hid_host_get_cached_feature(0x20, &fw_body, &fw_len) &&
        fw_len >= 61) {
        bt_hid_host_set_feature(0x65, fw_body, 61);
    }

    /* 2) SET 0x80 {0x70, 0x01, ...}: profile unlock.
     *    Must carry a valid CRC32 trailer. */
    uint8_t unlock[59];
    memset(unlock, 0, sizeof(unlock));
    unlock[0] = 0x70;
    unlock[1] = 0x01;
    bt_hid_host_set_feature_crc(0x80, unlock, sizeof(unlock));

    unlock_started_us = bflb_mtimer_get_time_us();
    unlock_phase      = 1;
    profiles_ready    = false;
    LOG_INF("[DSE] Unlock sequence started\n");
}

void dse_on_control_packet(const uint8_t *pkt, uint16_t size)
{
    if (size == 1 && (pkt[0] & 0xF0) == 0x00) {
        LOG_INF("[DSE] HID HANDSHAKE: 0x%02X (%s)\n", pkt[0],
               pkt[0] == 0x00 ? "success" : "rejected");
    }
}

void dse_on_profile_write(uint8_t report_id)
{
    if (report_id >= 0x60 && report_id <= 0x62) {
        profile_written_us = bflb_mtimer_get_time_us();
        post_save_round    = 0;
    }
}

void dse_task(void)
{
    uint64_t now_us = bflb_mtimer_get_time_us();

    /* --- Paced prefetch sequencer: one profile GET per 80 ms --- */
    if (prefetch_next != 0) {
        if (bt_hid_host_get_state() != BT_HID_STATE_CONNECTED) {
            prefetch_next       = 0;
            prefetch_mark_ready = false;
            return;
        }
        if (now_us - prefetch_last_us >= 80000ULL) {
            prefetch_last_us = now_us;
            bt_hid_host_get_feature(prefetch_next);
            if (prefetch_next == 0x7B) {
                prefetch_next = 0;
                if (prefetch_mark_ready) {
                    prefetch_mark_ready = false;
                    profiles_ready      = true;
                    LOG_INF("[DSE] Profile snapshot ready\n");
                }
            } else {
                prefetch_next++;
            }
        }
    }

    /* --- Post-save snapshot regeneration --- */
    if (profile_written_us != 0) {
        if (bt_hid_host_get_state() != BT_HID_STATE_CONNECTED) {
            profile_written_us = 0;
            post_save_round    = 0;
            return;
        }
        uint64_t since_write_us = now_us - profile_written_us;

        if (post_save_round == 0 && since_write_us >= 500000ULL) {
            uint8_t unlock[59];
            memset(unlock, 0, sizeof(unlock));
            unlock[0] = 0x70;
            unlock[1] = 0x01;
            bt_hid_host_set_feature_crc(0x80, unlock, sizeof(unlock));
            post_save_round = 1;
            LOG_INF("[DSE] Post-save: re-sent 0x80\n");
        } else if (post_save_round >= 1 && post_save_round <= 6 &&
                   since_write_us >= 1000000ULL +
                       250000ULL * (uint64_t)(post_save_round - 1)) {
            bt_hid_host_get_feature(0x81);
            post_save_round++;
        } else if (post_save_round == 7 && since_write_us >= 5500000ULL) {
            prefetch_start(false);
            post_save_round    = 8;
            profile_written_us = 0;
            LOG_INF("[DSE] Post-save: profile snapshot refetch started\n");
        }
    }

    /* --- Unlock wait: ~4 s after SET 0x80, start paced prefetch --- */
    if (unlock_phase == 0)
        return;

    if (bt_hid_host_get_state() != BT_HID_STATE_CONNECTED) {
        unlock_phase   = 0;
        profiles_ready = true;
        return;
    }

    if (unlock_phase == 1 &&
        now_us - unlock_started_us >= 4000000ULL) {
        prefetch_start(true);
        unlock_phase = 0;
        LOG_INF("[DSE] Unlock wait done, prefetching profile reports\n");
    }
}

void dse_reset(void)
{
    unlock_phase        = 0;
    unlock_started_us   = 0;
    profiles_ready      = true;
    profile_written_us  = 0;
    post_save_round     = 0;
    prefetch_next       = 0;
    prefetch_last_us    = 0;
    prefetch_mark_ready = false;
}
