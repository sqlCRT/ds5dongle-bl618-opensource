#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "dse.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/hci_host.h>
#include "conn_internal.h"
#include "keys.h"
#include "settings.h"
#include <string.h>
#include "debug_log.h"
#include <errno.h>
#include "bflb_efuse.h"
#include "hci_core.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bl616_glb.h"


#define BT_INQUIRY_LEN         5
#define BT_MAX_DISCOVERED      8
#define L2CAP_BR_MTU           672

/* ---------- feature report cache ---------- */

#define FEATURE_CACHE_SLOTS  24
#define FEATURE_DATA_MAX     256

static struct {
    uint8_t  report_id;
    uint8_t  data[FEATURE_DATA_MAX];
    uint16_t len;
} feature_cache[FEATURE_CACHE_SLOTS];

static void feature_cache_clear(void)
{
    memset(feature_cache, 0, sizeof(feature_cache));
}

static void feature_cache_store(uint8_t report_id,
                                const uint8_t *data, uint16_t len)
{
    if (len > FEATURE_DATA_MAX)
        len = FEATURE_DATA_MAX;

    int slot = -1;
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].report_id == report_id && feature_cache[i].len > 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
            if (feature_cache[i].len == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
        slot = 0;

    feature_cache[slot].report_id = report_id;
    memcpy(feature_cache[slot].data, data, len);
    feature_cache[slot].len = len;
}

/* ---------- persistent blacklist ----------
 *
 * When the user clears bonds (BOOT long-press), all bonded MACs are added
 * to a persistent blacklist stored in flash. This prevents the controller
 * from silently re-pairing via PS-only button press (controller-initiated
 * reconnect). The user must put the controller into PS+Share pairing mode
 * (dongle-initiated inquiry) to re-pair, which removes the MAC from the
 * blacklist on successful L2CAP HID connection.
 * Mirrors DS5Dongle's bt_cleared_addrs / BLCK TLV mechanism. */

#define BLACKLIST_MAX     8
#define NV_BLACKLIST_KEY  "DS5_BLK"

static bt_addr_t blacklist_addrs[BLACKLIST_MAX];
static int       blacklist_count;
static volatile bool blacklist_dirty;

static void blacklist_load(void)
{
    size_t real_len = 0;
    int ret = bt_settings_get_bin(NV_BLACKLIST_KEY,
                                  (u8_t *)blacklist_addrs,
                                  sizeof(blacklist_addrs), &real_len);
    if (ret == 0 && real_len > 0 &&
        (real_len % sizeof(bt_addr_t)) == 0) {
        blacklist_count = (int)(real_len / sizeof(bt_addr_t));
        if (blacklist_count > BLACKLIST_MAX)
            blacklist_count = BLACKLIST_MAX;
        LOG_INF("[BT] Blacklist: loaded %d entries\n", blacklist_count);
    } else {
        blacklist_count = 0;
    }
}

static void blacklist_persist(void)
{
    if (blacklist_count == 0) {
        settings_delete(NV_BLACKLIST_KEY);
    } else {
        bt_settings_set_bin(NV_BLACKLIST_KEY,
                            (const u8_t *)blacklist_addrs,
                            blacklist_count * sizeof(bt_addr_t));
    }
}

static bool blacklist_contains(const bt_addr_t *addr)
{
    for (int i = 0; i < blacklist_count; i++) {
        if (bt_addr_cmp(addr, &blacklist_addrs[i]) == 0)
            return true;
    }
    return false;
}

static void __attribute__((unused)) blacklist_add(const bt_addr_t *addr)
{
    if (blacklist_count >= BLACKLIST_MAX || blacklist_contains(addr))
        return;
    bt_addr_copy(&blacklist_addrs[blacklist_count++], addr);
    char a[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, a, sizeof(a));
    LOG_INF("[BT] Blacklist: added %s\n", a);
}

static void blacklist_remove(const bt_addr_t *addr)
{
    for (int i = 0; i < blacklist_count; i++) {
        if (bt_addr_cmp(addr, &blacklist_addrs[i]) == 0) {
            for (int j = i; j < blacklist_count - 1; j++)
                bt_addr_copy(&blacklist_addrs[j], &blacklist_addrs[j + 1]);
            blacklist_count--;
            char a[BT_ADDR_STR_LEN];
            bt_addr_to_str(addr, a, sizeof(a));
            LOG_INF("[BT] Blacklist: removed %s, %d remaining\n",
                   a, blacklist_count);
            return;
        }
    }
}

/* ---------- multi-controller memory ---------- */

#define MAX_BONDED_CONTROLLERS 8
#define NV_ACTIVE_CTRL_KEY     "active_ctrl"

static bt_addr_t bonded_list[MAX_BONDED_CONTROLLERS];
static uint8_t   bonded_count = 0;
static uint8_t   active_idx   = 0;

static void bonded_list_collect_cb(const struct bt_br_bond_info *info,
                                   void *user_data)
{
    (void)user_data;
    if (bonded_count < MAX_BONDED_CONTROLLERS) {
        bt_addr_copy(&bonded_list[bonded_count], info->addr);
        bonded_count++;
    }
}

static void bonded_list_load(void)
{
    bonded_count = 0;
    bt_br_foreach_bond(bonded_list_collect_cb, NULL);

    size_t real_len = 0;
    uint8_t stored_idx = 0;
    int ret = bt_settings_get_bin(NV_ACTIVE_CTRL_KEY, &stored_idx,
                                  sizeof(stored_idx), &real_len);
    if (ret == 0 && real_len == 1 && stored_idx < bonded_count) {
        active_idx = stored_idx;
    } else {
        active_idx = 0;
    }

    LOG_INF("[BT] Bonded list: %d controllers, active_idx=%d\n",
           bonded_count, active_idx);
    for (int i = 0; i < bonded_count; i++) {
        char a[BT_ADDR_STR_LEN];
        bt_addr_to_str(&bonded_list[i], a, sizeof(a));
        LOG_INF("[BT]   [%d]%s %s\n", i, (i == active_idx) ? "*" : " ", a);
    }
}

static void bonded_list_persist_idx(void)
{
    bt_settings_set_bin(NV_ACTIVE_CTRL_KEY, &active_idx, sizeof(active_idx));
}

static int bonded_list_find_addr(const bt_addr_t *addr)
{
    for (int i = 0; i < bonded_count; i++) {
        if (bt_addr_cmp(&bonded_list[i], addr) == 0)
            return i;
    }
    return -1;
}

/* ---------- internal state ---------- */

static struct {
    volatile enum bt_hid_host_state state;
    struct bt_conn *conn;
    bt_addr_t target_addr;
    bool has_target;

    struct bt_l2cap_br_chan ctrl_chan;
    struct bt_l2cap_br_chan intr_chan;
    volatile bool ctrl_connected;
    volatile bool intr_connected;
    volatile bool ctrl_accepting;
    volatile bool intr_accepting;
    bool hid_service_found;

    volatile bool outgoing;
    volatile bool is_dse;
    volatile bool check_dse;

    volatile bool connect_pending;
    volatile TickType_t connect_pending_tick;
    bt_addr_t     pending_addr;

    volatile bool fallback_retry;

    volatile bool sdp_pending;

    /* Security establishment deferred to task context */
    volatile bool security_pending;
    volatile bool security_done;
    volatile TickType_t acl_connect_tick;
    volatile TickType_t security_done_tick;

    /* Paced handshake: send feature GETs one at a time */
    const uint8_t *handshake_ids;
    volatile uint8_t handshake_count;
    volatile uint8_t handshake_next;
    volatile bool handshake_send_failed;

    bt_hid_input_cb_t  input_cb;
    bt_hid_state_cb_t  state_cb;
    bt_hid_primer_cb_t primer_cb;
    uint16_t filter_vid;
    uint16_t filter_pid;
} hid_ctx;

/* L2CAP requires headroom for ACL + L2CAP headers (typically ~8 bytes).
 * BT_L2CAP_BUF_SIZE adds this headroom to the payload MTU. */
#ifndef BT_L2CAP_BUF_SIZE
#define BT_L2CAP_BUF_SIZE(mtu) ((mtu) + 8)
#endif

struct net_buf_pool hid_tx_pool;
struct net_buf_pool hid_rx_pool;

static bool first_intr_logged = false;
static bool first_output_logged = false;

/* ---------- Application-level TX FIFO (CAN_SEND_NOW equivalent) ----------
 *
 * DS5Dongle uses BTstack's L2CAP_EVENT_CAN_SEND_NOW for flow control:
 *   bt_write() → enqueue to send_fifo (10-deep)
 *   CAN_SEND_NOW fires → dequeue one → l2cap_send → if more, request again
 *
 * Zephyr equivalent:
 *   bt_hid_host_send_output() → enqueue to app_tx_fifo (10-deep)
 *   on_hid_output_sent() fires when HCI ACKs the packet → drain next one
 *
 * This ensures only ONE packet is in-flight in the BT stack at any time,
 * preventing net_buf pool exhaustion and HCI credit starvation.
 */
#define APP_TX_DEPTH    10
#define APP_TX_DATA_SZ  80   /* 1 HID header + 79 bytes (covers 0x31 78-byte report) */

static uint8_t  app_tx_buf[APP_TX_DEPTH][APP_TX_DATA_SZ];
static uint16_t app_tx_len[APP_TX_DEPTH];
static volatile uint8_t  app_tx_head = 0;   /* next slot to consume */
static volatile uint8_t  app_tx_tail = 0;   /* next slot to fill */
static volatile bool     app_tx_busy = false; /* true: one packet is in HCI */

