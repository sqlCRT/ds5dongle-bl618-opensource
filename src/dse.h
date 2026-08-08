#ifndef DSE_H
#define DSE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * DualSense Edge profile support.
 *
 * The Edge stores up to four custom controller profiles.  The PS Accessories
 * app reads them back through feature reports 0x70-0x7B, but the controller
 * only populates those reports (a "snapshot") after it processes a SET 0x80
 * unlock command, which takes ~3.5 s.  This module reproduces the native BT
 * host's unlock handshake so the profiles appear on the first app open
 * without delaying USB enumeration, and keeps the snapshot fresh after saves.
 */

void dse_on_edge_detected(void);

void dse_on_control_packet(const uint8_t *pkt, uint16_t size);

void dse_on_profile_write(uint8_t report_id);

bool dse_profiles_ready(void);

bool dse_is_profile_report(uint8_t report_id);

void dse_task(void);

void dse_reset(void);

#endif /* DSE_H */