/* bt_conn_tx_cb_t: SDK declaration from conn_internal.h */
static void hid_tx_drain_one(void);  /* forward declaration */

static void on_hid_output_sent(struct bt_conn *conn, void *user_data)
{
    /* HCI controller acknowledged the previous packet — equivalent to
     * BTstack's L2CAP_EVENT_CAN_SEND_NOW: mark idle then drain next. */
    app_tx_busy = false;
    hid_tx_drain_one();
}

static void hid_tx_drain_one(void)
{
    if (app_tx_busy)
        return;
    if (app_tx_head == app_tx_tail)
        return;  /* FIFO empty */
    if (!hid_ctx.intr_connected || !hid_ctx.conn)
        return;

    app_tx_busy = true;

    uint8_t idx = app_tx_head;
    app_tx_head = (app_tx_head + 1) % APP_TX_DEPTH;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        LOG_WRN("[HID] drain: pool empty, dropping frame (head=%u tail=%u)\n",
               app_tx_head, app_tx_tail);
        app_tx_busy = false;
        return;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_mem(buf, app_tx_buf[idx], app_tx_len[idx]);

    /* bt_l2cap_br_chan_send_cb added to SDK l2cap_br.c */
    extern int bt_l2cap_br_chan_send_cb(struct bt_l2cap_chan *chan,
                                        struct net_buf *buf,
                                        bt_conn_tx_cb_t cb, void *user_data);

    int err = bt_l2cap_br_chan_send_cb(&hid_ctx.intr_chan.chan, buf,
                                        on_hid_output_sent, NULL);
    if (err < 0) {
        LOG_ERR("[HID] drain: send err=%d\n", err);
        net_buf_unref(buf);
        app_tx_busy = false;
    }
    /* On success: app_tx_busy stays true until on_hid_output_sent fires */
}

static void app_tx_fifo_reset(void)
{
    app_tx_head = 0;
    app_tx_tail = 0;
    app_tx_busy = false;
}

static struct bt_br_discovery_result discovery_results[BT_MAX_DISCOVERED];

/* ---------- helpers ---------- */

static const bt_addr_t *conn_get_br_addr(const struct bt_conn *conn)
{
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0)
        return info.br.dst;
    return NULL;
}

static const char *state_name(enum bt_hid_host_state s) {
    switch (s) {
    case BT_HID_STATE_IDLE:          return "IDLE";
    case BT_HID_STATE_SCANNING:      return "SCANNING";
    case BT_HID_STATE_CONNECTING:    return "CONNECTING";
    case BT_HID_STATE_SDP_QUERY:     return "SDP_QUERY";
    case BT_HID_STATE_L2CAP_CONTROL: return "L2CAP_CTRL";
    case BT_HID_STATE_L2CAP_INTERRUPT: return "L2CAP_INTR";
    case BT_HID_STATE_HANDSHAKE:     return "HANDSHAKE";
    case BT_HID_STATE_CONNECTED:     return "CONNECTED";
    case BT_HID_STATE_DISCONNECTING: return "DISCONNECTING";
    default:                         return "?";
    }
}

static void set_state(enum bt_hid_host_state new_state)
{
    LOG_INF("[BT-FSM] %s -> %s\n", state_name(hid_ctx.state), state_name(new_state));
    hid_ctx.state = new_state;

    if (new_state == BT_HID_STATE_CONNECTED && hid_ctx.conn) {
        const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
        if (remote) {
            bt_addr_copy(&hid_ctx.target_addr, remote);
            hid_ctx.has_target = true;
            char a[BT_ADDR_STR_LEN];
            bt_addr_to_str(remote, a, sizeof(a));
            LOG_INF("[BT] Connected to bonded device: %s\n", a);
        }
    }

    if (hid_ctx.state_cb)
        hid_ctx.state_cb(new_state);
}

/* ---------- L2CAP callbacks ---------- */

static struct net_buf *l2cap_alloc_buf(struct bt_l2cap_chan *chan)
{
    return net_buf_alloc(&hid_rx_pool, K_NO_WAIT);
}

static void handshake_send_next(void)
{
    if (hid_ctx.state != BT_HID_STATE_HANDSHAKE)
        return;

    if (hid_ctx.handshake_next < hid_ctx.handshake_count) {
        uint8_t rid = hid_ctx.handshake_ids[hid_ctx.handshake_next];
        if (bt_hid_host_get_feature(rid) == 0) {
            hid_ctx.handshake_next++;
            hid_ctx.handshake_send_failed = false;
        } else {
            hid_ctx.handshake_send_failed = true;
            LOG_ERR("[HID] GET_REPORT(0x%02x) send failed, will retry\n", rid);
        }
    } else if (hid_ctx.check_dse &&
               hid_ctx.state == BT_HID_STATE_HANDSHAKE) {
        hid_ctx.check_dse = false;
        hid_ctx.is_dse    = false;
        LOG_INF("[HID] DualSense (non-Edge) assumed (handshake complete)\n");
        set_state(BT_HID_STATE_CONNECTED);
    }
}

static int l2cap_ctrl_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    uint8_t *data = buf->data;
    uint16_t len  = buf->len;

    if (len < 1)
        return 0;

    uint8_t header = data[0];
    uint8_t trans  = (header >> 4) & 0x0F;
    uint8_t param  = header & 0x0F;

    switch (trans) {
    case HID_TRANS_HANDSHAKE:
        LOG_INF("[HID] Handshake: 0x%02x\n", param);
        if (hid_ctx.check_dse && param == HID_HANDSHAKE_INVALID_ID) {
            hid_ctx.check_dse = false;
            hid_ctx.is_dse    = false;
            LOG_INF("[HID] DualSense (non-Edge) confirmed\n");
            set_state(BT_HID_STATE_CONNECTED);
        }
        handshake_send_next();
        break;

    case HID_TRANS_DATA:
        if (param == HID_REPORT_FEATURE) {
            if (len > 2) {
                uint8_t rid = data[1];
                LOG_INF("[HID] Feature report received, id=0x%02x len=%d\n",
                       rid, len - 2);

                feature_cache_store(rid, data + 2, len - 2);

                if (rid == 0x70 && hid_ctx.check_dse) {
                    hid_ctx.check_dse = false;
                    hid_ctx.is_dse    = true;
                    LOG_INF("[HID] DualSense Edge detected\n");
                    dse_on_edge_detected();
                    set_state(BT_HID_STATE_CONNECTED);
                }
            } else {
                LOG_WRN("[HID] Feature DATA too short (len=%d), skipping\n",
                       len);
            }
            handshake_send_next();
        }
        break;

    default:
        LOG_DBG("[HID] Control: trans=0x%x param=0x%x len=%d\n",
               trans, param, len);
        break;
    }

    dse_on_control_packet(data, len);

    return 0;
}

static int l2cap_intr_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    uint8_t *data = buf->data;
    uint16_t len  = buf->len;

    if (len < 2)
        return 0;

    uint8_t header = data[0];
    uint8_t trans  = (header >> 4) & 0x0F;
    uint8_t param  = header & 0x0F;

    if (trans == HID_TRANS_DATA && param == HID_REPORT_INPUT) {
        if (!first_intr_logged) {
            first_intr_logged = true;
            LOG_INF("[HID] First input report from controller: len=%d rid=0x%02x\n",
                   len - 1, data[1]);
        }
        if (hid_ctx.input_cb) {
            hid_ctx.input_cb(data + 1, len - 1);
        }
    } else {
        LOG_WRN("[HID] intr_recv: unexpected trans=0x%x param=0x%x len=%d\n",
               trans, param, len);
    }

    return 0;
}

static void l2cap_ctrl_connected(struct bt_l2cap_chan *chan)
{
    LOG_DBG("[HID-CB] l2cap_ctrl_connected ENTER chan=%p state=%d\n",
           chan, hid_ctx.state);
    hid_ctx.ctrl_accepting = false;
    hid_ctx.ctrl_connected = true;

    /* Only create outgoing interrupt channel if WE initiated the connection
     * (via inquiry → connect). For controller-initiated reconnects, the
     * controller will open the interrupt channel via the L2CAP server. */
    if (hid_ctx.outgoing) {
        set_state(BT_HID_STATE_L2CAP_INTERRUPT);

        int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                        &hid_ctx.intr_chan.chan,
                                        HID_PSM_INTERRUPT);
        if (err) {
            LOG_ERR("[HID] Failed to connect interrupt channel: %d\n", err);
            bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }
}

static void l2cap_intr_connected(struct bt_l2cap_chan *chan)
{
    LOG_DBG("[HID-CB] l2cap_intr_connected ENTER chan=%p state=%d\n",
           chan, hid_ctx.state);
    hid_ctx.intr_accepting = false;
    hid_ctx.intr_connected = true;


    /* Fire primer callback immediately — equivalent to DS5Dongle's
     * L2CAP_EVENT_CHANNEL_OPENED handler calling update_state(yellow).
     * This is the earliest possible moment to send the 0x32 primer. */
    if (hid_ctx.primer_cb)
        hid_ctx.primer_cb();

    /* Successful L2CAP HID open removes this MAC from the blacklist,
     * treating it as an explicit user re-pair (PS+Share mode). */
    if (hid_ctx.conn) {
        const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
        if (remote && blacklist_contains(remote)) {
            blacklist_remove(remote);
            blacklist_dirty = true;
        }
    }

    /* Stop accepting further connections while connected (matches
     * DS5Dongle's gap_connectable_control(false) on HID open). */
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);

    /* Shorten the Link Supervision Timeout so the BL618 controller
     * detects a dead link within ~5 s instead of the default ~20 s.
     * HCI Write_Link_Supervision_Timeout: OGF 0x03, OCF 0x0037.
     * Timeout unit = 0.625 ms; 5 s = 8000 slots = 0x1F40. */
    if (hid_ctx.conn) {
        uint16_t handle = hid_ctx.conn->handle;
        struct net_buf *buf = bt_hci_cmd_create(
            BT_OP(BT_OGF_BASEBAND, 0x0037), 4);
        if (buf) {
            net_buf_add_le16(buf, handle);
            net_buf_add_le16(buf, 0x1F40);
            int err = bt_hci_cmd_send_sync(
                BT_OP(BT_OGF_BASEBAND, 0x0037), buf, NULL);
            LOG_INF("[BT] Write Link Supervision Timeout: handle=0x%04x "
                   "timeout=5s err=%d\n", handle, err);
        }
    }

    set_state(BT_HID_STATE_HANDSHAKE);

    /* Request feature reports in the same order as DS5Dongle:
     * 0x09 (pairing/firmware), 0x20 (firmware info), 0x22 (additional),
     * 0x05 (calibration), then 0x70 to probe for DualSense Edge.
     * Paced: send one at a time, next sent in l2cap_ctrl_recv on response. */
    static const uint8_t handshake_ids[] = {
        DS5_FEATURE_REPORT_PAIRING, DS5_FEATURE_REPORT_FIRMWARE,
        0x22, DS5_FEATURE_REPORT_CALIBRATION, 0x70
    };
    hid_ctx.handshake_ids   = handshake_ids;
    hid_ctx.handshake_count = sizeof(handshake_ids);
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.check_dse       = true;

    handshake_send_next();
}

static void l2cap_ctrl_disconnected(struct bt_l2cap_chan *chan)
{
    LOG_INF("[HID] Control channel disconnected (fallback_retry=%d conn=%p)\n",
           hid_ctx.fallback_retry, hid_ctx.conn);
    hid_ctx.ctrl_accepting = false;
    hid_ctx.ctrl_connected = false;
    if (!hid_ctx.intr_connected && hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void l2cap_intr_disconnected(struct bt_l2cap_chan *chan)
{
    LOG_INF("[HID] Interrupt channel disconnected\n");
    hid_ctx.intr_accepting = false;
    hid_ctx.intr_connected = false;
    if (!hid_ctx.ctrl_connected && hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static struct bt_l2cap_chan_ops ctrl_ops = {
    .alloc_buf    = l2cap_alloc_buf,
    .recv         = l2cap_ctrl_recv,
    .connected    = l2cap_ctrl_connected,
    .disconnected = l2cap_ctrl_disconnected,
};

static struct bt_l2cap_chan_ops intr_ops = {
    .alloc_buf    = l2cap_alloc_buf,
    .recv         = l2cap_intr_recv,
    .connected    = l2cap_intr_connected,
    .disconnected = l2cap_intr_disconnected,
};

/* ---------- SDP callback ---------- */

static uint8_t sdp_discover_cb(struct bt_conn *conn,
                                struct bt_sdp_client_result *result)
{
    if (conn != hid_ctx.conn)
        return BT_SDP_DISCOVER_UUID_STOP;

    if (!result) {
        LOG_INF("[SDP] Discovery complete, HID found=%d\n",
               hid_ctx.hid_service_found);

        if (!hid_ctx.hid_service_found) {
            LOG_INF("[SDP] No HID service, disconnecting\n");
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return BT_SDP_DISCOVER_UUID_STOP;
        }

        set_state(BT_HID_STATE_L2CAP_CONTROL);

        memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
        memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));

        hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
        hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
        hid_ctx.intr_chan.chan.ops = &intr_ops;
        hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;

        int err = bt_l2cap_chan_connect(conn, &hid_ctx.ctrl_chan.chan,
                                           HID_PSM_CONTROL);
        if (err) {
            LOG_ERR("[SDP] Failed to connect control channel: %d\n", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        return BT_SDP_DISCOVER_UUID_STOP;
    }

    LOG_INF("[SDP] Found HID service record\n");
    hid_ctx.hid_service_found = true;
    return BT_SDP_DISCOVER_UUID_CONTINUE;
}

/* ---------- SSP Authentication ---------- */

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("[BT] SSP confirm passkey: %06u (auto-accepting)\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("[BT] SSP display passkey: %06u\n", passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
    LOG_INF("[BT] SSP pairing cancelled\n");
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
    LOG_INF("[BT] Pairing complete, bonded=%d\n", bonded);

    if (bonded) {
        const bt_addr_le_t *dst = bt_conn_get_dst(conn);
        if (dst) {
            const bt_addr_t *addr = &dst->a;
            int idx = bonded_list_find_addr(addr);
            if (idx >= 0) {
                active_idx = (uint8_t)idx;
            } else {
                /* link_key_notify may not have fired yet, so
                 * bt_br_foreach_bond won't find the new key.
                 * Manually add this address to bonded_list. */
                if (bonded_count < MAX_BONDED_CONTROLLERS) {
                    bt_addr_copy(&bonded_list[bonded_count], addr);
                    active_idx = bonded_count;
                    bonded_count++;
                } else {
                    active_idx = 0;
                }
            }
            bonded_list_persist_idx();
            LOG_INF("[BT] Active controller updated: idx=%d, total=%d\n",
                   active_idx, bonded_count);
        }
    }
}

static void auth_pairing_failed(struct bt_conn *conn,
                                 enum bt_security_err reason)
{
    LOG_ERR("[BT] Pairing failed, reason=%d\n", reason);
}

static struct bt_conn_auth_cb auth_callbacks = {
    .passkey_display   = auth_passkey_display,
    .passkey_confirm   = auth_passkey_confirm,
    .cancel            = auth_cancel,
    .pairing_complete  = auth_pairing_complete,
    .pairing_failed    = auth_pairing_failed,
};

/* ---------- SDP ---------- */

static void start_sdp_discovery(struct bt_conn *conn)
{
    static struct bt_sdp_discover_params params;

    static struct bt_uuid_16 hid_uuid = BT_UUID_INIT_16(0x1124);

    params.uuid  = &hid_uuid.uuid;
    params.func  = sdp_discover_cb;
    params.pool  = &hid_tx_pool;

    set_state(BT_HID_STATE_SDP_QUERY);

    int err = bt_sdp_discover(conn, &params);
    if (err) {
        LOG_ERR("[SDP] Failed to start discovery: %d\n", err);
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* ---------- BR/EDR connection callbacks ---------- */

static void security_changed(struct bt_conn *conn, bt_security_t level,
                              enum bt_security_err err)
{
    LOG_DBG("[BT-CB] security_changed ENTER conn=%p level=%d err=%d state=%d\n",
           conn, level, err, hid_ctx.state);

    if (conn != hid_ctx.conn) {
        LOG_DBG("[BT-CB] security_changed: not our conn, ignoring\n");
        return;
    }

    if (err) {
        LOG_ERR("[BT] Security elevation failed: level=%d err=%d\n", level, err);
        /* Clear stale link key so next reconnect triggers fresh SSP pairing
         * (mirrors DS5Dongle's gap_drop_link_key on AUTHENTICATION_COMPLETE fail) */
        const bt_addr_t *remote = conn_get_br_addr(conn);
        if (remote) {
            extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
            bt_keys_link_key_clear_addr(remote);
            LOG_WRN("[BT] Cleared stale link key for re-pairing\n");
        }
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    LOG_INF("[BT] Security level raised to %d (outgoing=%d state=%s)\n",
           level, hid_ctx.outgoing, state_name(hid_ctx.state));

    hid_ctx.security_done = true;
    hid_ctx.security_done_tick = xTaskGetTickCount();

    if (level >= BT_SECURITY_L2 && hid_ctx.outgoing
        && (hid_ctx.state == BT_HID_STATE_CONNECTING
            || hid_ctx.state == BT_HID_STATE_SDP_QUERY)) {
        /* Encryption complete for outgoing connection (new pairing).
         * Defer L2CAP channel creation to task context, matching
         * DS5Dongle's ENCRYPTION_CHANGE + new_pair path. */
        LOG_DBG("[BT] security_changed: outgoing — deferring L2CAP to task\n");
        hid_ctx.sdp_pending = true;
    } else if (level >= BT_SECURITY_L2 && !hid_ctx.outgoing) {
        /* Incoming reconnection: encryption OK.
         * Controller will create L2CAP channels via our servers. */
        LOG_INF("[BT] security OK (incoming), waiting for controller L2CAP\n");
    }
    LOG_DBG("[BT-CB] security_changed EXIT\n");
}

static void conn_connected(struct bt_conn *conn, uint8_t err)
{
    LOG_DBG("[BT-CB] conn_connected ENTER conn=%p err=%d state=%d\n",
           conn, err, hid_ctx.state);

    if (err) {
        LOG_ERR("[BT] Connection failed: 0x%02x\n", err);
        if (hid_ctx.conn != NULL && conn != hid_ctx.conn)
            return;
        hid_ctx.conn = NULL;
        hid_ctx.outgoing = false;
        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);
        LOG_INF("[BT] Re-enabled page scan after failed connection\n");
        set_state(BT_HID_STATE_IDLE);
        return;
    }

    char addr_str[BT_ADDR_STR_LEN];
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote)
        bt_addr_to_str(remote, addr_str, sizeof(addr_str));
    else
        snprintf(addr_str, sizeof(addr_str), "??:??:??:??:??:??");
    LOG_INF("[BT] Connected to %s\n", addr_str);

    if (hid_ctx.conn != NULL && conn != hid_ctx.conn) {
        LOG_WRN("[BT] Already connected, rejecting second ACL from %s\n",
               addr_str);
        bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }

    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        bt_br_discovery_stop();

    if (!hid_ctx.outgoing && remote && blacklist_contains(remote)) {
        LOG_WRN("[BT] Rejecting blacklisted device %s "
               "(re-pair via PS+Share to unblock)\n", addr_str);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    if (!hid_ctx.conn) {
        LOG_DBG("[BT] conn_connected: taking ref on conn=%p\n", conn);
        hid_ctx.conn = bt_conn_ref(conn);
    }

    hid_ctx.fallback_retry = false;
    hid_ctx.connect_pending = false;
    hid_ctx.connect_pending_tick = 0;
    LOG_INF("[BT] conn_connected: fallback_retry CLEARED (incoming conn)\n");

    set_state(BT_HID_STATE_CONNECTING);

    /* Log internal security state for diagnostics */
    LOG_DBG("[BT] conn handle=0x%04x sec_level=%d required_sec=%d encrypt=%d\n",
           conn->handle, conn->sec_level, conn->required_sec_level,
           conn->encrypt);

    /* Defer security establishment to task context.
     *
     * Root cause found in SDK: conn_new() does NOT clear the bt_conn struct,
     * so required_sec_level retains the stale value (L2) from the previous
     * connection.  bt_conn_set_security(L2) checks:
     *     if (sec_level >= sec || required_sec_level >= sec) return 0;
     * and returns immediately without doing anything.
     *
     * Fix: reset required_sec_level in poll_security() (task context) before
     * calling bt_conn_set_security.  We also can't call bt_hci_cmd_send_sync
     * from this callback (deadlock risk), so deferring is the safe approach. */
    hid_ctx.security_pending = true;
    hid_ctx.security_done = false;
    hid_ctx.acl_connect_tick = xTaskGetTickCount();

    LOG_DBG("[BT] %s connection — security deferred to task context\n",
           hid_ctx.outgoing ? "Outgoing" : "Incoming");
    LOG_DBG("[BT-CB] conn_connected EXIT\n");
}

static volatile int8_t cached_rssi = 1; /* 1 = unknown (positive → web shows "Unknown") */
static volatile bool switch_pending = false;

static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("[BT-CB] conn_disconnected: conn=%p hid_ctx.conn=%p reason=0x%02x "
           "state=%d fallback_retry=%d\n",
           conn, hid_ctx.conn, reason, hid_ctx.state, hid_ctx.fallback_retry);

    if (conn != hid_ctx.conn) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
               "state=%d)\n", reason, hid_ctx.state);
        /* If a fallback outgoing connect is waiting for ACL teardown, fire it now */
        if (hid_ctx.connect_pending && hid_ctx.connect_pending_tick != 0 &&
            hid_ctx.state == BT_HID_STATE_IDLE) {
            hid_ctx.connect_pending_tick = xTaskGetTickCount();
            LOG_INF("[BT] ACL clear — fallback connect ready\n");
        }
        return;
    }

    const char *r_str = reason == 0x08 ? "CONN_TIMEOUT" :
                        reason == 0x13 ? "REMOTE_USER" :
                        reason == 0x16 ? "LOCAL_HOST" : "OTHER";
    LOG_INF("[BT] Disconnected reason=0x%02x (%s)\n", reason, r_str);

    bt_conn_unref(hid_ctx.conn);
    hid_ctx.conn = NULL;

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.connect_pending = false;

    first_intr_logged  = false;
    first_output_logged = false;

    app_tx_fifo_reset();

    feature_cache_clear();
    dse_reset();
    cached_rssi = 1;

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    LOG_INF("[BT] Re-enabled connectable+discoverable, ready for new connection\n");
    set_state(BT_HID_STATE_IDLE);

    if (switch_pending) {
        switch_pending = false;
        LOG_INF("[BT] Switch: old ACL teardown complete, waiting for "
               "controller [%d/%d] to reconnect\n", active_idx, bonded_count);
        return;
    }

    LOG_INF("[BT] conn_disconnected: fallback_retry=%d bonded_count=%d\n",
           hid_ctx.fallback_retry, bonded_count);
    if (hid_ctx.fallback_retry && bonded_count > 0) {
        hid_ctx.fallback_retry = false;
        memcpy(hid_ctx.pending_addr.val, bonded_list[active_idx].val, 6);
        hid_ctx.connect_pending = true;
        hid_ctx.connect_pending_tick = xTaskGetTickCount();
        LOG_INF("[BT] Fallback: outgoing connect scheduled (ACL already down)\n");
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected        = conn_connected,
    .disconnected     = conn_disconnected,
    .security_changed = security_changed,
};

/* ---------- Inquiry / Discovery ---------- */

static void discovery_complete(struct bt_br_discovery_result *results,
                               size_t count)
{
    LOG_INF("[BT] Inquiry complete, found %zu device(s)\n", count);

    if (hid_ctx.conn) {
        LOG_INF("[BT] Inquiry ended but ACL already established, skipping\n");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        char addr_str[BT_ADDR_STR_LEN];
        bt_addr_to_str(&results[i].addr, addr_str, sizeof(addr_str));

        uint32_t cod = results[i].cod[0]
                     | ((uint32_t)results[i].cod[1] << 8)
                     | ((uint32_t)results[i].cod[2] << 16);
        uint8_t major = (cod >> 8) & 0x1F;
        uint8_t minor = (cod >> 2) & 0x3F;

        LOG_DBG("[BT] Device: %s COD=0x%06x (major=%d minor=%d)\n",
               addr_str, cod, major, minor);

        /* CoD Major Class 0x05 = Peripheral, Minor bit 1 = Gamepad */
        bool is_gamepad = (major == 0x05) && (minor & 0x02);

        if (is_gamepad || hid_ctx.has_target) {
            if (hid_ctx.has_target &&
                bt_addr_cmp(&results[i].addr, &hid_ctx.target_addr) != 0)
                continue;

            LOG_INF("[BT] Gamepad found, deferring connect to task context\n");
            bt_addr_copy(&hid_ctx.pending_addr, &results[i].addr);
            hid_ctx.connect_pending = true;
            hid_ctx.connect_pending_tick = 0;
            /* Set CONNECTING instead of IDLE to prevent the auto-reconnect
             * logic from starting a new scan before poll_connect runs. */
            set_state(BT_HID_STATE_CONNECTING);
            return;
        }
    }

    LOG_INF("[BT] No gamepad found, will retry via bt_task\n");
    set_state(BT_HID_STATE_IDLE);
}

/* ---------- L2CAP server (for controller-initiated reconnection) ---------- */

static int l2cap_server_reject_blacklisted(struct bt_conn *conn)
{
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote && blacklist_contains(remote)) {
        char a[BT_ADDR_STR_LEN];
        bt_addr_to_str(remote, a, sizeof(a));
        LOG_WRN("[HID] Rejecting L2CAP from blacklisted %s\n", a);
        return -EACCES;
    }
    return 0;
}

static void l2cap_server_ensure_conn(struct bt_conn *conn)
{
    if (!hid_ctx.conn) {
        hid_ctx.conn = bt_conn_ref(conn);
        set_state(BT_HID_STATE_CONNECTING);
    }
}

static int l2cap_ctrl_server_accept(struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    char addr_str[BT_ADDR_STR_LEN] = "??";
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote)
        bt_addr_to_str(remote, addr_str, sizeof(addr_str));

    LOG_DBG("[HID] Incoming L2CAP control (PSM 0x%04x) from %s "
           "state=%s conn=%p our_conn=%p\n",
           HID_PSM_CONTROL, addr_str, state_name(hid_ctx.state),
           conn, hid_ctx.conn);

    int err = l2cap_server_reject_blacklisted(conn);
    if (err) return err;

    /* Reject L2CAP from a different ACL than the primary connection —
     * prevents a racing "rejected" second controller from stealing channels */
    if (hid_ctx.conn && conn != hid_ctx.conn) {
        LOG_WRN("[HID] Control: rejecting L2CAP from non-primary conn %p "
               "(primary=%p)\n", conn, hid_ctx.conn);
        return -EACCES;
    }

    if (hid_ctx.ctrl_connected || hid_ctx.ctrl_accepting) {
        LOG_WRN("[HID] Control channel already connected/accepting\n");
        return -ENOMEM;
    }
    hid_ctx.ctrl_accepting = true;

    memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
    hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
    hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
    *chan = &hid_ctx.ctrl_chan.chan;

    l2cap_server_ensure_conn(conn);
    LOG_INF("[HID] Control server: accepted, chan=%p\n", *chan);
    return 0;
}

static int l2cap_intr_server_accept(struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    char addr_str[BT_ADDR_STR_LEN] = "??";
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote)
        bt_addr_to_str(remote, addr_str, sizeof(addr_str));

    LOG_DBG("[HID] Incoming L2CAP interrupt (PSM 0x%04x) from %s "
           "state=%s conn=%p our_conn=%p\n",
           HID_PSM_INTERRUPT, addr_str, state_name(hid_ctx.state),
           conn, hid_ctx.conn);

    int err = l2cap_server_reject_blacklisted(conn);
    if (err) return err;

    /* Reject L2CAP from a different ACL than the primary connection */
    if (hid_ctx.conn && conn != hid_ctx.conn) {
        LOG_WRN("[HID] Interrupt: rejecting L2CAP from non-primary conn %p "
               "(primary=%p)\n", conn, hid_ctx.conn);
        return -EACCES;
    }

    if (hid_ctx.intr_connected || hid_ctx.intr_accepting) {
        LOG_WRN("[HID] Interrupt channel already connected/accepting\n");
        return -ENOMEM;
    }
    hid_ctx.intr_accepting = true;

    memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));
    hid_ctx.intr_chan.chan.ops = &intr_ops;
    hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;
    *chan = &hid_ctx.intr_chan.chan;

    l2cap_server_ensure_conn(conn);
    LOG_INF("[HID] Interrupt server: accepted, chan=%p\n", *chan);
    return 0;
}

static struct bt_l2cap_server l2cap_hid_ctrl_server = {
    .psm    = HID_PSM_CONTROL,
    .accept = l2cap_ctrl_server_accept,
};

static struct bt_l2cap_server l2cap_hid_intr_server = {
    .psm    = HID_PSM_INTERRUPT,
    .accept = l2cap_intr_server_accept,
};

/* ---------- SDP Service Record for HID Host ---------- */

static struct bt_sdp_attribute hid_host_attrs[] = {
    BT_SDP_NEW_SERVICE,
    /* Service Class ID List: HID (0x1124) */
    {
        BT_SDP_ATTR_SVCLASS_ID_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x1124) },
          ),
        }
    },
    /* Protocol Descriptor List: L2CAP(PSM=0x0011) + HIDP */
    {
        BT_SDP_ATTR_PROTO_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0100) },
                    { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(HID_PSM_CONTROL) },
                ),
              },
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0011) },
                ),
              },
          ),
        }
    },
    /* Additional Protocol Descriptor List: L2CAP(PSM=0x0013) + HIDP */
    {
        BT_SDP_ATTR_ADD_PROTO_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 15),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                      BT_SDP_DATA_ELEM_LIST(
                          { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0100) },
                          { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(HID_PSM_INTERRUPT) },
                      ),
                    },
                    { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
                      BT_SDP_DATA_ELEM_LIST(
                          { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0011) },
                      ),
                    },
                ),
              },
          ),
        }
    },
    /* Profile Descriptor List: HID v1.11 */
    {
        BT_SDP_ATTR_PROFILE_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 8),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x1124) },
                    { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(0x0111) },
                ),
              },
          ),
        }
    },
    BT_SDP_SERVICE_NAME("DS5Dongle HID"),
};

static struct bt_sdp_record hid_host_rec = {
    .attrs = hid_host_attrs,
    .attr_count = ARRAY_SIZE(hid_host_attrs),
};

/* ---------- bt_enable ready synchronization ---------- */

static volatile bool bt_ready_flag;
static SemaphoreHandle_t bt_ready_sem;
static StaticSemaphore_t bt_ready_sem_buf;

static void bt_ready_cb(int err)
{
    LOG_INF("[BT] bt_ready_cb: err=%d (stack init complete)\n", err);
    bt_ready_flag = (err == 0);
    xSemaphoreGive(bt_ready_sem);
}

/* ---------- public API ---------- */

int bt_hid_host_init(const struct bt_hid_host_config *config)
{
    memset(&hid_ctx, 0, sizeof(hid_ctx));
    feature_cache_clear();
    hid_ctx.input_cb   = config->input_cb;
    hid_ctx.state_cb   = config->state_cb;
    hid_ctx.primer_cb  = config->primer_cb;
    hid_ctx.filter_vid = config->target_vid;
    hid_ctx.filter_pid = config->target_pid;
    hid_ctx.state      = BT_HID_STATE_IDLE;

    net_buf_init(&hid_tx_pool, 20, BT_L2CAP_BUF_SIZE(L2CAP_BR_MTU), NULL);
    net_buf_init(&hid_rx_pool, 4, BT_L2CAP_BUF_SIZE(L2CAP_BR_MTU), NULL);

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&auth_callbacks);

    bt_ready_sem = xSemaphoreCreateBinaryStatic(&bt_ready_sem_buf);

    int err = bt_enable(bt_ready_cb);
    if (err) {
        LOG_ERR("[BT] bt_enable failed: %d\n", err);
        return err;
    }

    LOG_INF("[BT] bt_enable returned, waiting for stack init...\n");
    if (xSemaphoreTake(bt_ready_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        LOG_ERR("[BT] ERROR: stack init timeout!\n");
        return -1;
    }
    if (!bt_ready_flag) {
        LOG_ERR("[BT] ERROR: stack init failed\n");
        return -1;
    }
    LOG_INF("[BT] Bluetooth fully initialized\n");

    /* Generate BD_ADDR from chip eFuse ID (boards often ship without
     * a pre-programmed BT MAC, resulting in 00:00:00:00:00:00).
     * Must be called AFTER bt_enable (HCI transport must be up). */
    {
        bt_addr_le_t cur;
        bt_get_local_public_address(&cur);
        bool all_zero = true;
        for (int i = 0; i < 6; i++)
            if (cur.a.val[i]) { all_zero = false; break; }

        if (all_zero) {
            uint8_t chipid[8];
            bflb_efuse_get_chipid(chipid);
            bt_addr_t bd;
            bd.val[0] = chipid[0];
            bd.val[1] = chipid[1];
            bd.val[2] = chipid[2];
            bd.val[3] = chipid[3];
            bd.val[4] = chipid[4];
            bd.val[5] = chipid[5] | 0xC0;
            int r1 = bt_set_bd_addr(&bd);
            int r2 = bt_set_local_public_address(bd.val);
            LOG_INF("[BT] Set BD_ADDR from chipid: %02X:%02X:%02X:%02X:%02X:%02X "
                   "(hci=%d host=%d)\n",
                   bd.val[5], bd.val[4], bd.val[3],
                   bd.val[2], bd.val[1], bd.val[0], r1, r2);
        }

        bt_get_local_public_address(&cur);
        LOG_INF("[BT] BD_ADDR: %02X:%02X:%02X:%02X:%02X:%02X\n",
               cur.a.val[5], cur.a.val[4], cur.a.val[3],
               cur.a.val[2], cur.a.val[1], cur.a.val[0]);
    }

    blacklist_load();
    bonded_list_load();

    /* Register L2CAP servers so the controller can initiate reconnection
     * (matching DS5Dongle's l2cap_register_service for both PSMs). */
    err = bt_l2cap_br_server_register(&l2cap_hid_ctrl_server);
    if (err)
        LOG_ERR("[BT] Warning: register control server failed: %d\n", err);

    err = bt_l2cap_br_server_register(&l2cap_hid_intr_server);
    if (err)
        LOG_ERR("[BT] Warning: register interrupt server failed: %d\n", err);

    err = bt_sdp_register_service(&hid_host_rec);
    if (err)
        LOG_ERR("[BT] Warning: register SDP HID record failed: %d\n", err);
    else
        LOG_INF("[BT] SDP HID service record registered\n");

    /* Match Pico's aggressive page scan: interval=window=0x0012 (11.25ms)
     * = 100% duty cycle. Ensures near-instant response to controller
     * reconnection attempts, critical for low-battery reconnect success.
     * HCI Write_Page_Scan_Activity: OGF 0x03, OCF 0x001C. */
    {
        struct net_buf *ps_buf = bt_hci_cmd_create(
            BT_OP(BT_OGF_BASEBAND, 0x001c), 4);
        if (ps_buf) {
            net_buf_add_le16(ps_buf, 0x0012);  /* interval: 11.25ms */
            net_buf_add_le16(ps_buf, 0x0012);  /* window:   11.25ms */
            int ps_err = bt_hci_cmd_send_sync(
                BT_OP(BT_OGF_BASEBAND, 0x001c), ps_buf, NULL);
            LOG_INF("[BT] Write Page Scan Activity: interval=0x0012 "
                   "window=0x0012 err=%d\n", ps_err);
        }
    }

    /* Make device discoverable so the controller can find us if needed */
    err = bt_br_set_connectable(true);
    LOG_INF("[BT] set_connectable: %d\n", err);

    err = bt_br_set_discoverable(true);
    LOG_INF("[BT] set_discoverable: %d\n", err);

    /* Diagnostic: check if BR/EDR write_scan_enable succeeded.
     * If both return 0, the controller accepted BR/EDR commands,
     * confirming that BR/EDR HCI path is functional. */
    LOG_INF("[BT] BR/EDR init complete — phone should see \"%s\"\n",
           CONFIG_BT_DEVICE_NAME);

    return 0;
}

int bt_hid_host_scan_start(uint8_t duration_sec)
{
    if (hid_ctx.state != BT_HID_STATE_IDLE || hid_ctx.conn)
        return -1;

    /* Don't scan if an outgoing connect retry is pending —
     * HCI can't do inquiry and create_connection simultaneously */
    if (hid_ctx.connect_pending) {
        LOG_INF("[BT] scan_start suppressed: outgoing connect pending\n");
        return -2;
    }

    set_state(BT_HID_STATE_SCANNING);

    memset(discovery_results, 0, sizeof(discovery_results));

    struct bt_br_discovery_param param = {
        .length     = duration_sec ? duration_sec : BT_INQUIRY_LEN,
        .limited    = false,
    };

    int err = bt_br_discovery_start(&param, discovery_results,
                                     BT_MAX_DISCOVERED,
                                     discovery_complete);
    if (err) {
        LOG_ERR("[BT] Discovery start failed: %d\n", err);
        set_state(BT_HID_STATE_IDLE);
        return err;
    }

    LOG_INF("[BT] Scanning for devices...\n");
    return 0;
}

void bt_hid_host_scan_stop(void)
{
    bt_br_discovery_stop();
    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        set_state(BT_HID_STATE_IDLE);
}

bool bt_hid_host_poll_scan_early(void)
{
    if (hid_ctx.state != BT_HID_STATE_SCANNING)
        return false;

    for (int i = 0; i < BT_MAX_DISCOVERED; i++) {
        static const bt_addr_t zero = {{0}};
        if (bt_addr_cmp(&discovery_results[i].addr, &zero) == 0)
            break;

        uint32_t cod = discovery_results[i].cod[0]
                     | ((uint32_t)discovery_results[i].cod[1] << 8)
                     | ((uint32_t)discovery_results[i].cod[2] << 16);
        uint8_t major = (cod >> 8) & 0x1F;
        uint8_t minor = (cod >> 2) & 0x3F;
        bool is_gamepad = (major == 0x05) && (minor & 0x02);

        if (is_gamepad || (hid_ctx.has_target &&
            bt_addr_cmp(&discovery_results[i].addr, &hid_ctx.target_addr) == 0)) {

            if (hid_ctx.has_target &&
                bt_addr_cmp(&discovery_results[i].addr, &hid_ctx.target_addr) != 0)
                continue;

            LOG_INF("[BT] Early termination: gamepad found during inquiry\n");
            bt_br_discovery_stop();
            bt_addr_copy(&hid_ctx.pending_addr, &discovery_results[i].addr);
            hid_ctx.connect_pending = true;
            hid_ctx.connect_pending_tick = 0;
            set_state(BT_HID_STATE_CONNECTING);
            return true;
        }
    }
    return false;
}

bool bt_hid_host_poll_connect(void)
{
    if (!hid_ctx.connect_pending)
        return false;

    /* If controller already reconnected (incoming), cancel outgoing */
    if (hid_ctx.conn != NULL && hid_ctx.state != BT_HID_STATE_IDLE) {
        LOG_INF("[BT] poll_connect: cancelled (controller already connected, "
               "state=%d)\n", hid_ctx.state);
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
        return false;
    }

    /* connect_pending_tick == UINT32_MAX means "waiting for ACL teardown" */
    if (hid_ctx.connect_pending_tick == (TickType_t)UINT32_MAX)
        return false;

    /* After ACL teardown event fires, wait before connecting */
    if (hid_ctx.connect_pending_tick != 0) {
        TickType_t elapsed = xTaskGetTickCount() - hid_ctx.connect_pending_tick;
        if (elapsed < pdMS_TO_TICKS(500))
            return false;
    }

    /* Cancel if another connect path is active (not from our deferred flow).
     * States IDLE, SCANNING, and CONNECTING are valid — early termination and
     * discovery_complete set CONNECTING before deferring to poll_connect. */
    if (hid_ctx.state != BT_HID_STATE_IDLE &&
        hid_ctx.state != BT_HID_STATE_SCANNING &&
        hid_ctx.state != BT_HID_STATE_CONNECTING) {
        LOG_INF("[BT] poll_connect: cancelled (state=%d, not idle)\n",
               hid_ctx.state);
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
        return false;
    }

    /* If scanning is in progress, cancel it first (HCI can't do both) */
    if (hid_ctx.state == BT_HID_STATE_SCANNING) {
        LOG_INF("[BT] poll_connect: cancelling active scan first\n");
        bt_br_discovery_stop();
        set_state(BT_HID_STATE_IDLE);
    }

    LOG_INF("[BT] Fallback: attempting outgoing connect (state=%d conn=%p)\n",
           hid_ctx.state, hid_ctx.conn);
    int rc = bt_hid_host_connect(hid_ctx.pending_addr.val);
    if (rc != 0) {
        static uint8_t retry_count = 0;
        retry_count++;
        if (retry_count >= 3) {
            LOG_WRN("[BT] Fallback: gave up after %d attempts\n", retry_count);
            retry_count = 0;
            hid_ctx.connect_pending = false;
            hid_ctx.connect_pending_tick = 0;
        } else {
            LOG_WRN("[BT] Fallback: attempt %d failed (%d), retry in 500ms\n",
                   retry_count, rc);
            hid_ctx.connect_pending_tick = xTaskGetTickCount();
        }
    } else {
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
    }
    return true;
}

bool bt_hid_host_poll_security(void)
{
    if (!hid_ctx.security_pending || !hid_ctx.conn)
        return false;

    hid_ctx.security_pending = false;

    struct bt_conn *conn = hid_ctx.conn;

    LOG_DBG("[BT] poll_security: conn=%p handle=0x%04x state=%s "
           "sec_level=%d required_sec=%d encrypt=%d outgoing=%d\n",
           conn, conn->handle, state_name(hid_ctx.state),
           conn->sec_level, conn->required_sec_level, conn->encrypt,
           hid_ctx.outgoing);

    /* Root cause fix: conn_new() doesn't clear bt_conn, so
     * required_sec_level retains the stale value from a previous
     * connection.  Reset it so bt_conn_set_security() actually
     * initiates authentication. */
    if (conn->required_sec_level >= BT_SECURITY_L2) {
        LOG_DBG("[BT] poll_security: resetting stale required_sec_level "
               "%d -> L1\n", conn->required_sec_level);
        conn->required_sec_level = BT_SECURITY_L1;
    }

    int err = bt_conn_set_security(conn, BT_SECURITY_L2);
    LOG_DBG("[BT] poll_security: bt_conn_set_security(L2) = %d "
           "(now sec=%d req=%d)\n",
           err, conn->sec_level, conn->required_sec_level);

    if (err == -EBUSY) {
        LOG_DBG("[BT] poll_security: pairing already in progress, waiting\n");
    } else if (err && err != -EALREADY) {
        LOG_DBG("[BT] poll_security: set_security failed (%d), "
               "trying HCI Auth_Requested directly\n", err);
        struct net_buf *buf = bt_hci_cmd_create(
            BT_HCI_OP_AUTH_REQUESTED, 2);
        if (buf) {
            net_buf_add_le16(buf, conn->handle);
            int hci_err = bt_hci_cmd_send_sync(
                BT_HCI_OP_AUTH_REQUESTED, buf, NULL);
            LOG_DBG("[BT] poll_security: HCI Auth_Requested = %d\n", hci_err);
        }
    }

    return true;
}

bool bt_hid_host_poll_sdp(void)
{
    if (!hid_ctx.sdp_pending || !hid_ctx.conn)
        return false;

    hid_ctx.sdp_pending = false;

    if (hid_ctx.state != BT_HID_STATE_CONNECTING) {
        LOG_DBG("[BT] poll_sdp: state already advanced (%s), skipping\n",
               state_name(hid_ctx.state));
        return false;
    }

    /* Skip SDP for bonded devices — directly create L2CAP channels.
     * This matches DS5Dongle behavior where reconnection never does SDP. */
    LOG_DBG("[BT] poll_sdp: creating outgoing L2CAP channels\n");

    set_state(BT_HID_STATE_L2CAP_CONTROL);

    memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
    memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));

    hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
    hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
    hid_ctx.intr_chan.chan.ops = &intr_ops;
    hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;

    int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                    &hid_ctx.ctrl_chan.chan,
                                    HID_PSM_CONTROL);
    if (err) {
        LOG_DBG("[BT] poll_sdp: L2CAP control connect failed: %d\n", err);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    return true;
}

#define SECURITY_TIMEOUT_TICKS  pdMS_TO_TICKS(8000)

bool bt_hid_host_poll_security_watchdog(void)
{
    if (hid_ctx.security_done || !hid_ctx.conn)
        return false;

    if (hid_ctx.state != BT_HID_STATE_CONNECTING)
        return false;

    TickType_t elapsed = xTaskGetTickCount() - hid_ctx.acl_connect_tick;
    if (elapsed < SECURITY_TIMEOUT_TICKS)
        return false;

    struct bt_conn *conn = hid_ctx.conn;
    LOG_WRN("[BT] WATCHDOG: security not established after %lu ms! "
           "sec_level=%d required_sec=%d encrypt=%d\n",
           (unsigned long)(elapsed * portTICK_PERIOD_MS),
           conn->sec_level, conn->required_sec_level, conn->encrypt);

    if (conn->encrypt && conn->sec_level >= BT_SECURITY_L2) {
        LOG_WRN("[BT] WATCHDOG: encryption active, forcing L2CAP path\n");
        hid_ctx.security_done = true;
        if (hid_ctx.outgoing) {
            hid_ctx.sdp_pending = true;
        }
        return true;
    }

    /* Auth failed: required_sec_level was reset by reset_pairing() in the
     * SDK, meaning the controller rejected our link key.  Delete the stale
     * key so the next connection attempt triggers fresh SSP pairing. */
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote) {
        LOG_WRN("[BT] WATCHDOG: clearing stale link key for "
               "%02X:%02X:%02X:%02X:%02X:%02X\n",
               remote->val[5], remote->val[4], remote->val[3],
               remote->val[2], remote->val[1], remote->val[0]);
        extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
        bt_keys_link_key_clear_addr(remote);
    }

    LOG_WRN("[BT] WATCHDOG: disconnecting to retry with fresh pairing\n");
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return false;
}

#define INCOMING_L2CAP_FALLBACK_MS 3000

bool bt_hid_host_poll_incoming_l2cap_fallback(void)
{
    if (hid_ctx.state != BT_HID_STATE_CONNECTING)
        return false;
    if (!hid_ctx.security_done || hid_ctx.outgoing || !hid_ctx.conn)
        return false;

    TickType_t elapsed = xTaskGetTickCount() - hid_ctx.security_done_tick;
    if (elapsed < pdMS_TO_TICKS(INCOMING_L2CAP_FALLBACK_MS))
        return false;

    /* Control L2CAP handshake stuck (accept fired but connected never came) */
    if (hid_ctx.ctrl_accepting && !hid_ctx.ctrl_connected) {
        LOG_INF("[BT] L2CAP fallback: Control handshake stuck, disconnecting "
               "(ctrl_accept=%d ctrl_conn=%d intr_conn=%d)\n",
               hid_ctx.ctrl_accepting, hid_ctx.ctrl_connected, hid_ctx.intr_connected);
        /* Explicitly disconnect the stuck L2CAP channel to cancel RTX timer
         * before tearing down the ACL — speeds up ACL teardown significantly */
        bt_l2cap_chan_disconnect(&hid_ctx.ctrl_chan.chan);
        hid_ctx.fallback_retry = true;
        LOG_INF("[BT] fallback_retry SET=1 (stuck path)\n");
        bt_hid_host_disconnect();
        return true;
    }

    if (hid_ctx.ctrl_connected) {
        /* Control is up but Interrupt isn't — open Interrupt only */
        if (!hid_ctx.intr_connected && !hid_ctx.intr_accepting) {
            LOG_INF("[BT] L2CAP fallback: Control connected, opening Interrupt\n");
            hid_ctx.outgoing = true;
            set_state(BT_HID_STATE_L2CAP_INTERRUPT);
            memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));
            hid_ctx.intr_chan.chan.ops = &intr_ops;
            hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;
            int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                            &hid_ctx.intr_chan.chan,
                                            HID_PSM_INTERRUPT);
            if (err)
                LOG_ERR("[BT] Interrupt connect failed: %d\n", err);
            return true;
        }
        return false;
    }

    /* Neither Control connecting nor connected — controller didn't open L2CAP.
     * Controller's SDP query may have closed before receiving our response.
     * Set fallback_retry to actively page controller and open L2CAP from host side
     * (standard HID Host behavior, same as Pico2W DS5Dongle). */
    LOG_WRN("[BT] L2CAP fallback: no activity in %dms, retry outgoing\n",
            (int)(elapsed * portTICK_PERIOD_MS));
    hid_ctx.fallback_retry = true;
    bt_hid_host_disconnect();
    return true;
}

int bt_hid_host_connect(const uint8_t *addr)
{
    bt_addr_t bt_addr;
    memcpy(bt_addr.val, addr, 6);

    LOG_DBG("[BT] connect ENTER addr=%02X:%02X:%02X:%02X:%02X:%02X state=%d conn=%p\n",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
           hid_ctx.state, hid_ctx.conn);

    if (hid_ctx.state == BT_HID_STATE_SCANNING) {
        LOG_DBG("[BT] connect [1] stopping scan...\n");
        int stop_err = bt_br_discovery_stop();
        LOG_DBG("[BT] connect [2] scan stop rc=%d\n", stop_err);
    }

    if (hid_ctx.conn) {
        LOG_DBG("[BT] connect: already have conn=%p, skip create\n", hid_ctx.conn);
        hid_ctx.outgoing = true;
        set_state(BT_HID_STATE_CONNECTING);
        return 0;
    }

    hid_ctx.outgoing = true;
    set_state(BT_HID_STATE_CONNECTING);
    LOG_DBG("[BT] connect [3] state=CONNECTING, outgoing=true\n");

    /* Clear link key only for devices NOT in our bonded list (fresh discovery).
     * For bonded devices (fallback retry / reconnection), keep the key. */
    {
        bool is_bonded = false;
        for (uint8_t i = 0; i < bonded_count; i++) {
            if (bt_addr_cmp(&bonded_list[i], &bt_addr) == 0) {
                is_bonded = true;
                break;
            }
        }
        if (!is_bonded) {
            extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
            bt_keys_link_key_clear_addr(&bt_addr);
            LOG_INF("[BT] Cleared stale link key for outgoing connection "
                   "(fresh SSP)\n");
        }
    }

    LOG_DBG("[BT] connect [4] bt_conn_create_br...\n");
    /* Disable page scan before paging — BL616 HCI controller can't do both */
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);
    struct bt_conn *conn = bt_conn_create_br(&bt_addr,
                                              BT_BR_CONN_PARAM_DEFAULT);
    LOG_DBG("[BT] connect [5] returned %p\n", conn);
    if (!conn) {
        LOG_WRN("[BT] bt_conn_create_br failed — re-enabling page scan\n");
        hid_ctx.outgoing = false;
        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);
        set_state(BT_HID_STATE_IDLE);
        return -1;
    }

    /* Bouffalo SDK's bt_conn_create_br does NOT return a caller
     * reference (unlike standard Zephyr). SDK examples never call
     * bt_conn_unref after bt_conn_create_br. */
    LOG_DBG("[BT] connect EXIT OK\n");
    return 0;
}

void bt_hid_host_disconnect(void)
{
    LOG_INF("[BT] bt_hid_host_disconnect called (state=%d fallback_retry=%d)\n",
           hid_ctx.state, hid_ctx.fallback_retry);
    if (hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn,
                           BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

void bt_hid_host_force_disconnect(void)
{
    LOG_WRN("[BT] Force disconnect — HID cleanup, conn=%p\n", hid_ctx.conn);

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.connect_pending = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;

    first_intr_logged  = false;
    first_output_logged = false;

    app_tx_fifo_reset();

    feature_cache_clear();
    dse_reset();

    if (hid_ctx.conn) {
        LOG_INF("[BT] Sending HCI Disconnect for ACL cleanup\n");
        bt_conn_disconnect(hid_ctx.conn, 0x13);
        bt_conn_unref(hid_ctx.conn);
        hid_ctx.conn = NULL;
    }

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    set_state(BT_HID_STATE_IDLE);

    LOG_INF("[BT] force_disconnect: fallback_retry=%d bonded_count=%d\n",
           hid_ctx.fallback_retry, bonded_count);
    if (hid_ctx.fallback_retry && bonded_count > 0) {
        hid_ctx.fallback_retry = false;
        memcpy(hid_ctx.pending_addr.val, bonded_list[active_idx].val, 6);
        hid_ctx.connect_pending = true;
        hid_ctx.connect_pending_tick = (TickType_t)UINT32_MAX;
        LOG_INF("[BT] Fallback: outgoing connect pending (waiting for ACL teardown)\n");
    }
}

bool bt_hid_host_has_pending_conn(void)
{
    return hid_ctx.conn != NULL;
}

void bt_hid_host_drop_stale_conn(void)
{
    if (!hid_ctx.conn)
        return;

    LOG_WRN("[BT] conn_disconnected never arrived — force cleanup\n");

    bt_conn_unref(hid_ctx.conn);
    hid_ctx.conn = NULL;

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.connect_pending = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;

    app_tx_fifo_reset();

    feature_cache_clear();

    set_state(BT_HID_STATE_IDLE);

    if (switch_pending) {
        switch_pending = false;
        LOG_INF("[BT] Switch: stale conn cleared, waiting for "
               "controller [%d/%d] to reconnect\n", active_idx, bonded_count);
    }

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);
    LOG_INF("[BT] Re-enabled connectable+discoverable after stale cleanup\n");
}

int bt_hid_host_read_rssi(int8_t *rssi)
{
    if (!hid_ctx.conn || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return -1;
    int8_t val = 0;
    int ret = bt_le_read_rssi(hid_ctx.conn->handle, &val);
    if (ret == 0) {
        cached_rssi = val;
        if (rssi) *rssi = val;
    } else {
        LOG_WRN("[BT] Read RSSI failed: %d\n", ret);
    }
    return ret;
}

int8_t bt_hid_host_get_cached_rssi(void)
{
    return cached_rssi;
}

int bt_hid_host_send_output(const uint8_t *data, uint16_t len)
{
    if (!hid_ctx.intr_connected || !hid_ctx.conn)
        return -1;

    /* Route by report type:
     *   0x31 game output (78 B)  → 10-deep FIFO with CAN_SEND_NOW flow control
     *   0x32 init (142 B)        → direct send (large, infrequent, bypasses FIFO)
     *   0x39 audio (547 B)       → direct send (too large for FIFO slot, high rate)
     *
     * This matches DS5Dongle which routes only game output through send_fifo;
     * audio haptics and init go via separate direct l2cap_send calls. */
    bool is_game_output = (len <= APP_TX_DATA_SZ - 1) &&
                          (len == DS5_BT_OUTPUT_REPORT_SIZE || data[0] == DS5_BT_OUTPUT_REPORT_ID);

    if (!is_game_output) {
        /* Direct path: allocate and send immediately */
        struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
        if (!buf) {
            LOG_WRN("[HID] send_output direct: pool empty (len=%d)\n", (int)len);
            return -1;
        }
        net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
        net_buf_add_u8(buf, HID_HEADER(HID_TRANS_DATA, HID_REPORT_OUTPUT));
        net_buf_add_mem(buf, data, len);
        int err = bt_l2cap_chan_send(&hid_ctx.intr_chan.chan, buf);
        if (err < 0) {
            net_buf_unref(buf);
            LOG_ERR("[HID] send_output direct: l2cap err=%d\n", err);
            return err;
        }
        return 0;
    }

    /* FIFO path for 0x31 game output (equivalent to DS5Dongle's send_fifo) */
    uint8_t next_tail = (app_tx_tail + 1) % APP_TX_DEPTH;

    /* FIFO full: drop oldest frame to make room (same as DS5Dongle) */
    if (next_tail == app_tx_head) {
        app_tx_head = (app_tx_head + 1) % APP_TX_DEPTH;
    }

    uint8_t slot = app_tx_tail;
    app_tx_buf[slot][0] = HID_HEADER(HID_TRANS_DATA, HID_REPORT_OUTPUT);
    memcpy(app_tx_buf[slot] + 1, data, len);
    app_tx_len[slot] = len + 1;
    app_tx_tail = next_tail;

    if (!first_output_logged) {
        first_output_logged = true;
        LOG_INF("[HID] First output report enqueued, len=%d\n", (int)len);
    }

    /* Trigger drain if channel is idle (CAN_SEND_NOW equivalent) */
    if (!app_tx_busy) {
        hid_tx_drain_one();
    }

    return 0;
}

int bt_hid_host_set_feature(uint8_t report_id, const uint8_t *data,
                             uint16_t len)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn)
        return -1;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf)
        return -1;

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_SET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);
    if (data && len > 0)
        net_buf_add_mem(buf, data, len);

    int err = bt_l2cap_chan_send(&hid_ctx.ctrl_chan.chan, buf);
    if (err < 0) {
        net_buf_unref(buf);
        return err;
    }

    return 0;
}

int bt_hid_host_get_feature(uint8_t report_id)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn)
        return -1;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf)
        return -1;

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_GET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);

    int err = bt_l2cap_chan_send(&hid_ctx.ctrl_chan.chan, buf);
    if (err < 0) {
        net_buf_unref(buf);
        return err;
    }

    LOG_DBG("[HID] GET_REPORT(Feature, 0x%02x) sent\n", report_id);
    return 0;
}

enum bt_hid_host_state bt_hid_host_get_state(void)
{
    return hid_ctx.state;
}

bool bt_hid_host_get_connected_addr(uint8_t *addr)
{
    if (!hid_ctx.conn)
        return false;

    const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
    if (!remote)
        return false;

    memcpy(addr, remote->val, 6);
    return true;
}

/* ---------- bonding persistence ---------- */

int bt_hid_host_try_reconnect(void)
{
    if (hid_ctx.state != BT_HID_STATE_IDLE)
        return -1;

    if (bonded_count == 0) {
        LOG_INF("[BT] No bonded BR/EDR device found\n");
        return -1;
    }

    if (active_idx >= bonded_count)
        active_idx = 0;

    bt_addr_t *addr = &bonded_list[active_idx];
    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, addr_str, sizeof(addr_str));

    bt_addr_copy(&hid_ctx.target_addr, addr);
    hid_ctx.has_target = true;

    /* DS5Dongle approach: don't actively page the controller.
     * Stay connectable and let the controller reconnect to us.
     * The controller will create ACL + L2CAP channels. */
    LOG_INF("[BT] Bonded device [%d/%d] %s — waiting for controller to reconnect "
           "(page scan enabled)\n", active_idx, bonded_count, addr_str);
    return 0;
}

int bt_hid_host_switch_next(void)
{
    if (bonded_count <= 1) {
        LOG_INF("[BT] switch_next: only %d bonded, nothing to switch\n",
               bonded_count);
        return -1;
    }

    active_idx = (active_idx + 1) % bonded_count;
    bonded_list_persist_idx();

    bt_addr_t *addr = &bonded_list[active_idx];
    bt_addr_copy(&hid_ctx.target_addr, addr);
    hid_ctx.has_target = true;

    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, addr_str, sizeof(addr_str));

    if (hid_ctx.conn) {
        /* Block further BT sends immediately */
        hid_ctx.ctrl_connected = false;
        hid_ctx.intr_connected = false;
        switch_pending = true;

        LOG_INF("[BT] Switching: disconnect current → wait ACL teardown → "
               "reconnect [%d/%d] %s\n", active_idx, bonded_count, addr_str);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        /* conn_disconnected callback will handle full cleanup and
         * enable connectable only after old ACL is fully torn down. */
    } else {
        if (hid_ctx.state == BT_HID_STATE_SCANNING)
            bt_br_discovery_stop();

        hid_ctx.ctrl_accepting = false;
        hid_ctx.intr_accepting = false;
        hid_ctx.outgoing = false;
        hid_ctx.is_dse = false;
        hid_ctx.check_dse = false;
        hid_ctx.security_pending = false;
        hid_ctx.security_done = false;
        hid_ctx.sdp_pending = false;
        hid_ctx.connect_pending = false;
        hid_ctx.handshake_next  = 0;
        hid_ctx.handshake_count = 0;
        hid_ctx.handshake_send_failed = false;

        app_tx_fifo_reset();
        feature_cache_clear();
        dse_reset();
        cached_rssi = 1;

        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);

        LOG_INF("[BT] Switched to controller [%d/%d] %s — waiting for reconnect\n",
               active_idx, bonded_count, addr_str);
        set_state(BT_HID_STATE_IDLE);
    }

    return (int)active_idx;
}

bool bt_hid_host_is_switching(void)
{
    return switch_pending;
}

uint8_t bt_hid_host_get_bonded_count(void)
{
    return bonded_count;
}

uint8_t bt_hid_host_get_active_idx(void)
{
    return active_idx;
}

void bt_hid_host_clear_bonds(void)
{
    LOG_INF("[BT] Clearing all bonding info\n");

    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        bt_br_discovery_stop();

    /* Disconnect the current controller if connected */
    if (hid_ctx.conn) {
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(hid_ctx.conn);
        hid_ctx.conn = NULL;
    }

    /* Clear the blacklist too — we want the controller to reconnect freely
     * after a manual bond clear. The security watchdog handles stale keys. */
    blacklist_count = 0;

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.connect_pending = false;

    bt_keys_link_key_clear_addr(NULL);

    blacklist_persist();
    blacklist_dirty = false;
    LOG_INF("[BT] All link keys and blacklist cleared\n");

    bonded_count = 0;
    active_idx = 0;
    bonded_list_persist_idx();

    hid_ctx.has_target = false;
    memset(&hid_ctx.target_addr, 0, sizeof(hid_ctx.target_addr));

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    feature_cache_clear();
    dse_reset();

    set_state(BT_HID_STATE_IDLE);
}

bool bt_hid_host_get_cached_feature(uint8_t report_id,
                                    const uint8_t **data, uint16_t *len)
{
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].report_id == report_id &&
            feature_cache[i].len > 0) {
            *data = feature_cache[i].data;
            *len  = feature_cache[i].len;
            return true;
        }
    }
    return false;
}

bool bt_hid_host_is_dse(void)
{
    return hid_ctx.is_dse;
}

int bt_hid_host_set_feature_crc(uint8_t report_id, const uint8_t *data,
                                uint16_t len)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn) {
        LOG_WRN("[HID] SET_FEATURE_CRC(0x%02x): not connected (ctrl=%d conn=%p)\n",
               report_id, hid_ctx.ctrl_connected, hid_ctx.conn);
        return -1;
    }

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        LOG_ERR("[HID] SET_FEATURE_CRC(0x%02x): net_buf alloc failed\n", report_id);
        return -1;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_SET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);
    if (data && len > 0)
        net_buf_add_mem(buf, data, len);

    /* Overwrite last 4 bytes of data with CRC (matching DS5Dongle's
     * fill_feature_report_checksum). The HID descriptor's Report Count
     * includes 4 bytes of CRC space that the USB host sends as zeros. */
    if (len >= 4) {
        uint8_t *crc_region = buf->data + 1;
        uint16_t crc_data_len = 1 + (len - 4);
        uint32_t crc = ds5_crc32(DS5_BT_FEATURE_CRC_SEED,
                                  crc_region, crc_data_len);
        ds5_write_le32(crc_region + crc_data_len, crc);
    }

    int err = bt_l2cap_chan_send(&hid_ctx.ctrl_chan.chan, buf);
    if (err < 0) {
        LOG_ERR("[HID] SET_FEATURE_CRC(0x%02x): l2cap send failed err=%d\n",
               report_id, err);
        net_buf_unref(buf);
        return err;
    }

    LOG_INF("[HID] SET_REPORT(Feature, 0x%02x) sent with CRC, len=%d\n",
           report_id, (int)(len + 2));
    return 0;
}

void bt_hid_host_persist_if_dirty(void)
{
    if (blacklist_dirty) {
        blacklist_dirty = false;
        blacklist_persist();
    }
}

void bt_hid_host_handshake_tick(void)
{
    if (hid_ctx.state == BT_HID_STATE_HANDSHAKE &&
        hid_ctx.handshake_send_failed) {
        handshake_send_next();
    }
}

void bt_hid_host_radio_idle(void)
{
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);
    LOG_INF("[BT] Radio idle — page/inquiry scan disabled\n");
}

void bt_hid_host_radio_wake(void)
{
    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);
    LOG_INF("[BT] Radio wake — page/inquiry scan re-enabled\n");
}
