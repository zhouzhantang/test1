/* hci_core.c - HCI core Bluetooth handling */

/*
 * Copyright (c) 2017 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 */

#include <zephyr.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <atomic.h>
#include <misc/util.h>
#include <misc/slist.h>
#include <misc/byteorder.h>
#include <misc/stack.h>
#include <misc/__assert.h>

#include <bluetooth.h>
#include <conn.h>
#include <l2cap.h>
#include <hci.h>
#include <hci_api.h>
#include <hci_vs.h>
#include <hci_driver.h>
#include <storage.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_CORE)
#include "common/log.h"

#include "rpa.h"
#include "keys.h"
#include "monitor.h"
#include "hci_core.h"
#include "hci_ecc.h"
#include "ecc.h"

#include "conn_internal.h"
#include "l2cap_internal.h"
#include "smp.h"
#include "crypto.h"

/* Peripheral timeout to initialize Connection Parameter Update procedure */
#define CONN_UPDATE_TIMEOUT K_SECONDS(5)
#define RPA_TIMEOUT K_SECONDS(CONFIG_BT_RPA_TIMEOUT)

#define HCI_CMD_TIMEOUT K_SECONDS(10)

/* Stacks for the threads */
#if defined(CONFIG_BT_HOST_RX_THREAD)
static struct k_thread rx_thread_data;
static BT_STACK_NOINIT(rx_thread_stack, CONFIG_BT_HCI_RX_STACK_SIZE);
#endif
static struct k_thread tx_thread_data;
static BT_STACK_NOINIT(tx_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);

static void init_work(struct k_work *work);

struct bt_dev bt_dev = {
    .init = _K_WORK_INITIALIZER(init_work),
    .cmd_tx_queue = K_FIFO_INITIALIZER(bt_dev.cmd_tx_queue),
#if defined(CONFIG_BT_HOST_RX_THREAD)
    .rx_queue = K_FIFO_INITIALIZER(bt_dev.rx_queue),
#endif
};

static bt_ready_cb_t ready_cb;

const struct bt_storage *bt_storage;

static bt_le_scan_cb_t *scan_dev_found_cb;

static u8_t pub_key[64];
static struct bt_pub_key_cb *pub_key_cb;
static bt_dh_key_cb_t dh_key_cb;

#ifdef CONFIG_CONTROLLER_IN_ONE_TASK
struct k_poll_signal g_pkt_recv = K_POLL_SIGNAL_INITIALIZER(g_pkt_recv);
#endif

#define SYNC_TX 1
#define SYNC_TX_DONE 2

struct cmd_data {
    /** BT_BUF_CMD */
    u8_t type;

    /** HCI status of the command completion */
    u8_t status;

    /** The command OpCode that the buffer contains */
    u16_t opcode;

    uint8_t sync;
};

struct acl_data
{
    /** BT_BUF_ACL_IN */
    u8_t type;

    /* Index into the bt_conn storage array */
    u8_t id;

    /** ACL connection handle */
    u16_t handle;
};

#define cmd(buf) ((struct cmd_data *)net_buf_user_data(buf))
#define acl(buf) ((struct acl_data *)net_buf_user_data(buf))

/* HCI command buffers. Derive the needed size from BT_BUF_RX_SIZE since
 * the same buffer is also used for the response.
 */
#define CMD_BUF_SIZE BT_BUF_RX_SIZE
NET_BUF_POOL_DEFINE(hci_cmd_pool, CONFIG_BT_HCI_CMD_COUNT,
                    CMD_BUF_SIZE, BT_BUF_USER_DATA_MIN, NULL);

NET_BUF_POOL_DEFINE(hci_rx_pool, CONFIG_BT_RX_BUF_COUNT,
                    BT_BUF_RX_SIZE, BT_BUF_USER_DATA_MIN, NULL);

static void send_cmd(struct net_buf *buf);

#if defined(CONFIG_BT_HCI_ACL_FLOW_CONTROL)
static void report_completed_packet(struct net_buf *buf)
{

    struct bt_hci_cp_host_num_completed_packets *cp;
    u16_t                                        handle = acl(buf)->handle;
    struct bt_hci_handle_count *                 hc;
    struct bt_conn *                             conn;

    net_buf_destroy(buf);

    /* Do nothing if controller to host flow control is not supported */
    if (!(bt_dev.supported_commands[10] & 0x20)) {
        return;
    }

    conn = bt_conn_lookup_id(acl(buf)->id);
    if (!conn) {
        BT_WARN("Unable to look up conn with id 0x%02x", acl(buf)->id);
        return;
    }

    if (conn->state != BT_CONN_CONNECTED && conn->state != BT_CONN_DISCONNECT) {
        BT_WARN("Not reporting packet for non-connected conn");
        bt_conn_unref(conn);
        return;
    }

    bt_conn_unref(conn);

    BT_DBG("Reporting completed packet for handle %u", handle);
#ifdef CONFIG_BT_USING_HCI_API
    int err;
    struct bt_hci_handle_count hc_p;
    hc_p.count = 1;
    hc_p.handle = handle;

    err = hci_api_host_num_complete_packets(1, &hc_p);
    if (err) {
        return err;
    }
    return 0;
#else
    buf = bt_hci_cmd_create(BT_HCI_OP_HOST_NUM_COMPLETED_PACKETS, sizeof(*cp) + sizeof(*hc));
    if (!buf) {
        BT_ERR("Unable to allocate new HCI command");
        return;
    }

    cp              = net_buf_add(buf, sizeof(*cp));
    cp->num_handles = sys_cpu_to_le16(1);

    hc         = net_buf_add(buf, sizeof(*hc));
    hc->handle = sys_cpu_to_le16(handle);
    hc->count  = sys_cpu_to_le16(1);

    bt_hci_cmd_send(BT_HCI_OP_HOST_NUM_COMPLETED_PACKETS, buf);
#endif
}

#define ACL_IN_SIZE BT_L2CAP_BUF_SIZE(CONFIG_BT_L2CAP_RX_MTU)
NET_BUF_POOL_DEFINE(acl_in_pool, CONFIG_BT_ACL_RX_COUNT, ACL_IN_SIZE,
                    BT_BUF_USER_DATA_MIN, report_completed_packet);
#endif /* CONFIG_BT_HCI_ACL_FLOW_CONTROL */

struct net_buf *bt_hci_cmd_create(u16_t opcode, u8_t param_len)
{
    struct bt_hci_cmd_hdr *hdr;
    struct net_buf *buf = NULL;

    BT_DBG("opcode 0x%04x param_len %u", opcode, param_len);

    buf = net_buf_alloc(&hci_cmd_pool, K_FOREVER);
    if (buf) {
        BT_DBG("buf %p", buf);

        net_buf_reserve(buf, CONFIG_BT_HCI_RESERVE);
        cmd(buf)->type = BT_BUF_CMD;
        cmd(buf)->opcode = opcode;
        cmd(buf)->sync = 0;

        hdr = net_buf_add(buf, sizeof(*hdr));
        hdr->opcode = sys_cpu_to_le16(opcode);
        hdr->param_len = param_len;
    }

    return buf;
}

int bt_hci_cmd_send(u16_t opcode, struct net_buf *buf)
{
    if (!buf) {
        buf = bt_hci_cmd_create(opcode, 0);
        if (!buf) {
            return -ENOBUFS;
        }
    }

    BT_DBG("%s, opcode 0x%04x len %u", __func__, opcode, buf->len);

    /* Host Number of Completed Packets can ignore the ncmd value
     * and does not generate any cmd complete/status events.
     */
    if (opcode == BT_HCI_OP_HOST_NUM_COMPLETED_PACKETS) {
        int err;

        err = bt_send(buf);
        if (err) {
            BT_ERR("Unable to send to driver (err %d)", err);
            net_buf_unref(buf);
        }

        return err;
    }

    net_buf_put(&bt_dev.cmd_tx_queue, buf);
    return 0;
}

void process_events(struct k_poll_event *ev, int count);
int bt_hci_cmd_send_sync(u16_t opcode, struct net_buf *buf, struct net_buf **rsp)
{
    int err = 0;
    uint32_t time_start;

    if (!buf) {
        buf = bt_hci_cmd_create(opcode, 0);
        if (!buf) {
            err = -ENOBUFS;
            goto exit;
        }
    }

    BT_DBG("%s, buf %p opcode 0x%04x len %u", __func__, buf, opcode, buf->len);

    cmd(buf)->sync = SYNC_TX;
    net_buf_ref(buf);

    net_buf_put(&bt_dev.cmd_tx_queue, buf);

    time_start = k_uptime_get_32();

    while (1) {
        static struct k_poll_event events[1] = {
            K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_FIFO_DATA_AVAILABLE,
                                            K_POLL_MODE_NOTIFY_ONLY,
                                            &bt_dev.cmd_tx_queue, BT_EVENT_CMD_TX),
        };
        events[0].state = K_POLL_STATE_NOT_READY;
        if (!k_queue_is_empty((struct k_queue *)&bt_dev.cmd_tx_queue)) {
            events[0].state = K_POLL_STATE_FIFO_DATA_AVAILABLE;
        }

        process_events(events, 1);
        if ((cmd(buf)->sync == SYNC_TX_DONE) ||
            (k_uptime_get_32() - time_start) >= HCI_CMD_TIMEOUT) {
            break;
        } else {
            k_sleep(1);
        }
    }

    BT_DBG("opcode 0x%04x status 0x%02x", opcode, cmd(buf)->status);

    if (cmd(buf)->status) {
        err = -EIO;
        net_buf_unref(buf);
    } else {
        err = 0;
        if (rsp) {
            *rsp = buf;
        } else {
            net_buf_unref(buf);
        }
    }

exit:
    return err;
}

static const bt_addr_le_t *find_id_addr(const bt_addr_le_t *addr)
{
    if (IS_ENABLED(CONFIG_BT_SMP)) {
        struct bt_keys *keys;

        keys = bt_keys_find_irk(addr);
        if (keys) {
            BT_DBG("Identity %s matched RPA %s", bt_addr_le_str(&keys->addr),
                   bt_addr_le_str(addr));
            return &keys->addr;
        }
    }

    return addr;
}

static int set_advertise_enable(bool enable)
{
    int             err;

#ifdef CONFIG_BT_USING_HCI_API
    u8_t adv_enable;
    if (enable) {
        adv_enable = BT_HCI_LE_ADV_ENABLE;
    } else {
        adv_enable = BT_HCI_LE_ADV_DISABLE;
    }
    err = hci_api_le_adv_enable(adv_enable);
#else
    struct net_buf *buf;
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_ADV_ENABLE, 1);
    if (!buf) {
        return -ENOBUFS;
    }

    if (enable) {
        net_buf_add_u8(buf, BT_HCI_LE_ADV_ENABLE);
    } else {
        net_buf_add_u8(buf, BT_HCI_LE_ADV_DISABLE);
    }

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_ADV_ENABLE, buf, NULL);
#endif
    if (err) {
        return err;
    }

    if (enable) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ADVERTISING);
    } else {
        atomic_clear_bit(bt_dev.flags, BT_DEV_ADVERTISING);
    }

    return 0;
}


/*Robin added for set public address*/
static int set_public_address(const bt_addr_t *addr)
{
    struct net_buf *buf;
    int             err = 0;

    buf = bt_hci_cmd_create(BT_HCI_OP_VS_WRITE_BD_ADDR, sizeof(*addr));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, addr, sizeof(*addr));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_BD_ADDR, buf, NULL);
    if (err) {
        return err;
    }

    return 0;
}


static int set_random_address(const bt_addr_t *addr)
{
    int             err = 0;

    BT_DBG("%s", bt_addr_str(addr));
#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_set_random_addr((uint8_t *)addr->val);
#else
    struct net_buf *buf;

    /* Do nothing if we already have the right address */
    if (!bt_addr_cmp(addr, &bt_dev.random_addr.a)) {
        return 0;
    }

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_RANDOM_ADDRESS, sizeof(*addr));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, addr, sizeof(*addr));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_RANDOM_ADDRESS, buf, NULL);
#endif
    if (err) {
        return err;
    }

    bt_addr_copy(&bt_dev.random_addr.a, addr);
    bt_dev.random_addr.type = BT_ADDR_LE_RANDOM;

    return 0;
}


#if defined(CONFIG_BT_PRIVACY)
/* this function sets new RPA only if current one is no longer valid */
static int le_set_private_addr(void)
{
    bt_addr_t rpa;
    int       err;

    /* check if RPA is valid */
    if (atomic_test_bit(bt_dev.flags, BT_DEV_RPA_VALID)) {
        return 0;
    }

    err = bt_rpa_create(bt_dev.irk, &rpa);
    if (!err) {
        err = set_random_address(&rpa);
        if (!err) {
            atomic_set_bit(bt_dev.flags, BT_DEV_RPA_VALID);
        }
    }

    /* restart timer even if failed to set new RPA */
    k_delayed_work_submit(&bt_dev.rpa_update, RPA_TIMEOUT);

    return err;
}

static void rpa_timeout(struct k_work *work)
{
    BT_DBG("%s", __func__, __LINE__);

    /* Invalidate RPA */
    atomic_clear_bit(bt_dev.flags, BT_DEV_RPA_VALID);

    /*
     * we need to update rpa only if advertising is ongoing, with
     * BT_DEV_KEEP_ADVERTISING flag is handled in disconnected event
     */
    if (atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        /* make sure new address is used */
        set_advertise_enable(false);
        le_set_private_addr();
        set_advertise_enable(true);
    }

    if (atomic_test_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN)) {
        /* TODO do we need to toggle scan? */
        le_set_private_addr();
    }
}
#else
static int le_set_private_addr(void)
{
    bt_addr_t nrpa;
    int err;

    err = bt_rand(nrpa.val, sizeof(nrpa.val));
    if (err) {
        return err;
    }

    nrpa.val[5] &= 0x3f;
    return set_random_address(&nrpa);
}
#endif

static int set_le_scan_enable(u8_t enable)
{
    int err;
#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_scan_enable(enable, (enable == BT_HCI_LE_SCAN_ENABLE) ? atomic_test_bit(bt_dev.flags,
                                 BT_DEV_SCAN_FILTER_DUP) : BT_HCI_LE_SCAN_FILTER_DUP_DISABLE);
#else
    struct bt_hci_cp_le_set_scan_enable *cp;
    struct net_buf *                     buf;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_SCAN_ENABLE, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));

    if (enable == BT_HCI_LE_SCAN_ENABLE) {
        cp->filter_dup = atomic_test_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);
    } else {
        cp->filter_dup = BT_HCI_LE_SCAN_FILTER_DUP_DISABLE;
    }

    cp->enable = enable;

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_SCAN_ENABLE, buf, NULL);
#endif
    if (err) {
        return err;
    }

    if (enable == BT_HCI_LE_SCAN_ENABLE) {
        atomic_set_bit(bt_dev.flags, BT_DEV_SCANNING);
    } else {
        atomic_clear_bit(bt_dev.flags, BT_DEV_SCANNING);
    }

    return 0;
}

#if defined(CONFIG_BT_CONN)
static void hci_acl(struct net_buf *buf)
{
    struct bt_hci_acl_hdr *hdr = (void *)buf->data;
    u16_t                  handle, len = sys_le16_to_cpu(hdr->len);
    struct bt_conn *       conn;
    u8_t                   flags;

    BT_DBG("%s, buf %p", __func__, buf);

    handle = sys_le16_to_cpu(hdr->handle);
    flags  = bt_acl_flags(handle);

    acl(buf)->handle = bt_acl_handle(handle);
    acl(buf)->id     = BT_CONN_ID_INVALID;

    net_buf_pull(buf, sizeof(*hdr));

    BT_DBG("%s, handle %u len %u flags %u", __func__, acl(buf)->handle, len, flags);

    if (buf->len != len) {
        BT_ERR("ACL data length mismatch (%u != %u)", buf->len, len);
        net_buf_unref(buf);
        return;
    }

    conn = bt_conn_lookup_handle(acl(buf)->handle);
    if (!conn) {
        BT_ERR("Unable to find conn for handle %u", acl(buf)->handle);
        net_buf_unref(buf);
        return;
    }

    acl(buf)->id = bt_conn_get_id(conn);

    bt_conn_recv(conn, buf, flags);
    bt_conn_unref(conn);
}

extern void bt_conn_notify_tx_done(struct bt_conn *conn);
static void hci_num_completed_packets(struct net_buf *buf)
{
    struct bt_hci_evt_num_completed_packets *evt = (void *)buf->data;
    int i;

    BT_DBG("num_handles %u", evt->num_handles);

    for (i = 0; i < evt->num_handles; i++) {
        u16_t handle, count;
        struct bt_conn *conn;
        unsigned int key;

        handle = sys_le16_to_cpu(evt->h[i].handle);
        count  = sys_le16_to_cpu(evt->h[i].count);

        BT_DBG("handle %u count %u", handle, count);

        key = irq_lock();

        conn = bt_conn_lookup_handle(handle);
        if (!conn) {
            BT_ERR("No connection for handle %u", handle);
            irq_unlock(key);
            continue;
        }

        irq_unlock(key);

        while (count--) {
            sys_snode_t *node;

            key = irq_lock();
            node = sys_slist_get(&conn->tx_pending);
            irq_unlock(key);

            if (!node) {
                BT_ERR("packets count mismatch");
                break;
            }
            k_fifo_put(&conn->tx_notify, node);
            bt_conn_notify_tx_done(conn);
        }

        bt_conn_unref(conn);
    }
}

static int hci_le_create_conn(const struct bt_conn *conn)
{
    int err;
#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_create_conn(BT_GAP_SCAN_FAST_INTERVAL,
                           BT_GAP_SCAN_FAST_INTERVAL,
                           0,
                           conn->le.init_addr.type,
                           (uint8_t *)conn->le.resp_addr.a.val,
                           conn->le.init_addr.type,
                           conn->le.interval_min,
                           conn->le.interval_max,
                           conn->le.latency,
                           conn->le.timeout,
                           0,
                           0);
#else
    struct net_buf *                 buf;
    struct bt_hci_cp_le_create_conn *cp;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_CREATE_CONN, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    memset(cp, 0, sizeof(*cp));

    /* Interval == window for continuous scanning */
    cp->scan_interval = sys_cpu_to_le16(BT_GAP_SCAN_FAST_INTERVAL);
    cp->scan_window = cp->scan_interval;

    bt_addr_le_copy(&cp->peer_addr, &conn->le.resp_addr);
    cp->own_addr_type       = conn->le.init_addr.type;
    cp->conn_interval_min   = sys_cpu_to_le16(conn->le.interval_min);
    cp->conn_interval_max   = sys_cpu_to_le16(conn->le.interval_max);
    cp->conn_latency        = sys_cpu_to_le16(conn->le.latency);
    cp->supervision_timeout = sys_cpu_to_le16(conn->le.timeout);

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_CREATE_CONN, buf, NULL);
#endif
    if (err) {
        return err;
    }
    return 0;
}

static void hci_disconn_complete(struct net_buf *buf)
{
    struct bt_hci_evt_disconn_complete *evt    = (void *)buf->data;
    u16_t                               handle = sys_le16_to_cpu(evt->handle);
    struct bt_conn *                    conn;

    BT_DBG("status %u handle %u reason %u", evt->status, handle, evt->reason);

    if (evt->status) {
        return;
    }

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to look up conn with handle %u", handle);
        goto advertise;
    }

    conn->err = evt->reason;

#if defined(CONFIG_BT_HOST_RX_THREAD)
    STACK_ANALYZE("rx stack", rx_thread_stack);
#endif
    STACK_ANALYZE("tx stack", tx_thread_stack);

    bt_conn_set_state(conn, BT_CONN_DISCONNECTED);
    conn->handle = 0;

    if (conn->type != BT_CONN_TYPE_LE) {
        bt_conn_unref(conn);
        return;
    }

    if (atomic_test_bit(conn->flags, BT_CONN_AUTO_CONNECT)) {
        bt_conn_set_state(conn, BT_CONN_CONNECT_SCAN);
        bt_le_scan_update(false);
    }

    bt_conn_unref(conn);
#ifdef CONFIG_CONTROLLER_IN_ONE_TASK
    g_pkt_recv.signaled = 0;
#endif

advertise:
    if (atomic_test_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING) &&
        !atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        if (IS_ENABLED(CONFIG_BT_PRIVACY) &&
            !BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
            le_set_private_addr();
        }

        set_advertise_enable(true);
    }
}

static int hci_le_read_remote_features(struct bt_conn *conn)
{
#ifdef CONFIG_BT_USING_HCI_API
    int err;
    err = hci_api_le_read_remote_features(conn->handle);
    if (err) {
        return err;
    }
    return 0;
#else
    struct bt_hci_cp_le_read_remote_features *cp;
    struct net_buf *                          buf;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_READ_REMOTE_FEATURES, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp         = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(conn->handle);
    bt_hci_cmd_send(BT_HCI_OP_LE_READ_REMOTE_FEATURES, buf);

    return 0;
#endif
}

static int hci_le_set_data_len(struct bt_conn *conn)
{
    u16_t tx_octets, tx_time;
    int err;
#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_get_max_data_len(&tx_octets, &tx_time);
    if (err) {
        return err;
    }
    err = hci_api_le_set_data_len(conn->handle, tx_octets, tx_time);
    if (err) {
        return err;
    }
    return 0;
#else
    struct bt_hci_rp_le_read_max_data_len *rp;
    struct bt_hci_cp_le_set_data_len *cp;
    struct net_buf *buf, *rsp;

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_MAX_DATA_LEN, NULL, &rsp);
    if (err) {
        return err;
    }

    rp = (void *)rsp->data;
    tx_octets = sys_le16_to_cpu(rp->max_tx_octets);
    tx_time = sys_le16_to_cpu(rp->max_tx_time);
    net_buf_unref(rsp);

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_DATA_LEN, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(conn->handle);
    cp->tx_octets = sys_cpu_to_le16(tx_octets);
    cp->tx_time = sys_cpu_to_le16(tx_time);
    err = bt_hci_cmd_send(BT_HCI_OP_LE_SET_DATA_LEN, buf);
    if (err) {
        return err;
    }

    return 0;
#endif
}

static int hci_le_set_phy(struct bt_conn *conn)
{
    int err;
#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_set_phy(conn->handle,
                                 0,
                                 BT_HCI_LE_PHY_PREFER_2M,
                                 BT_HCI_LE_PHY_PREFER_2M,
                                 BT_HCI_LE_PHY_CODED_ANY);
#else

    struct bt_hci_cp_le_set_phy *cp;
    struct net_buf *             buf;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_PHY, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp           = net_buf_add(buf, sizeof(*cp));
    cp->handle   = sys_cpu_to_le16(conn->handle);
    cp->all_phys = 0;
    cp->tx_phys  = BT_HCI_LE_PHY_PREFER_2M;
    cp->rx_phys  = BT_HCI_LE_PHY_PREFER_2M;
    cp->phy_opts = BT_HCI_LE_PHY_CODED_ANY;
    err = bt_hci_cmd_send(BT_HCI_OP_LE_SET_PHY, buf);
#endif
    if (err) {
        return err;
    }
    return 0;
}

static void update_conn_param(struct bt_conn *conn)
{
    /*
     * Core 4.2 Vol 3, Part C, 9.3.12.2
     * The Peripheral device should not perform a Connection Parameter
     * Update procedure within 5 s after establishing a connection.
     */
    k_delayed_work_submit(&conn->le.update_work,
        conn->role == BT_HCI_ROLE_MASTER ? K_NO_WAIT : CONN_UPDATE_TIMEOUT);
}

#if defined(CONFIG_BT_SMP)
static void update_pending_id(struct bt_keys *keys)
{
    if (atomic_test_and_clear_bit(keys->flags, BT_KEYS_ID_PENDING_ADD)) {
        bt_id_add(keys);
        return;
    }

    if (atomic_test_and_clear_bit(keys->flags, BT_KEYS_ID_PENDING_DEL)) {
        bt_id_del(keys);
        return;
    }
}
#endif

static void le_enh_conn_complete(struct bt_hci_evt_le_enh_conn_complete *evt)
{
    u16_t           handle = sys_le16_to_cpu(evt->handle);
    bt_addr_le_t    peer_addr, id_addr;
    struct bt_conn *conn;
    int             err;

    BT_DBG("status %u handle %u role %u %s", evt->status, handle, evt->role,
           bt_addr_le_str(&evt->peer_addr));

#if defined(CONFIG_BT_SMP)
    if (atomic_test_and_clear_bit(bt_dev.flags, BT_DEV_ID_PENDING)) {
        bt_keys_foreach(BT_KEYS_IRK, update_pending_id);
    }
#endif

    if (evt->status) {
        /*
         * if there was an error we are only interested in pending
         * connection so there is no need to check ID address as
         * only one connection can be in that state
         *
         * Depending on error code address might not be valid anyway.
         */
        conn = bt_conn_lookup_state_le(NULL, BT_CONN_CONNECT);
        if (!conn) {
            return;
        }

        conn->err = evt->status;

        bt_conn_set_state(conn, BT_CONN_DISCONNECTED);

        /* Drop the reference got by lookup call in CONNECT state.
         * We are now in DISCONNECTED state since no successful LE
         * link been made.
         */
        bt_conn_unref(conn);

        return;
    }

    bt_addr_le_copy(&id_addr, &evt->peer_addr);

    /* Translate "enhanced" identity address type to normal one */
    if (id_addr.type == BT_ADDR_LE_PUBLIC_ID ||
        id_addr.type == BT_ADDR_LE_RANDOM_ID) {
        id_addr.type -= BT_ADDR_LE_PUBLIC_ID;
        bt_addr_copy(&peer_addr.a, &evt->peer_rpa);
        peer_addr.type = BT_ADDR_LE_RANDOM;
    } else {
        bt_addr_le_copy(&peer_addr, &evt->peer_addr);
    }

    /*
     * Make lookup to check if there's a connection object in
     * CONNECT state associated with passed peer LE address.
     */
    conn = bt_conn_lookup_state_le(&id_addr, BT_CONN_CONNECT);

    if (evt->role == BT_CONN_ROLE_SLAVE) {
        /*
         * clear advertising even if we are not able to add connection
         * object to keep host in sync with controller state
         */
        atomic_clear_bit(bt_dev.flags, BT_DEV_ADVERTISING);

        /* only for slave we may need to add new connection */
        if (!conn) {
            conn = bt_conn_add_le(&id_addr);
        }
    }

    if (!conn) {
        BT_ERR("Unable to add new conn for handle %u", handle);
        return;
    }

    conn->handle = handle;
    bt_addr_le_copy(&conn->le.dst, &id_addr);
    conn->le.interval = sys_le16_to_cpu(evt->interval);
    conn->le.latency  = sys_le16_to_cpu(evt->latency);
    conn->le.timeout  = sys_le16_to_cpu(evt->supv_timeout);
    conn->role        = evt->role;

    /*
     * Use connection address (instead of identity address) as initiator
     * or responder address. Only slave needs to be updated. For master all
     * was set during outgoing connection creation.
     */
    if (conn->role == BT_HCI_ROLE_SLAVE) {
        bt_addr_le_copy(&conn->le.init_addr, &peer_addr);

        if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
            bt_addr_copy(&conn->le.resp_addr.a, &evt->local_rpa);
            conn->le.resp_addr.type = BT_ADDR_LE_RANDOM;
        } else {
            bt_addr_le_copy(&conn->le.resp_addr, &bt_dev.id_addr);
        }

        /* if the controller supports, lets advertise for another
         * slave connection.
         * check for connectable advertising state is sufficient as
         * this is how this le connection complete for slave occurred.
         */
        if (atomic_test_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING) &&
            BT_LE_STATES_SLAVE_CONN_ADV(bt_dev.le.states)) {
            if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
                le_set_private_addr();
            }

            set_advertise_enable(true);
        }
    }

    bt_conn_set_state(conn, BT_CONN_CONNECTED);

    /*
     * it is possible that connection was disconnected directly from
     * connected callback so we must check state before doing connection
     * parameters update
     */
    if (conn->state != BT_CONN_CONNECTED) {
        goto done;
    }

    if ((evt->role == BT_HCI_ROLE_MASTER) ||
        BT_FEAT_LE_SLAVE_FEATURE_XCHG(bt_dev.le.features)) {
        err = hci_le_read_remote_features(conn);
        if (!err) {
            goto done;
        }
    }

    if (BT_FEAT_LE_PHY_2M(bt_dev.le.features)) {
        err = hci_le_set_phy(conn);
        if (!err) {
            atomic_set_bit(conn->flags, BT_CONN_AUTO_PHY_UPDATE);
            goto done;
        }
    }
#if !defined(BOARD_TG7100B)
    if (BT_FEAT_LE_DLE(bt_dev.le.features)) {
        err = hci_le_set_data_len(conn);
        if (!err) {
            atomic_set_bit(conn->flags, BT_CONN_AUTO_DATA_LEN);
            goto done;
        }
    }
#endif
    update_conn_param(conn);

done:
    bt_conn_unref(conn);
    bt_le_scan_update(false);
}

static void le_legacy_conn_complete(struct net_buf *buf)
{
    struct bt_hci_evt_le_conn_complete *   evt = (void *)buf->data;
    struct bt_hci_evt_le_enh_conn_complete enh;
    const bt_addr_le_t *                   id_addr;

    BT_DBG("status %u role %u %s", evt->status, evt->role,
           bt_addr_le_str(&evt->peer_addr));

    enh.status         = evt->status;
    enh.handle         = evt->handle;
    enh.role           = evt->role;
    enh.interval       = evt->interval;
    enh.latency        = evt->latency;
    enh.supv_timeout   = evt->supv_timeout;
    enh.clock_accuracy = evt->clock_accuracy;

    bt_addr_le_copy(&enh.peer_addr, &evt->peer_addr);

    if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
        bt_addr_copy(&enh.local_rpa, &bt_dev.random_addr.a);
    } else {
        bt_addr_copy(&enh.local_rpa, BT_ADDR_ANY);
    }

    id_addr = find_id_addr(&enh.peer_addr);
    if (id_addr != &enh.peer_addr) {
        bt_addr_copy(&enh.peer_rpa, &enh.peer_addr.a);
        bt_addr_le_copy(&enh.peer_addr, id_addr);
        enh.peer_addr.type += BT_ADDR_LE_PUBLIC_ID;
    } else {
        bt_addr_copy(&enh.peer_rpa, BT_ADDR_ANY);
    }

    le_enh_conn_complete(&enh);
}

static void le_remote_feat_complete(struct net_buf *buf)
{
    struct bt_hci_evt_le_remote_feat_complete *evt = (void *)buf->data;
    u16_t           handle = sys_le16_to_cpu(evt->handle);
    struct bt_conn *conn;

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to lookup conn for handle %u", handle);
        return;
    }

    if (!evt->status) {
        memcpy(conn->le.features, evt->features, sizeof(conn->le.features));
    }

    if (BT_FEAT_LE_PHY_2M(bt_dev.le.features) &&
        BT_FEAT_LE_PHY_2M(conn->le.features)) {
        int err;

        err = hci_le_set_phy(conn);
        if (!err) {
            atomic_set_bit(conn->flags, BT_CONN_AUTO_PHY_UPDATE);
            goto done;
        }
    }

    if (BT_FEAT_LE_DLE(bt_dev.le.features) &&
        BT_FEAT_LE_DLE(conn->le.features)) {
        int err;

        err = hci_le_set_data_len(conn);
        if (!err) {
            atomic_set_bit(conn->flags, BT_CONN_AUTO_DATA_LEN);
            goto done;
        }
    }

    update_conn_param(conn);

done:
    bt_conn_unref(conn);
}

static void le_data_len_change(struct net_buf *buf)
{
    struct bt_hci_evt_le_data_len_change *evt    = (void *)buf->data;
    u16_t                                 handle = sys_le16_to_cpu(evt->handle);
    struct bt_conn *                      conn;

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to lookup conn for handle %u", handle);
        return;
    }

    if (!atomic_test_and_clear_bit(conn->flags, BT_CONN_AUTO_DATA_LEN)) {
        goto done;
    }

    update_conn_param(conn);

done:
    bt_conn_unref(conn);
}

static void le_phy_update_complete(struct net_buf *buf)
{
    struct bt_hci_evt_le_phy_update_complete *evt = (void *)buf->data;
    u16_t           handle = sys_le16_to_cpu(evt->handle);
    struct bt_conn *conn;

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to lookup conn for handle %u", handle);
        return;
    }

    BT_DBG("PHY updated: status: 0x%x, tx: %u, rx: %u", evt->status,
           evt->tx_phy, evt->rx_phy);

    if (!atomic_test_and_clear_bit(conn->flags, BT_CONN_AUTO_PHY_UPDATE)) {
        goto done;
    }

    if (BT_FEAT_LE_DLE(bt_dev.le.features) &&
        BT_FEAT_LE_DLE(conn->le.features)) {
        int err;

        err = hci_le_set_data_len(conn);
        if (!err) {
            atomic_set_bit(conn->flags, BT_CONN_AUTO_DATA_LEN);
            goto done;
        }
    }

    update_conn_param(conn);

done:
    bt_conn_unref(conn);
}

bool bt_le_conn_params_valid(const struct bt_le_conn_param *param)
{
    /* All limits according to BT Core spec 5.0 [Vol 2, Part E, 7.8.12] */

    if (param->interval_min > param->interval_max || param->interval_min < 6 ||
        param->interval_max > 3200) {
        return false;
    }

    if (param->latency > 499) {
        return false;
    }

    if (param->timeout < 10 || param->timeout > 3200 ||
        ((4 * param->timeout) <=
         ((1 + param->latency) * param->interval_max))) {
        return false;
    }

    return true;
}

static int le_conn_param_neg_reply(u16_t handle, u8_t reason)
{
#ifdef CONFIG_BT_USING_HCI_API
    int err;
    err = hci_api_le_conn_param_neg_reply(handle, reason);
    if (err) {
        return err;
    }
    return 0;
#else
    struct bt_hci_cp_le_conn_param_req_neg_reply *cp;
    struct net_buf *                              buf;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_CONN_PARAM_REQ_NEG_REPLY, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp         = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(handle);
    cp->reason = sys_cpu_to_le16(reason);

    return bt_hci_cmd_send(BT_HCI_OP_LE_CONN_PARAM_REQ_NEG_REPLY, buf);
#endif
}

static int le_conn_param_req_reply(u16_t                          handle,
                                   const struct bt_le_conn_param *param)
{
#ifdef CONFIG_BT_USING_HCI_API
    int err;
    err = hci_api_le_conn_param_req_reply(handle, param->interval_min, param->interval_max,
                                          param->latency, param->timeout, 0, 0);
    if (err) {
        return err;
    }
    return 0;
#else
    struct bt_hci_cp_le_conn_param_req_reply *cp;
    struct net_buf *                          buf;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_CONN_PARAM_REQ_REPLY, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    memset(cp, 0, sizeof(*cp));

    cp->handle       = sys_cpu_to_le16(handle);
    cp->interval_min = sys_cpu_to_le16(param->interval_min);
    cp->interval_max = sys_cpu_to_le16(param->interval_max);
    cp->latency      = sys_cpu_to_le16(param->latency);
    cp->timeout      = sys_cpu_to_le16(param->timeout);

    return bt_hci_cmd_send(BT_HCI_OP_LE_CONN_PARAM_REQ_REPLY, buf);
#endif
}

static int le_conn_param_req(struct net_buf *buf)
{
    struct bt_hci_evt_le_conn_param_req *evt = (void *)buf->data;
    struct bt_le_conn_param              param;
    struct bt_conn *                     conn;
    u16_t                                handle;
    int                                  err;

    handle             = sys_le16_to_cpu(evt->handle);
    param.interval_min = sys_le16_to_cpu(evt->interval_min);
    param.interval_max = sys_le16_to_cpu(evt->interval_max);
    param.latency      = sys_le16_to_cpu(evt->latency);
    param.timeout      = sys_le16_to_cpu(evt->timeout);

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to lookup conn for handle %u", handle);
        return le_conn_param_neg_reply(handle, BT_HCI_ERR_UNKNOWN_CONN_ID);
    }

    if (!le_param_req(conn, &param)) {
        err = le_conn_param_neg_reply(handle, BT_HCI_ERR_INVALID_LL_PARAM);
    } else {
        err = le_conn_param_req_reply(handle, &param);
    }

    bt_conn_unref(conn);
    return err;
}

static void le_conn_update_complete(struct net_buf *buf)
{
    struct bt_hci_evt_le_conn_update_complete *evt = (void *)buf->data;
    struct bt_conn *                           conn;
    u16_t                                      handle;

    handle = sys_le16_to_cpu(evt->handle);

    BT_DBG("status %u, handle %u", evt->status, handle);

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to lookup conn for handle %u", handle);
        return;
    }

    if (!evt->status) {
        conn->le.interval = sys_le16_to_cpu(evt->interval);
        conn->le.latency  = sys_le16_to_cpu(evt->latency);
        conn->le.timeout  = sys_le16_to_cpu(evt->supv_timeout);
        notify_le_param_updated(conn);
    }

    bt_conn_unref(conn);
}

static void check_pending_conn(const bt_addr_le_t *id_addr,
                               const bt_addr_le_t *addr, u8_t evtype)
{
    struct bt_conn *conn;

    /* No connections are allowed during explicit scanning */
    if (atomic_test_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return;
    }

    /* Return if event is not connectable */
    if (evtype != BT_LE_ADV_IND && evtype != BT_LE_ADV_DIRECT_IND) {
        return;
    }

    conn = bt_conn_lookup_state_le(id_addr, BT_CONN_CONNECT_SCAN);
    if (!conn) {
        return;
    }

    if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING) &&
        set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE)) {
        goto failed;
    }

    if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
        if (le_set_private_addr()) {
            goto failed;
        }

        bt_addr_le_copy(&conn->le.init_addr, &bt_dev.random_addr);
    } else {
        /* If Static Random address is used as Identity address we
         * need to restore it before creating connection. Otherwise
         * NRPA used for active scan could be used for connection.
         */
        if (atomic_test_bit(bt_dev.flags, BT_DEV_ID_STATIC_RANDOM)) {
            set_random_address(&bt_dev.id_addr.a);
        }

        bt_addr_le_copy(&conn->le.init_addr, &bt_dev.id_addr);
    }

    bt_addr_le_copy(&conn->le.resp_addr, addr);

    if (hci_le_create_conn(conn)) {
        goto failed;
    }
    bt_conn_set_state(conn, BT_CONN_CONNECT);
    bt_conn_unref(conn);
    return;

failed:
    conn->err = BT_HCI_ERR_UNSPECIFIED;
    bt_conn_set_state(conn, BT_CONN_DISCONNECTED);
    bt_conn_unref(conn);
    bt_le_scan_update(false);
}
#endif /* CONFIG_BT_CONN */

#if defined(CONFIG_BT_SMP)
static int addr_res_enable(u8_t enable)
{
    struct net_buf *buf;

    BT_DBG("%s", enable ? "enabled" : "disabled");

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_ADDR_RES_ENABLE, 1);
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_u8(buf, enable);

    return bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_ADDR_RES_ENABLE, buf, NULL);
}

static int hci_id_add(const bt_addr_le_t *addr, u8_t val[16])
{
    struct bt_hci_cp_le_add_dev_to_rl *cp;
    struct net_buf *                   buf;

    BT_DBG("addr %s", bt_addr_le_str(addr));

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_ADD_DEV_TO_RL, sizeof(*cp));
    if (!buf) {
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    bt_addr_le_copy(&cp->peer_id_addr, addr);
    memcpy(cp->peer_irk, val, 16);

#if defined(CONFIG_BT_PRIVACY)
    memcpy(cp->local_irk, bt_dev.irk, 16);
#else
    memset(cp->local_irk, 0, 16);
#endif

    return bt_hci_cmd_send_sync(BT_HCI_OP_LE_ADD_DEV_TO_RL, buf, NULL);
}

int bt_id_add(struct bt_keys *keys)
{
    bool            adv_enabled, scan_enabled;
    struct bt_conn *conn;
    int             err;

    BT_DBG("addr %s", bt_addr_le_str(&keys->addr));

    /* Nothing to be done if host-side resolving is used */
    if (!bt_dev.le.rl_size || bt_dev.le.rl_entries > bt_dev.le.rl_size) {
        bt_dev.le.rl_entries++;
        return 0;
    }

    conn = bt_conn_lookup_state_le(NULL, BT_CONN_CONNECT);
    if (conn) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ID_PENDING);
        atomic_set_bit(keys->flags, BT_KEYS_ID_PENDING_ADD);
        bt_conn_unref(conn);
        return -EAGAIN;
    }

    adv_enabled = atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING);
    if (adv_enabled) {
        set_advertise_enable(false);
    }

    scan_enabled = atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING);
    if (scan_enabled) {
        set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE);
    }

    /* If there are any existing entries address resolution will be on */
    if (bt_dev.le.rl_entries) {
        err = addr_res_enable(BT_HCI_ADDR_RES_DISABLE);
        if (err) {
            BT_WARN("Failed to disable address resolution");
            goto done;
        }
    }

    if (bt_dev.le.rl_entries == bt_dev.le.rl_size) {
        BT_WARN("Resolving list size exceeded. Switching to host.");

        err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_CLEAR_RL, NULL, NULL);
        if (err) {
            BT_ERR("Failed to clear resolution list");
            goto done;
        }

        bt_dev.le.rl_entries++;

        goto done;
    }

    err = hci_id_add(&keys->addr, keys->irk.val);
    if (err) {
        BT_ERR("Failed to add IRK to controller");
        goto done;
    }

    bt_dev.le.rl_entries++;

done:
    addr_res_enable(BT_HCI_ADDR_RES_ENABLE);

    if (scan_enabled) {
        set_le_scan_enable(BT_HCI_LE_SCAN_ENABLE);
    }

    if (adv_enabled) {
        set_advertise_enable(true);
    }

    return err;
}

static void keys_add_id(struct bt_keys *keys)
{
    hci_id_add(&keys->addr, keys->irk.val);
}

int bt_id_del(struct bt_keys *keys)
{
    struct bt_hci_cp_le_rem_dev_from_rl *cp;
    bool                                 adv_enabled, scan_enabled;
    struct bt_conn *                     conn;
    struct net_buf *                     buf;
    int                                  err;

    BT_DBG("addr %s", bt_addr_le_str(&keys->addr));

    if (!bt_dev.le.rl_size || bt_dev.le.rl_entries > bt_dev.le.rl_size + 1) {
        bt_dev.le.rl_entries--;
        return 0;
    }

    conn = bt_conn_lookup_state_le(NULL, BT_CONN_CONNECT);
    if (conn) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ID_PENDING);
        atomic_set_bit(keys->flags, BT_KEYS_ID_PENDING_DEL);
        bt_conn_unref(conn);
        return -EAGAIN;
    }

    adv_enabled = atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING);
    if (adv_enabled) {
        set_advertise_enable(false);
    }

    scan_enabled = atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING);
    if (scan_enabled) {
        set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE);
    }

    err = addr_res_enable(BT_HCI_ADDR_RES_DISABLE);
    if (err) {
        BT_ERR("Disabling address resolution failed (err %d)", err);
        goto done;
    }

    /* We checked size + 1 earlier, so here we know we can fit again */
    if (bt_dev.le.rl_entries > bt_dev.le.rl_size) {
        bt_dev.le.rl_entries--;
        keys->keys &= ~BT_KEYS_IRK;
        bt_keys_foreach(BT_KEYS_IRK, keys_add_id);
        goto done;
    }

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_REM_DEV_FROM_RL, sizeof(*cp));
    if (!buf) {
        err = -ENOBUFS;
        goto done;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    bt_addr_le_copy(&cp->peer_id_addr, &keys->addr);

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_REM_DEV_FROM_RL, buf, NULL);
    if (err) {
        BT_ERR("Failed to remove IRK from controller");
        goto done;
    }

    bt_dev.le.rl_entries--;

done:
    /* Only re-enable if there are entries to do resolving with */
    if (bt_dev.le.rl_entries) {
        addr_res_enable(BT_HCI_ADDR_RES_ENABLE);
    }

    if (scan_enabled) {
        set_le_scan_enable(BT_HCI_LE_SCAN_ENABLE);
    }

    if (adv_enabled) {
        set_advertise_enable(true);
    }

    return err;
}

static void update_sec_level(struct bt_conn *conn)
{
    if (!conn->encrypt) {
        conn->sec_level = BT_SECURITY_LOW;
        return;
    }

    if (conn->le.keys &&
        atomic_test_bit(conn->le.keys->flags, BT_KEYS_AUTHENTICATED)) {
        if (conn->le.keys->keys & BT_KEYS_LTK_P256) {
            conn->sec_level = BT_SECURITY_FIPS;
        } else {
            conn->sec_level = BT_SECURITY_HIGH;
        }
    } else {
        conn->sec_level = BT_SECURITY_MEDIUM;
    }

    if (conn->required_sec_level > conn->sec_level) {
        BT_ERR("Failed to set required security level");
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTHENTICATION_FAIL);
    }
}
#endif /* CONFIG_BT_SMP */

#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_BREDR)
static void hci_encrypt_change(struct net_buf *buf)
{
    struct bt_hci_evt_encrypt_change *evt    = (void *)buf->data;
    u16_t                             handle = sys_le16_to_cpu(evt->handle);
    struct bt_conn *                  conn;

    BT_DBG("status %u handle %u encrypt 0x%02x", evt->status, handle,
           evt->encrypt);

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to look up conn with handle %u", handle);
        return;
    }

    if (evt->status) {
        /* TODO report error */
        if (conn->type == BT_CONN_TYPE_LE) {
            /* reset required security level in case of error */
            conn->required_sec_level = conn->sec_level;
        }
        bt_conn_unref(conn);
        return;
    }

    conn->encrypt = evt->encrypt;

#if defined(CONFIG_BT_SMP)
    if (conn->type == BT_CONN_TYPE_LE) {
        /*
         * we update keys properties only on successful encryption to
         * avoid losing valid keys if encryption was not successful.
         *
         * Update keys with last pairing info for proper sec level
         * update. This is done only for LE transport, for BR/EDR keys
         * are updated on HCI 'Link Key Notification Event'
         */
        if (conn->encrypt) {
            bt_smp_update_keys(conn);
        }
        update_sec_level(conn);
    }
#endif /* CONFIG_BT_SMP */

    bt_l2cap_encrypt_change(conn, evt->status);
    bt_conn_security_changed(conn);

    bt_conn_unref(conn);
}

static void hci_encrypt_key_refresh_complete(struct net_buf *buf)
{
    struct bt_hci_evt_encrypt_key_refresh_complete *evt = (void *)buf->data;
    struct bt_conn *                                conn;
    u16_t                                           handle;

    handle = sys_le16_to_cpu(evt->handle);

    BT_DBG("status %u handle %u", evt->status, handle);

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to look up conn with handle %u", handle);
        return;
    }

    if (evt->status) {
        bt_l2cap_encrypt_change(conn, evt->status);
        return;
    }

    /*
     * Update keys with last pairing info for proper sec level update.
     * This is done only for LE transport. For BR/EDR transport keys are
     * updated on HCI 'Link Key Notification Event', therefore update here
     * only security level based on available keys and encryption state.
     */
#if defined(CONFIG_BT_SMP)
    if (conn->type == BT_CONN_TYPE_LE) {
        bt_smp_update_keys(conn);
        update_sec_level(conn);
    }
#endif /* CONFIG_BT_SMP */

    bt_l2cap_encrypt_change(conn, evt->status);
    bt_conn_security_changed(conn);
    bt_conn_unref(conn);
}
#endif /* CONFIG_BT_SMP || CONFIG_BT_BREDR */

#if defined(CONFIG_BT_SMP)
static void le_ltk_request(struct net_buf *buf)
{
    struct bt_hci_evt_le_ltk_request *     evt = (void *)buf->data;
    struct bt_hci_cp_le_ltk_req_neg_reply *cp;
    struct bt_conn *                       conn;
    u16_t                                  handle;
    u8_t                                   tk[16];

    handle = sys_le16_to_cpu(evt->handle);

    BT_DBG("handle %u", handle);

    conn = bt_conn_lookup_handle(handle);
    if (!conn) {
        BT_ERR("Unable to lookup conn for handle %u", handle);
        return;
    }

    /*
     * if TK is present use it, that means pairing is in progress and
     * we should use new TK for encryption
     *
     * Both legacy STK and LE SC LTK have rand and ediv equal to zero.
     */
    if (evt->rand == 0 && evt->ediv == 0 && bt_smp_get_tk(conn, tk)) {
        struct bt_hci_cp_le_ltk_req_reply *cp;

        buf = bt_hci_cmd_create(BT_HCI_OP_LE_LTK_REQ_REPLY, sizeof(*cp));
        if (!buf) {
            BT_ERR("Out of command buffers");
            goto done;
        }

        cp         = net_buf_add(buf, sizeof(*cp));
        cp->handle = evt->handle;
        memcpy(cp->ltk, tk, sizeof(cp->ltk));

        bt_hci_cmd_send(BT_HCI_OP_LE_LTK_REQ_REPLY, buf);
        goto done;
    }

    if (!conn->le.keys) {
        conn->le.keys = bt_keys_find(BT_KEYS_LTK_P256, &conn->le.dst);
        if (!conn->le.keys) {
            conn->le.keys = bt_keys_find(BT_KEYS_SLAVE_LTK, &conn->le.dst);
        }
    }

    if (conn->le.keys && (conn->le.keys->keys & BT_KEYS_LTK_P256) &&
        evt->rand == 0 && evt->ediv == 0) {
        struct bt_hci_cp_le_ltk_req_reply *cp;

        buf = bt_hci_cmd_create(BT_HCI_OP_LE_LTK_REQ_REPLY, sizeof(*cp));
        if (!buf) {
            BT_ERR("Out of command buffers");
            goto done;
        }

        cp         = net_buf_add(buf, sizeof(*cp));
        cp->handle = evt->handle;

        /* use only enc_size bytes of key for encryption */
        memcpy(cp->ltk, conn->le.keys->ltk.val, conn->le.keys->enc_size);
        if (conn->le.keys->enc_size < sizeof(cp->ltk)) {
            memset(cp->ltk + conn->le.keys->enc_size, 0,
                   sizeof(cp->ltk) - conn->le.keys->enc_size);
        }

        bt_hci_cmd_send(BT_HCI_OP_LE_LTK_REQ_REPLY, buf);
        goto done;
    }

#if !defined(CONFIG_BT_SMP_SC_ONLY)
    if (conn->le.keys && (conn->le.keys->keys & BT_KEYS_SLAVE_LTK) &&
        conn->le.keys->slave_ltk.rand == evt->rand &&
        conn->le.keys->slave_ltk.ediv == evt->ediv) {
        struct bt_hci_cp_le_ltk_req_reply *cp;
        struct net_buf *                   buf;

        buf = bt_hci_cmd_create(BT_HCI_OP_LE_LTK_REQ_REPLY, sizeof(*cp));
        if (!buf) {
            BT_ERR("Out of command buffers");
            goto done;
        }

        cp         = net_buf_add(buf, sizeof(*cp));
        cp->handle = evt->handle;

        /* use only enc_size bytes of key for encryption */
        memcpy(cp->ltk, conn->le.keys->slave_ltk.val, conn->le.keys->enc_size);
        if (conn->le.keys->enc_size < sizeof(cp->ltk)) {
            memset(cp->ltk + conn->le.keys->enc_size, 0,
                   sizeof(cp->ltk) - conn->le.keys->enc_size);
        }

        bt_hci_cmd_send(BT_HCI_OP_LE_LTK_REQ_REPLY, buf);
        goto done;
    }
#endif /* !CONFIG_BT_SMP_SC_ONLY */

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_LTK_REQ_NEG_REPLY, sizeof(*cp));
    if (!buf) {
        BT_ERR("Out of command buffers");
        goto done;
    }

    cp         = net_buf_add(buf, sizeof(*cp));
    cp->handle = evt->handle;

    bt_hci_cmd_send(BT_HCI_OP_LE_LTK_REQ_NEG_REPLY, buf);

done:
    bt_conn_unref(conn);
}
#endif /* CONFIG_BT_SMP */

static void le_pkey_complete(struct net_buf *buf)
{
    struct bt_hci_evt_le_p256_public_key_complete *evt = (void *)buf->data;
    struct bt_pub_key_cb *                         cb;

    BT_DBG("status: 0x%x", evt->status);

    atomic_clear_bit(bt_dev.flags, BT_DEV_PUB_KEY_BUSY);

    if (!evt->status) {
        memcpy(pub_key, evt->key, 64);
        atomic_set_bit(bt_dev.flags, BT_DEV_HAS_PUB_KEY);
    }

    for (cb = pub_key_cb; cb; cb = cb->_next) {
        cb->func(evt->status ? NULL : evt->key);
    }
}

static void le_dhkey_complete(struct net_buf *buf)
{
    struct bt_hci_evt_le_generate_dhkey_complete *evt = (void *)buf->data;

    BT_DBG("status: 0x%x", evt->status);

    if (dh_key_cb) {
        dh_key_cb(evt->status ? NULL : evt->dhkey);
        dh_key_cb = NULL;
    }
}

#ifdef CONFIG_BT_USING_HCI_API
static void hci_reset_complete()
{
    scan_dev_found_cb = NULL;
    atomic_set(bt_dev.flags, BIT(BT_DEV_ENABLE));
}
#else
static void hci_reset_complete(struct net_buf *buf)
{
    u8_t status = buf->data[0];

    BT_DBG("status %u", status);

    if (status) {
        return;
    }

    scan_dev_found_cb = NULL;

    /* we only allow to enable once so this bit must be keep set */
    atomic_set(bt_dev.flags, BIT(BT_DEV_ENABLE));
}
#endif

static void hci_cmd_done(u16_t opcode, u8_t status, struct net_buf *buf)
{
    BT_DBG("%s, opcode 0x%04x status 0x%02x buf %p", __func__, opcode, status, buf);

    if (net_buf_pool_get(buf->pool_id) != &hci_cmd_pool) {
        BT_WARN("opcode 0x%04x pool id %u pool %p != &hci_cmd_pool %p", opcode,
                buf->pool_id, net_buf_pool_get(buf->pool_id), &hci_cmd_pool);
        return;
    }

    if (cmd(buf)->opcode != opcode) {
        BT_WARN("OpCode 0x%04x completed instead of expected 0x%04x", opcode,
                cmd(buf)->opcode);
    }

    if (cmd(buf)->sync) {
        cmd(buf)->status = status;
        cmd(buf)->sync = SYNC_TX_DONE;
    }
}

static void hci_cmd_complete(struct net_buf *buf)
{
    struct bt_hci_evt_cmd_complete *evt    = (void *)buf->data;
    u16_t                           opcode = sys_le16_to_cpu(evt->opcode);
    u8_t                            status;//, ncmd = evt->ncmd;

    BT_DBG("opcode 0x%04x", opcode);

    net_buf_pull(buf, sizeof(*evt));

    /* All command return parameters have a 1-byte status in the
     * beginning, so we can safely make this generalization.
     */
    status = buf->data[0];

    hci_cmd_done(opcode, status, buf);
}

static void hci_cmd_status(struct net_buf *buf)
{
    struct bt_hci_evt_cmd_status *evt    = (void *)buf->data;
    u16_t opcode = sys_le16_to_cpu(evt->opcode);

    BT_DBG("opcode 0x%04x", opcode);

    net_buf_pull(buf, sizeof(*evt));

    hci_cmd_done(opcode, evt->status, buf);
}

static int start_le_scan(u8_t scan_type, u16_t interval, u16_t window)
{
    struct bt_hci_cp_le_set_scan_param set_param;
    int                                err;

    memset(&set_param, 0, sizeof(set_param));

    set_param.scan_type = scan_type;

    /* for the rest parameters apply default values according to
     *  spec 4.2, vol2, part E, 7.8.10
     */
    set_param.interval      = sys_cpu_to_le16(interval);
    set_param.window        = sys_cpu_to_le16(window);
    set_param.filter_policy = 0x00;

    if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
        err = le_set_private_addr();
        if (err) {
            return err;
        }

        if (BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
            set_param.addr_type = BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
        } else {
            set_param.addr_type = BT_ADDR_LE_RANDOM;
        }
    } else {
        set_param.addr_type = bt_dev.id_addr.type;

        /* Use NRPA unless identity has been explicitly requested
         * (through Kconfig), or if there is no advertising ongoing.
         */
        if (!IS_ENABLED(CONFIG_BT_SCAN_WITH_IDENTITY) &&
            scan_type == BT_HCI_LE_SCAN_ACTIVE &&
            !atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
            err = le_set_private_addr();
            if (err) {
                return err;
            }

            set_param.addr_type = BT_ADDR_LE_RANDOM;
        }
    }

#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_scan_param_set(scan_type, interval, window, set_param.addr_type, atomic_test_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP));
#else
    struct net_buf *                   buf;
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_SCAN_PARAM, sizeof(set_param));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, &set_param, sizeof(set_param));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_SCAN_PARAM, buf, NULL);
#endif
    if (err) {
        return err;
    }

    err = set_le_scan_enable(BT_HCI_LE_SCAN_ENABLE);
    if (err) {
        return err;
    }

    if (scan_type == BT_HCI_LE_SCAN_ACTIVE) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN);
    } else {
        atomic_clear_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN);
    }

    return 0;
}

int bt_le_scan_update(bool fast_scan)
{
    if (atomic_test_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return 0;
    }

    if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING)) {
        int err;

        err = set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE);
        if (err) {
            return err;
        }
    }

    if (IS_ENABLED(CONFIG_BT_CENTRAL)) {
        u16_t           interval, window;
        struct bt_conn *conn;

        conn = bt_conn_lookup_state_le(NULL, BT_CONN_CONNECT_SCAN);
        if (!conn) {
            return 0;
        }

        atomic_set_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);

        bt_conn_unref(conn);

        if (fast_scan) {
            interval = BT_GAP_SCAN_FAST_INTERVAL;
            window   = BT_GAP_SCAN_FAST_WINDOW;
        } else {
            interval = BT_GAP_SCAN_SLOW_INTERVAL_1;
            window   = BT_GAP_SCAN_SLOW_WINDOW_1;
        }

        return start_le_scan(BT_HCI_LE_SCAN_PASSIVE, interval, window);
    }

    return 0;
}

static void le_adv_report(struct net_buf *buf)
{
    u8_t                                   num_reports = net_buf_pull_u8(buf);
    struct bt_hci_evt_le_advertising_info *info;

    BT_DBG("Adv number of reports %u", num_reports);

    while (num_reports--) {
        bt_addr_le_t id_addr;
        s8_t         rssi;

        info = (void *)buf->data;
        net_buf_pull(buf, sizeof(*info));

        rssi = info->data[info->length];

        BT_DBG("%s event %u, len %u, rssi %d dBm", bt_addr_le_str(&info->addr),
               info->evt_type, info->length, rssi);

        if (info->addr.type == BT_ADDR_LE_PUBLIC_ID ||
            info->addr.type == BT_ADDR_LE_RANDOM_ID) {
            bt_addr_le_copy(&id_addr, &info->addr);
            id_addr.type -= BT_ADDR_LE_PUBLIC_ID;
        } else {
            bt_addr_le_copy(&id_addr, find_id_addr(&info->addr));
        }

        /* bugfix: use scan_dev_found_cb_tmp to avoid jump to NULL */
        bt_le_scan_cb_t *scan_dev_found_cb_tmp = scan_dev_found_cb;

        if (scan_dev_found_cb_tmp) {
            struct net_buf_simple_state state;
            net_buf_simple_save(&buf->b, &state);
            buf->len = info->length;
            scan_dev_found_cb_tmp(&id_addr, rssi, info->evt_type, &buf->b);
            net_buf_simple_restore(&buf->b, &state);
        }

#if defined(CONFIG_BT_CONN)
        check_pending_conn(&id_addr, &info->addr, info->evt_type);
#endif /* CONFIG_BT_CONN */

        /* Get next report iteration by moving pointer to right offset
         * in buf according to spec 4.2, Vol 2, Part E, 7.7.65.2.
         */
        net_buf_pull(buf, info->length + sizeof(rssi));
    }
}

static void hci_le_meta_event(struct net_buf *buf)
{
    struct bt_hci_evt_le_meta_event *evt = (void *)buf->data;

    BT_DBG("%s, subevent 0x%02x", __func__, evt->subevent);

    net_buf_pull(buf, sizeof(*evt));

    switch (evt->subevent) {
#if defined(CONFIG_BT_CONN)
        case BT_HCI_EVT_LE_CONN_COMPLETE:
            le_legacy_conn_complete(buf);
            break;
        case BT_HCI_EVT_LE_ENH_CONN_COMPLETE:
            le_enh_conn_complete((void *)buf->data);
            break;
        case BT_HCI_EVT_LE_CONN_UPDATE_COMPLETE:
            le_conn_update_complete(buf);
            break;
        case BT_HCI_EV_LE_REMOTE_FEAT_COMPLETE:
            le_remote_feat_complete(buf);
            break;
        case BT_HCI_EVT_LE_CONN_PARAM_REQ:
            le_conn_param_req(buf);
            break;
        case BT_HCI_EVT_LE_DATA_LEN_CHANGE:
            le_data_len_change(buf);
            break;
        case BT_HCI_EVT_LE_PHY_UPDATE_COMPLETE:
            le_phy_update_complete(buf);
            break;
#endif /* CONFIG_BT_CONN */
#if defined(CONFIG_BT_SMP)
        case BT_HCI_EVT_LE_LTK_REQUEST:
            le_ltk_request(buf);
            break;
#endif /* CONFIG_BT_SMP */
        case BT_HCI_EVT_LE_P256_PUBLIC_KEY_COMPLETE:
            le_pkey_complete(buf);
            break;
        case BT_HCI_EVT_LE_GENERATE_DHKEY_COMPLETE:
            le_dhkey_complete(buf);
            break;
        case BT_HCI_EVT_LE_ADVERTISING_REPORT:
            le_adv_report(buf);
            break;
        default:
            BT_WARN("Unhandled LE event 0x%02x len %u: %s", evt->subevent,
                    buf->len, bt_hex(buf->data, buf->len));
            break;
    }
}

static void hci_event(struct net_buf *buf)
{
    struct bt_hci_evt_hdr *hdr = (void *)buf->data;

    BT_DBG("%s, event 0x%02x", __func__, hdr->evt);

    BT_ASSERT(!bt_hci_evt_is_prio(hdr->evt));

    net_buf_pull(buf, sizeof(*hdr));

    switch (hdr->evt) {
#if defined(CONFIG_BT_CONN)
        case BT_HCI_EVT_DISCONN_COMPLETE:
            hci_disconn_complete(buf);
            break;
#endif /* CONFIG_BT_CONN */
#if defined(CONFIG_BT_SMP)
        case BT_HCI_EVT_ENCRYPT_CHANGE:
            hci_encrypt_change(buf);
            break;
        case BT_HCI_EVT_ENCRYPT_KEY_REFRESH_COMPLETE:
            hci_encrypt_key_refresh_complete(buf);
            break;
#endif /* CONFIG_BT_SMP */
        case BT_HCI_EVT_LE_META_EVENT:
            hci_le_meta_event(buf);
            break;
        default:
            BT_WARN("Unhandled event 0x%02x len %u: %s", hdr->evt, buf->len,
                    bt_hex(buf->data, buf->len));
            break;
    }

    net_buf_unref(buf);
}

static void send_cmd(struct net_buf *buf)
{
    int err;

    /* Clear out any existing sent command */
    if (bt_dev.sent_cmd) {
        BT_ERR("Uncleared pending sent_cmd");
        net_buf_unref(bt_dev.sent_cmd);
        bt_dev.sent_cmd = NULL;
    }

    bt_dev.sent_cmd = net_buf_ref(buf);

    BT_DBG("Sending command 0x%04x (buf %p) to driver", cmd(buf)->opcode, buf);

    err = bt_send(buf);
    if (err) {
        BT_ERR("Unable to send to driver (err %d)", err);
        hci_cmd_done(cmd(buf)->opcode, BT_HCI_ERR_UNSPECIFIED, NULL);
        net_buf_unref(bt_dev.sent_cmd);
        bt_dev.sent_cmd = NULL;
        net_buf_unref(buf);
    }
}

extern int8_t bt_conn_get_pkts(struct bt_conn *conn);
int has_tx_sem(struct k_poll_event *event)
{
    struct bt_conn *conn = NULL;

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        if (event->tag == BT_EVENT_CONN_TX_QUEUE) {
            conn = CONTAINER_OF(event->fifo, struct bt_conn, tx_queue);
        }
        if (conn && (conn->tx || bt_conn_get_pkts(conn) == 0)) {
            return 0;
        }
    }
    return 1;
}

void process_events(struct k_poll_event *ev, int count)
{
    for (; count; ev++, count--) {
        switch (ev->state) {
            case K_POLL_STATE_FIFO_DATA_AVAILABLE:
                if (ev->tag == BT_EVENT_CMD_TX) {
                    struct net_buf *buf;
                    buf = net_buf_get(&bt_dev.cmd_tx_queue, K_NO_WAIT);
                    if (buf) {
                        send_cmd(buf);
                    }
                } else if (IS_ENABLED(CONFIG_BT_CONN)) {
                    struct bt_conn *conn;

                    if (ev->tag == BT_EVENT_CONN_TX_NOTIFY) {
                        conn = CONTAINER_OF(ev->fifo, struct bt_conn, tx_notify);
                        bt_conn_notify_tx(conn);
                    } else if (ev->tag == BT_EVENT_CONN_TX_QUEUE) {
                        conn = CONTAINER_OF(ev->fifo, struct bt_conn, tx_queue);
                        bt_conn_process_tx(conn);
                    }
                }
                break;
#ifdef CONFIG_CONTROLLER_IN_ONE_TASK
            case K_POLL_STATE_DATA_RECV:
                if (ev->tag == BT_EVENT_CONN_RX) {
                    if (bt_dev.drv->recv) {
                        bt_dev.drv->recv();
                    }
                }
                break;
#endif
            case K_POLL_STATE_NOT_READY:
                break;
            default:
                BT_WARN("Unexpected k_poll event state %u", ev->state);
                break;
        }
    }
}

#if defined(CONFIG_BT_CONN)
/* command FIFO + MAX_CONN * 2 (tx & tx_notify) */
#define EV_COUNT (2 + (CONFIG_BT_MAX_CONN * 2))
#else
/* command FIFO */
#define EV_COUNT 1
#endif

extern void scheduler_loop(struct k_poll_event *events);
static void hci_tx_thread(void *p1, void *p2, void *p3)
{
#ifdef CONFIG_CONTROLLER_IN_ONE_TASK
    static struct k_poll_event events[EV_COUNT] = {
        K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_DATA_RECV,
                                        K_POLL_MODE_NOTIFY_ONLY,
                                        &g_pkt_recv, BT_EVENT_CONN_RX),
        K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_FIFO_DATA_AVAILABLE,
                                        K_POLL_MODE_NOTIFY_ONLY,
                                        &bt_dev.cmd_tx_queue, BT_EVENT_CMD_TX),
    };
#else
    static struct k_poll_event events[EV_COUNT] = {
        K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_FIFO_DATA_AVAILABLE,
                                        K_POLL_MODE_NOTIFY_ONLY,
                                        &bt_dev.cmd_tx_queue, BT_EVENT_CMD_TX),
    };
#endif

    scheduler_loop(events);
}

#if !defined(CONFIG_BT_CONN)
static void le_read_buffer_size_complete(struct net_buf *buf)
{
    struct bt_hci_rp_le_read_buffer_size *rp = (void *)buf->data;

    BT_DBG("%s, status %u", __func__, rp->status);

    bt_dev.le.mtu = sys_le16_to_cpu(rp->le_max_len);
    if (!bt_dev.le.mtu) {
        return;
    }

    BT_DBG("ACL LE buffers: pkts %u mtu %u", rp->le_max_num, bt_dev.le.mtu);

    bt_dev.le.pkts = min(rp->le_max_num, CONFIG_BT_CONN_TX_MAX);
}
#endif

#ifndef CONFIG_BT_USING_HCI_API
static void read_local_ver_complete(struct net_buf *buf)
{
    struct bt_hci_rp_read_local_version_info *rp = (void *)buf->data;

    BT_DBG("status %u", rp->status);

    bt_dev.hci_version    = rp->hci_version;
    bt_dev.hci_revision   = sys_le16_to_cpu(rp->hci_revision);
    bt_dev.lmp_version    = rp->lmp_version;
    bt_dev.lmp_subversion = sys_le16_to_cpu(rp->lmp_subversion);
    bt_dev.manufacturer   = sys_le16_to_cpu(rp->manufacturer);
}

static void read_bdaddr_complete(struct net_buf *buf)
{
    struct bt_hci_rp_read_bd_addr *rp = (void *)buf->data;

    BT_DBG("status %u", rp->status);

    bt_addr_copy(&bt_dev.id_addr.a, &rp->bdaddr);
    bt_dev.id_addr.type = BT_ADDR_LE_PUBLIC;
}

static void read_le_features_complete(struct net_buf *buf)
{
    struct bt_hci_rp_le_read_local_features *rp = (void *)buf->data;

    BT_DBG("status %u", rp->status);

    memcpy(bt_dev.le.features, rp->features, sizeof(bt_dev.le.features));
}

static void read_supported_commands_complete(struct net_buf *buf)
{
    struct bt_hci_rp_read_supported_commands *rp = (void *)buf->data;

    BT_DBG("status %u", rp->status);

    memcpy(bt_dev.supported_commands, rp->commands, sizeof(bt_dev.supported_commands));

    /*
     * Report "LE Read Local P-256 Public Key" and "LE Generate DH Key" as
     * supported if TinyCrypt ECC is used for emulation.
     */
    if (IS_ENABLED(CONFIG_BT_TINYCRYPT_ECC)) {
        bt_dev.supported_commands[34] |= 0x02;
        bt_dev.supported_commands[34] |= 0x04;
    }
}

static void read_local_features_complete(struct net_buf *buf)
{
    struct bt_hci_rp_read_local_features *rp = (void *)buf->data;

    BT_DBG("status %u", rp->status);

    memcpy(bt_dev.features[0], rp->features, sizeof(bt_dev.features[0]));
}

static void le_read_supp_states_complete(struct net_buf *buf)
{
    struct bt_hci_rp_le_read_supp_states *rp = (void *)buf->data;

    BT_DBG("status %u", rp->status);

    bt_dev.le.states = sys_get_le64(rp->le_states);
}
#endif

static int common_init(void)
{
//    struct net_buf *rsp;
    int             err;

#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_reset();
    if (err)
    {
        return err;
    }
    hci_reset_complete();
#else
    /* Send HCI_RESET */
    err = bt_hci_cmd_send_sync(BT_HCI_OP_RESET, NULL, &rsp);
    if (err) {
        return err;
    }
    hci_reset_complete(rsp);
    net_buf_unref(rsp);
#endif
#ifdef CONFIG_BT_USING_HCI_API
    /* Read Local Supported Features */
    err = hci_api_read_local_feature(bt_dev.features[0]);
    if (err) {
        return err;
    }
    err = hci_api_read_local_version_info(&bt_dev.hci_version,
                                          &bt_dev.lmp_version,
                                          &bt_dev.hci_revision,
                                          &bt_dev.lmp_subversion,
                                          &bt_dev.manufacturer);

    if (err) {
        return err;
    }

    /* Read Bluetooth Address */
    err = hci_api_read_bdaddr((u8_t *)(&bt_dev.id_addr.a.val));

    if (!err) {
        bt_dev.id_addr.type = BT_ADDR_LE_PUBLIC;
    }

    /* Read Local Supported Commands */
    err = hci_api_read_local_support_command(bt_dev.supported_commands);

    if (err) {
        return err;
    }

    /*
     * Report "LE Read Local P-256 Public Key" and "LE Generate DH Key" as
     * supported if TinyCrypt ECC is used for emulation.
     */
    if (IS_ENABLED(CONFIG_BT_TINYCRYPT_ECC)) {
        bt_dev.supported_commands[34] |= 0x02;
        bt_dev.supported_commands[34] |= 0x04;

        if (err) {
            printf("err %d func %s %d", err, __func__, __LINE__);
            return err;
        }
    }
#else
    err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_LOCAL_FEATURES, NULL, &rsp);
    if (err) {
        return err;
    }
    read_local_features_complete(rsp);
    net_buf_unref(rsp);

    /* Read Local Version Information */
    err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_LOCAL_VERSION_INFO, NULL, &rsp);
    if (err) {
        return err;
    }
    read_local_ver_complete(rsp);
    net_buf_unref(rsp);

    /* Read Bluetooth Address */
    err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_BD_ADDR, NULL, &rsp);
    if (err) {
        return err;
    }
    read_bdaddr_complete(rsp);
    net_buf_unref(rsp);

    /* Read Local Supported Commands */
    err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_SUPPORTED_COMMANDS, NULL, &rsp);
    if (err) {
        return err;
    }
    read_supported_commands_complete(rsp);
    net_buf_unref(rsp);
#endif

    if (IS_ENABLED(CONFIG_BT_TINYCRYPT_ECC)) {
        /* Initialize the PRNG so that it is safe to use it later
         * on in the initialization process.
         */
        err = prng_init();
        if (err) {
            return err;
        }
    }

#if defined(CONFIG_BT_HCI_ACL_FLOW_CONTROL)
    err = set_flow_control();
    if (err) {
        return err;
    }
#endif /* CONFIG_BT_CONN */

    return 0;
}

#ifdef CONFIG_BT_USING_HCI_API
static int le_set_event_mask(void)
{
    u64_t                               mask = 0;

    mask |= BT_EVT_MASK_LE_ADVERTISING_REPORT;

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        if (IS_ENABLED(CONFIG_BT_PRIVACY) &&
            BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_ENH_CONN_COMPLETE;
        } else {
            mask |= BT_EVT_MASK_LE_CONN_COMPLETE;
        }

        mask |= BT_EVT_MASK_LE_CONN_UPDATE_COMPLETE;
        mask |= BT_EVT_MASK_LE_REMOTE_FEAT_COMPLETE;

        if (BT_FEAT_LE_CONN_PARAM_REQ_PROC(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_CONN_PARAM_REQ;
        }

        if (BT_FEAT_LE_DLE(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_DATA_LEN_CHANGE;
        }

        if (BT_FEAT_LE_PHY_2M(bt_dev.le.features) ||
            BT_FEAT_LE_PHY_CODED(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_PHY_UPDATE_COMPLETE;
        }
    }

    if (IS_ENABLED(CONFIG_BT_SMP) && BT_FEAT_LE_ENCR(bt_dev.le.features)) {
        mask |= BT_EVT_MASK_LE_LTK_REQUEST;
    }

    /*
     * If "LE Read Local P-256 Public Key" and "LE Generate DH Key" are
     * supported we need to enable events generated by those commands.
     */
    if ((bt_dev.supported_commands[34] & 0x02) &&
        (bt_dev.supported_commands[34] & 0x04)) {
        mask |= BT_EVT_MASK_LE_P256_PUBLIC_KEY_COMPLETE;
        mask |= BT_EVT_MASK_LE_GENERATE_DHKEY_COMPLETE;
    }

    return hci_api_le_set_event_mask(mask);
}

#else
static int le_set_event_mask(void)
{
    struct bt_hci_cp_le_set_event_mask *cp_mask;
    struct net_buf *                    buf;
    u64_t                               mask = 0;

    /* Set LE event mask */
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_EVENT_MASK, sizeof(*cp_mask));
    if (!buf) {
        return -ENOBUFS;
    }

    cp_mask = net_buf_add(buf, sizeof(*cp_mask));

    mask |= BT_EVT_MASK_LE_ADVERTISING_REPORT;

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        if (IS_ENABLED(CONFIG_BT_PRIVACY) &&
            BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_ENH_CONN_COMPLETE;
        } else {
            mask |= BT_EVT_MASK_LE_CONN_COMPLETE;
        }

        mask |= BT_EVT_MASK_LE_CONN_UPDATE_COMPLETE;
        mask |= BT_EVT_MASK_LE_REMOTE_FEAT_COMPLETE;

        if (BT_FEAT_LE_CONN_PARAM_REQ_PROC(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_CONN_PARAM_REQ;
        }

        if (BT_FEAT_LE_DLE(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_DATA_LEN_CHANGE;
        }

        if (BT_FEAT_LE_PHY_2M(bt_dev.le.features) ||
            BT_FEAT_LE_PHY_CODED(bt_dev.le.features)) {
            mask |= BT_EVT_MASK_LE_PHY_UPDATE_COMPLETE;
        }
    }

    if (IS_ENABLED(CONFIG_BT_SMP) && BT_FEAT_LE_ENCR(bt_dev.le.features)) {
        mask |= BT_EVT_MASK_LE_LTK_REQUEST;
    }

    /*
     * If "LE Read Local P-256 Public Key" and "LE Generate DH Key" are
     * supported we need to enable events generated by those commands.
     */
    if ((bt_dev.supported_commands[34] & 0x02) &&
        (bt_dev.supported_commands[34] & 0x04)) {
        mask |= BT_EVT_MASK_LE_P256_PUBLIC_KEY_COMPLETE;
        mask |= BT_EVT_MASK_LE_GENERATE_DHKEY_COMPLETE;
    }

    sys_put_le64(mask, cp_mask->events);
    return bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_EVENT_MASK, buf, NULL);
}
#endif

static int le_init(void)
{
    int err;

    /* For now we only support LE capable controllers */
    if (!BT_FEAT_LE(bt_dev.features)) {
        BT_ERR("Non-LE capable controller detected!");
        return -ENODEV;
    }
#ifdef CONFIG_BT_USING_HCI_API
    /* Read Low Energy Supported Features */
    err = hci_api_le_read_local_feature(bt_dev.le.features);
#else
    struct bt_hci_cp_write_le_host_supp *cp_le;
    struct net_buf *                     buf;
    struct net_buf *                     rsp;
    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_LOCAL_FEATURES, NULL, &rsp);
#endif
    if (err) {
        return err;
    }
#ifndef CONFIG_BT_USING_HCI_API
    read_le_features_complete(rsp);
    net_buf_unref(rsp);
#endif

#if defined(CONFIG_BT_CONN)
#ifdef CONFIG_BT_USING_HCI_API
    u8_t le_max_num;
    /* Read LE Buffer Size */
    err = hci_api_le_read_buffer_size_complete(&bt_dev.le.mtu, &le_max_num);

    if (err) {
        return err;
    }

    if (bt_dev.le.mtu) {
        le_max_num = min(le_max_num, CONFIG_BT_CONN_TX_MAX);
        bt_dev.le.pkts = min(le_max_num, CONFIG_BT_CONN_TX_MAX);
    }
#else
    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_BUFFER_SIZE, NULL, &rsp);
    if (err) {
        return err;
    }
    le_read_buffer_size_complete(rsp);
    net_buf_unref(rsp);
#endif

#endif
    if (BT_FEAT_BREDR(bt_dev.features)) {
#ifdef CONFIG_BT_USING_HCI_API
        err = hci_api_le_write_host_supp(0x01, 0x00);
#else
        buf = bt_hci_cmd_create(BT_HCI_OP_LE_WRITE_LE_HOST_SUPP, sizeof(*cp_le));
        if (!buf) {
            return -ENOBUFS;
        }

        cp_le = net_buf_add(buf, sizeof(*cp_le));
        /* Explicitly enable LE for dual-mode controllers */
        cp_le->le    = 0x01;
        cp_le->simul = 0x00;
        err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_WRITE_LE_HOST_SUPP, buf, NULL);
#endif
        if (err) {
            return err;
        }
    }

    /* Read LE Supported States */
    if (BT_CMD_LE_STATES(bt_dev.supported_commands)) {
#ifdef CONFIG_BT_USING_HCI_API
        err = hci_api_le_read_support_states(&bt_dev.le.states);
        if (err) {
            return err;
        }
#else
        err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_SUPP_STATES, NULL, &rsp);
        if (err) {
            return err;
        }
        le_read_supp_states_complete(rsp);
        net_buf_unref(rsp);
#endif
    }

    if (IS_ENABLED(CONFIG_BT_CONN) &&
        BT_FEAT_LE_DLE(bt_dev.le.features)) {
        u16_t tx_octets, tx_time;
#ifdef CONFIG_BT_USING_HCI_API
        err = hci_api_le_get_max_data_len(&tx_octets, &tx_time);
        if (err) {
            return err;
        }
        err = hci_api_le_set_default_data_len(tx_octets, tx_time);
#else
        struct bt_hci_cp_le_write_default_data_len *cp;
        struct bt_hci_rp_le_read_max_data_len *     rp;
        struct net_buf *                            buf, *rsp;

        err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_MAX_DATA_LEN, NULL, &rsp);
        if (err) {
            return err;
        }

        rp        = (void *)rsp->data;
        tx_octets = sys_le16_to_cpu(rp->max_tx_octets);
        tx_time   = sys_le16_to_cpu(rp->max_tx_time);
        net_buf_unref(rsp);

        buf =
          bt_hci_cmd_create(BT_HCI_OP_LE_WRITE_DEFAULT_DATA_LEN, sizeof(*cp));
        if (!buf) {
            return -ENOBUFS;
        }

        cp                = net_buf_add(buf, sizeof(*cp));
        cp->max_tx_octets = sys_cpu_to_le16(tx_octets);
        cp->max_tx_time   = sys_cpu_to_le16(tx_time);

        err =
          bt_hci_cmd_send_sync(BT_HCI_OP_LE_WRITE_DEFAULT_DATA_LEN, buf, NULL);
#endif
        if (err) {
            return err;
        }
    }

#if defined(CONFIG_BT_SMP)
    if (IS_ENABLED(CONFIG_BT_PRIVACY) &&
        BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
#ifdef CONFIG_BT_USING_HCI_API
        err = hci_api_le_read_rl_size(&bt_dev.le.rl_size);
        if (err) {
            return err;
        }
        BT_DBG("Resolving List size %u", bt_dev.le.rl_size);
#else
        struct bt_hci_rp_le_read_rl_size *rp;
        struct net_buf *                  rsp;

        err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_RL_SIZE, NULL, &rsp);
        if (err) {
            return err;
        }

        rp = (void *)rsp->data;

        BT_DBG("Resolving List size %u", rp->rl_size);

        bt_dev.le.rl_size = rp->rl_size;

        net_buf_unref(rsp);
#endif
    }

#endif

    return le_set_event_mask();
}

#ifdef CONFIG_BT_USING_HCI_API
static int set_event_mask(void)
{
    u64_t                            mask = 0;

    if (IS_ENABLED(CONFIG_BT_BREDR)) {
        /* Since we require LE support, we can count on a
         * Bluetooth 4.0 feature set
         */
        mask |= BT_EVT_MASK_INQUIRY_COMPLETE;
        mask |= BT_EVT_MASK_CONN_COMPLETE;
        mask |= BT_EVT_MASK_CONN_REQUEST;
        mask |= BT_EVT_MASK_AUTH_COMPLETE;
        mask |= BT_EVT_MASK_REMOTE_NAME_REQ_COMPLETE;
        mask |= BT_EVT_MASK_REMOTE_FEATURES;
        mask |= BT_EVT_MASK_ROLE_CHANGE;
        mask |= BT_EVT_MASK_PIN_CODE_REQ;
        mask |= BT_EVT_MASK_LINK_KEY_REQ;
        mask |= BT_EVT_MASK_LINK_KEY_NOTIFY;
        mask |= BT_EVT_MASK_INQUIRY_RESULT_WITH_RSSI;
        mask |= BT_EVT_MASK_REMOTE_EXT_FEATURES;
        mask |= BT_EVT_MASK_SYNC_CONN_COMPLETE;
        mask |= BT_EVT_MASK_EXTENDED_INQUIRY_RESULT;
        mask |= BT_EVT_MASK_IO_CAPA_REQ;
        mask |= BT_EVT_MASK_IO_CAPA_RESP;
        mask |= BT_EVT_MASK_USER_CONFIRM_REQ;
        mask |= BT_EVT_MASK_USER_PASSKEY_REQ;
        mask |= BT_EVT_MASK_SSP_COMPLETE;
        mask |= BT_EVT_MASK_USER_PASSKEY_NOTIFY;
    }

    mask |= BT_EVT_MASK_HARDWARE_ERROR;
    mask |= BT_EVT_MASK_DATA_BUFFER_OVERFLOW;
    mask |= BT_EVT_MASK_LE_META_EVENT;

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        mask |= BT_EVT_MASK_DISCONN_COMPLETE;
        mask |= BT_EVT_MASK_REMOTE_VERSION_INFO;
    }

    if (IS_ENABLED(CONFIG_BT_SMP) && BT_FEAT_LE_ENCR(bt_dev.le.features)) {
        mask |= BT_EVT_MASK_ENCRYPT_CHANGE;
        mask |= BT_EVT_MASK_ENCRYPT_KEY_REFRESH_COMPLETE;
    }

    return hci_api_set_event_mask(mask);
}

#else
static int set_event_mask(void)
{
    struct bt_hci_cp_set_event_mask *ev;
    struct net_buf *                 buf;
    u64_t                            mask = 0;

    buf = bt_hci_cmd_create(BT_HCI_OP_SET_EVENT_MASK, sizeof(*ev));
    if (!buf) {
        return -ENOBUFS;
    }

    ev = net_buf_add(buf, sizeof(*ev));

    if (IS_ENABLED(CONFIG_BT_BREDR)) {
        /* Since we require LE support, we can count on a
         * Bluetooth 4.0 feature set
         */
        mask |= BT_EVT_MASK_INQUIRY_COMPLETE;
        mask |= BT_EVT_MASK_CONN_COMPLETE;
        mask |= BT_EVT_MASK_CONN_REQUEST;
        mask |= BT_EVT_MASK_AUTH_COMPLETE;
        mask |= BT_EVT_MASK_REMOTE_NAME_REQ_COMPLETE;
        mask |= BT_EVT_MASK_REMOTE_FEATURES;
        mask |= BT_EVT_MASK_ROLE_CHANGE;
        mask |= BT_EVT_MASK_PIN_CODE_REQ;
        mask |= BT_EVT_MASK_LINK_KEY_REQ;
        mask |= BT_EVT_MASK_LINK_KEY_NOTIFY;
        mask |= BT_EVT_MASK_INQUIRY_RESULT_WITH_RSSI;
        mask |= BT_EVT_MASK_REMOTE_EXT_FEATURES;
        mask |= BT_EVT_MASK_SYNC_CONN_COMPLETE;
        mask |= BT_EVT_MASK_EXTENDED_INQUIRY_RESULT;
        mask |= BT_EVT_MASK_IO_CAPA_REQ;
        mask |= BT_EVT_MASK_IO_CAPA_RESP;
        mask |= BT_EVT_MASK_USER_CONFIRM_REQ;
        mask |= BT_EVT_MASK_USER_PASSKEY_REQ;
        mask |= BT_EVT_MASK_SSP_COMPLETE;
        mask |= BT_EVT_MASK_USER_PASSKEY_NOTIFY;
    }

    mask |= BT_EVT_MASK_HARDWARE_ERROR;
    mask |= BT_EVT_MASK_DATA_BUFFER_OVERFLOW;
    mask |= BT_EVT_MASK_LE_META_EVENT;

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        mask |= BT_EVT_MASK_DISCONN_COMPLETE;
        mask |= BT_EVT_MASK_REMOTE_VERSION_INFO;
    }

    if (IS_ENABLED(CONFIG_BT_SMP) && BT_FEAT_LE_ENCR(bt_dev.le.features)) {
        mask |= BT_EVT_MASK_ENCRYPT_CHANGE;
        mask |= BT_EVT_MASK_ENCRYPT_KEY_REFRESH_COMPLETE;
    }

    sys_put_le64(mask, ev->events);
    return bt_hci_cmd_send_sync(BT_HCI_OP_SET_EVENT_MASK, buf, NULL);
}
#endif

static inline int create_random_addr(bt_addr_le_t *addr)
{
    addr->type = BT_ADDR_LE_RANDOM;

    return bt_rand(addr->a.val, 6);
}

int bt_addr_le_create_nrpa(bt_addr_le_t *addr)
{
    int err;

    err = create_random_addr(addr);
    if (err) {
        return err;
    }

    BT_ADDR_SET_NRPA(&addr->a);

    return 0;
}

int bt_addr_le_create_static(bt_addr_le_t *addr)
{
    int err;

    err = create_random_addr(addr);
    if (err) {
        return err;
    }

    BT_ADDR_SET_STATIC(&addr->a);

    return 0;
}

/*[Genie begin] change by lgy at 2020-09-16*/
extern uint8_t *genie_triple_get_mac(void);

/*Robin modified to set public or static address*/
static int set_bd_addr(void)
{
    int err;

    if (bt_storage) {
#if 0
        ssize_t ret;

        ret = bt_storage->read(NULL, BT_STORAGE_ID_ADDR, &bt_dev.id_addr,
                               sizeof(bt_dev.id_addr));
        if (ret == sizeof(bt_dev.id_addr)) {
            if ((bt_dev.id_addr.a.val[5] & 0xc0) == 0xc0){
                bt_dev.id_addr.type = BT_ADDR_LE_RANDOM;
            }
            else{
                bt_dev.id_addr.type = BT_ADDR_LE_PUBLIC;
            }
            goto set_addr;
        }
#else
        memcpy(bt_dev.id_addr.a.val, genie_triple_get_mac(), 6);
        bt_dev.id_addr.type = BT_ADDR_LE_PUBLIC;
        goto set_addr;
#endif
    }

#if defined(CONFIG_BT_HCI_VS_EXT)
    /* Check for VS_Read_Static_Addresses support */
    if (bt_dev.vs_commands[1] & BIT(0)) {
        struct bt_hci_rp_vs_read_static_addrs *rp;
        struct net_buf *                       rsp;

        err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_READ_STATIC_ADDRS, NULL, &rsp);
        if (err) {
            BT_WARN("Failed to read static addresses");
            goto generate;
        }

        rp = (void *)rsp->data;
        if (rp->num_addrs) {
            bt_dev.id_addr.type = BT_ADDR_LE_RANDOM;
            bt_addr_copy(&bt_dev.id_addr.a, &rp->a[0].bdaddr);
            net_buf_unref(rsp);
            goto set_addr;
        }

        BT_WARN("No static addresses stored in controller");
        net_buf_unref(rsp);
    } else {
        BT_WARN("Read Static Addresses command not available");
    }

generate:
#endif
    BT_DBG("Generating new static random address");

    err = bt_addr_le_create_static(&bt_dev.id_addr);
    if (err) {
        return err;
    }

    bt_dev.id_addr.type = BT_ADDR_LE_RANDOM;

    if (bt_storage) {
        ssize_t ret;

        ret = bt_storage->write(NULL, BT_STORAGE_ID_ADDR, &bt_dev.id_addr,
                                sizeof(bt_dev.id_addr));
        if (ret != sizeof(bt_dev.id_addr)) {
            BT_ERR("Unable to store static address");
        }
    } else {
        BT_WARN("Using temporary static random address");
    }

set_addr:
    if (bt_dev.id_addr.type == BT_ADDR_LE_RANDOM)
    {
        err = set_random_address(&bt_dev.id_addr.a);
    }
    else
    {
        err = set_public_address(&bt_dev.id_addr.a);
    }
    if (err) {
        return err;
    }

    atomic_set_bit(bt_dev.flags, BT_DEV_ID_STATIC_RANDOM);
    return 0;
}

#if defined(CONFIG_BT_DEBUG) && BT_DBG_ENABLED
static const char *ver_str(u8_t ver)
{
    const char *const str[] = {
        "1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1", "4.2", "5.0",
    };

    if (ver < ARRAY_SIZE(str)) {
        return str[ver];
    }

    return "unknown";
}

static void show_dev_info(void)
{
    BT_INFO("Identity: %s", bt_addr_le_str(&bt_dev.id_addr));
    BT_INFO("HCI: version %s (0x%02x) revision 0x%04x, manufacturer 0x%04x",
            ver_str(bt_dev.hci_version), bt_dev.hci_version,
            bt_dev.hci_revision, bt_dev.manufacturer);
    BT_INFO("LMP: version %s (0x%02x) subver 0x%04x",
            ver_str(bt_dev.lmp_version), bt_dev.lmp_version,
            bt_dev.lmp_subversion);
}
#else
static inline void show_dev_info(void) {}
#endif /* CONFIG_BT_DEBUG */

#if defined(CONFIG_BT_HCI_VS_EXT)
#if defined(CONFIG_BT_DEBUG) && BT_DBG_ENABLED
static const char *vs_hw_platform(u16_t platform)
{
    static const char *const plat_str[] = { "reserved", "Intel Corporation",
                                            "Nordic Semiconductor",
                                            "NXP Semiconductors" };

    if (platform < ARRAY_SIZE(plat_str)) {
        return plat_str[platform];
    }

    return "unknown";
}

static const char *vs_hw_variant(u16_t platform, u16_t variant)
{
    static const char *const nordic_str[] = { "reserved", "nRF51x", "nRF52x" };

    if (platform != BT_HCI_VS_HW_PLAT_NORDIC) {
        return "unknown";
    }

    if (variant < ARRAY_SIZE(nordic_str)) {
        return nordic_str[variant];
    }

    return "unknown";
}

static const char *vs_fw_variant(u8_t variant)
{
    static const char *const var_str[] = {
        "Standard Bluetooth controller",
        "Vendor specific controller",
        "Firmware loader",
        "Rescue image",
    };

    if (variant < ARRAY_SIZE(var_str)) {
        return var_str[variant];
    }

    return "unknown";
}
#endif /* CONFIG_BT_DEBUG */

static void hci_vs_init(void)
{
    union
    {
        struct bt_hci_rp_vs_read_version_info *      info;
        struct bt_hci_rp_vs_read_supported_commands *cmds;
        struct bt_hci_rp_vs_read_supported_features *feat;
    } rp;
    struct net_buf *rsp;
    int             err;

    /* If heuristics is enabled, try to guess HCI VS support by looking
     * at the HCI version and identity address. We haven't tried to set
     * a static random address yet at this point, so the identity will
     * either be zeroes or a valid public address.
     */
    if (IS_ENABLED(CONFIG_BT_HCI_VS_EXT_DETECT) &&
        (bt_dev.hci_version < BT_HCI_VERSION_5_0 ||
         bt_addr_le_cmp(&bt_dev.id_addr, BT_ADDR_LE_ANY))) {
        BT_WARN("Controller doesn't seem to support Zephyr vendor HCI");
        return;
    }

    err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_READ_VERSION_INFO, NULL, &rsp);
    if (err) {
        BT_WARN("Vendor HCI extensions not available");
        return;
    }

#if defined(CONFIG_BT_DEBUG) && BT_DBG_ENABLED
    rp.info = (void *)rsp->data;
    BT_INFO("HW Platform: %s (0x%04x)",
            vs_hw_platform(sys_le16_to_cpu(rp.info->hw_platform)),
            sys_le16_to_cpu(rp.info->hw_platform));
    BT_INFO("HW Variant: %s (0x%04x)",
            vs_hw_variant(sys_le16_to_cpu(rp.info->hw_platform),
                          sys_le16_to_cpu(rp.info->hw_variant)),
            sys_le16_to_cpu(rp.info->hw_variant));
    BT_INFO("Firmware: %s (0x%02x) Version %u.%u Build %u",
            vs_fw_variant(rp.info->fw_variant), rp.info->fw_variant,
            rp.info->fw_version, sys_le16_to_cpu(rp.info->fw_revision),
            sys_le32_to_cpu(rp.info->fw_build));
#endif /* CONFIG_BT_DEBUG */

    net_buf_unref(rsp);

    err =
      bt_hci_cmd_send_sync(BT_HCI_OP_VS_READ_SUPPORTED_COMMANDS, NULL, &rsp);
    if (err) {
        BT_WARN("Failed to read supported vendor features");
        return;
    }

    rp.cmds = (void *)rsp->data;
    memcpy(bt_dev.vs_commands, rp.cmds->commands, BT_DEV_VS_CMDS_MAX);
    net_buf_unref(rsp);

    err =
      bt_hci_cmd_send_sync(BT_HCI_OP_VS_READ_SUPPORTED_FEATURES, NULL, &rsp);
    if (err) {
        BT_WARN("Failed to read supported vendor commands");
        return;
    }

    rp.feat = (void *)rsp->data;
    memcpy(bt_dev.vs_features, rp.feat->features, BT_DEV_VS_FEAT_MAX);
    net_buf_unref(rsp);
}
#endif /* CONFIG_BT_HCI_VS_EXT */

static int hci_init(void)
{
    int err;

    err = common_init();
    if (err) {
        return err;
    }

    err = le_init();
    if (err) {
        return err;
    }

    err = set_event_mask();
    if (err) {
        return err;
    }

#if defined(CONFIG_BT_HCI_VS_EXT)
    hci_vs_init();
#endif

    if (!bt_addr_le_cmp(&bt_dev.id_addr, BT_ADDR_LE_ANY) ||
        !bt_addr_le_cmp(&bt_dev.id_addr, BT_ADDR_LE_NONE)) {
        BT_DBG("No address. Trying to set addr from storage.");
        err = set_bd_addr();
        if (err) {
            BT_ERR("Unable to set identity address");
            return err;
        }
    }

#if defined(CONFIG_BT_DEBUG) && BT_DBG_ENABLED
    show_dev_info();
#endif
    return 0;
}

int bt_send(struct net_buf *buf)
{
    BT_DBG("buf %p len %u type %u", buf, buf->len, bt_buf_get_type(buf));

    bt_monitor_send(bt_monitor_opcode(buf), buf->data, buf->len);

    if (IS_ENABLED(CONFIG_BT_TINYCRYPT_ECC)) {
        return bt_hci_ecc_send(buf);
    }

    return bt_dev.drv->send(buf);
}

int bt_recv(struct net_buf *buf)
{
    struct net_buf_pool *pool;

    bt_monitor_send(bt_monitor_opcode(buf), buf->data, buf->len);

    BT_DBG("%s, buf %p len %u", __func__, buf, buf->len);

    pool = net_buf_pool_get(buf->pool_id);
    if (pool->user_data_size < BT_BUF_USER_DATA_MIN) {
        BT_ERR("Too small user data size");
        net_buf_unref(buf);
        return -EINVAL;
    }

    switch (bt_buf_get_type(buf)) {
#if defined(CONFIG_BT_CONN)
        case BT_BUF_ACL_IN:
#if !defined(CONFIG_BT_HOST_RX_THREAD)
            hci_acl(buf);
#else
            net_buf_put(&bt_dev.rx_queue, buf);
#endif
            return 0;
#endif /* BT_CONN */
        case BT_BUF_EVT:
#if !defined(CONFIG_BT_HOST_RX_THREAD)
            hci_event(buf);
#else
            net_buf_put(&bt_dev.rx_queue, buf);
#endif
            return 0;
        default:
            BT_ERR("Invalid buf type %u", bt_buf_get_type(buf));
            net_buf_unref(buf);
            return -EINVAL;
    }
}

int bt_recv_prio(struct net_buf *buf)
{
    struct bt_hci_evt_hdr *hdr = (void *)buf->data;

    bt_monitor_send(bt_monitor_opcode(buf), buf->data, buf->len);

    BT_ASSERT(bt_buf_get_type(buf) == BT_BUF_EVT);
    BT_ASSERT(buf->len >= sizeof(*hdr));
    BT_ASSERT(bt_hci_evt_is_prio(hdr->evt));

    net_buf_pull(buf, sizeof(*hdr));

    switch (hdr->evt) {
        case BT_HCI_EVT_CMD_COMPLETE:
            hci_cmd_complete(buf);
            break;
        case BT_HCI_EVT_CMD_STATUS:
            hci_cmd_status(buf);
            break;
#if defined(CONFIG_BT_CONN)
        case BT_HCI_EVT_NUM_COMPLETED_PACKETS:
            hci_num_completed_packets(buf);
            break;
#endif /* CONFIG_BT_CONN */
        default:
            net_buf_unref(buf);
            BT_ASSERT(0);
            return -EINVAL;
    }

    net_buf_unref(buf);

    return 0;
}

int bt_hci_driver_register(const struct bt_hci_driver *drv)
{
    if (bt_dev.drv) {
        return -EALREADY;
    }

    if (!drv->open || !drv->send) {
        return -EINVAL;
    }

    bt_dev.drv = drv;

    BT_DBG("Registered %s", drv->name ? drv->name : "");

    bt_monitor_new_index(BT_MONITOR_TYPE_PRIMARY, drv->bus, BT_ADDR_ANY,
                         drv->name ? drv->name : "bt0");

    return 0;
}

#if defined(CONFIG_BT_PRIVACY)
static int irk_init(void)
{
    ssize_t err;

    if (bt_storage) {
        err = bt_storage->read(NULL, BT_STORAGE_LOCAL_IRK, &bt_dev.irk,
                               sizeof(bt_dev.irk));
        if (err == sizeof(bt_dev.irk)) {
            return 0;
        }
    }

    BT_DBG("Generating new IRK");

    err = bt_rand(bt_dev.irk, sizeof(bt_dev.irk));
    if (err) {
        return err;
    }

    if (bt_storage) {
        err = bt_storage->write(NULL, BT_STORAGE_LOCAL_IRK, bt_dev.irk,
                                sizeof(bt_dev.irk));
        if (err != sizeof(bt_dev.irk)) {
            BT_ERR("Unable to store IRK");
        }
    } else {
        BT_WARN("Using temporary IRK");
    }

    return 0;
}
#endif /* CONFIG_BT_PRIVACY */

static int bt_init(void)
{
    int err;

    err = hci_init();
    if (err) {
        return err;
    }

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        err = bt_conn_init();
        if (err) {
            return err;
        }
    }

#if defined(CONFIG_BT_PRIVACY)
    err = irk_init();
    if (err) {
        return err;
    }

    k_delayed_work_init(&bt_dev.rpa_update, rpa_timeout);
#endif

    bt_monitor_send(BT_MONITOR_OPEN_INDEX, NULL, 0);
    atomic_set_bit(bt_dev.flags, BT_DEV_READY);
    bt_le_scan_update(false);

    return 0;
}

static void init_work(struct k_work *work)
{
    int err;

    err = bt_init();
    if (ready_cb) {
        ready_cb(err);
    }
}

#if defined(CONFIG_BT_HOST_RX_THREAD)
static void hci_rx_thread(void)
{
    struct net_buf *buf;

    BT_DBG("started");

    while (1) {
        buf = net_buf_get(&bt_dev.rx_queue, K_NO_WAIT);
        if (buf == NULL) {
            aos_msleep(10);
            continue;
        }

        BT_DBG("buf %p type %u len %u", buf, bt_buf_get_type(buf), buf->len);

        switch (bt_buf_get_type(buf)) {
#if defined(CONFIG_BT_CONN)
            case BT_BUF_ACL_IN:
                hci_acl(buf);
                break;
#endif /* CONFIG_BT_CONN */
            case BT_BUF_EVT:
                hci_event(buf);
                break;
            default:
                BT_ERR("Unknown buf type %u", bt_buf_get_type(buf));
                net_buf_unref(buf);
                break;
        }

        /* Make sure we don't hog the CPU if the rx_queue never
         * gets empty.
         */
        k_yield();
    }
}
#endif

int bt_enable(bt_ready_cb_t cb)
{
    int err;

    if (!bt_dev.drv) {
        BT_ERR("No HCI driver registered");
        return -ENODEV;
    }

    if (atomic_test_and_set_bit(bt_dev.flags, BT_DEV_ENABLE)) {
        return -EALREADY;
    }

    k_work_init(&bt_dev.init, init_work);
    k_work_q_start();

    ready_cb = cb;

#if defined(CONFIG_BT_HOST_RX_THREAD)
    /* RX thread */
    k_thread_create(&rx_thread_data, rx_thread_stack,
                    K_THREAD_STACK_SIZEOF(rx_thread_stack),
                    (k_thread_entry_t)hci_rx_thread, "hci_rx", NULL, NULL,
                    CONFIG_BT_RX_PRIO, 0, K_NO_WAIT);
#endif

    if (IS_ENABLED(CONFIG_BT_TINYCRYPT_ECC)) {
        bt_hci_ecc_init();
    }

    err = bt_dev.drv->open();
    if (err) {
        BT_ERR("HCI driver open failed (%d)", err);
        return err;
    }

    k_work_submit(&bt_dev.init);

    /* TX thread */
    k_thread_create(&tx_thread_data, tx_thread_stack,
                    K_THREAD_STACK_SIZEOF(tx_thread_stack), hci_tx_thread, "hci_tx",
                    NULL, NULL, CONFIG_BT_HCI_TX_PRIO, 0, K_NO_WAIT);

    return 0;
}

bool bt_addr_le_is_bonded(const bt_addr_le_t *addr)
{
    if (IS_ENABLED(CONFIG_BT_SMP)) {
        struct bt_keys *keys = bt_keys_find_addr(addr);

        /* if there are any keys stored then device is bonded */
        return keys && keys->keys;
    } else {
        return false;
    }
}

static bool valid_adv_param(const struct bt_le_adv_param *param)
{
    if (!(param->options & BT_LE_ADV_OPT_CONNECTABLE)) {
        /*
         * BT Core 4.2 [Vol 2, Part E, 7.8.5]
         * The Advertising_Interval_Min and Advertising_Interval_Max
         * shall not be set to less than 0x00A0 (100 ms) if the
         * Advertising_Type is set to ADV_SCAN_IND or ADV_NONCONN_IND.
         */
        if (bt_dev.hci_version < BT_HCI_VERSION_5_0 &&
            param->interval_min < 0x00a0) {
            /* not check here any more */
            #if 0   //!defined(CONFIG_HSE)
            /* force fast adv even if BT 4.0 */
            return false;
            #endif
        }
    }

    if (param->interval_min > param->interval_max ||
        param->interval_min < 0x0020 || param->interval_max > 0x4000) {
        return false;
    }

    return true;
}

static int set_ad(u16_t hci_op, const struct bt_data *ad, size_t ad_len)
{
#ifdef CONFIG_BT_USING_HCI_API
    struct bt_hci_cp_le_set_adv_data set_data;
    int i;

    memset(&set_data, 0, sizeof(set_data));

    for (i = 0; i < ad_len; i++) {

           /* Check if ad fit in the remaining buffer */
            if (set_data.len + ad[i].data_len + 2 > 31) {
                    return -EINVAL;
            }

            set_data.data[set_data.len++] = ad[i].data_len + 1;
            set_data.data[set_data.len++] = ad[i].type;
            memcpy(&set_data.data[set_data.len], ad[i].data,ad[i].data_len);
            set_data.len += ad[i].data_len;
        }

    if (hci_op == BT_HCI_OP_LE_SET_ADV_DATA) {
        return hci_api_le_set_ad_data(set_data.len, set_data.data);
    } else if (hci_op == BT_HCI_OP_LE_SET_SCAN_RSP_DATA) {
        return hci_api_le_set_sd_data(set_data.len, set_data.data);
    }

    return 0;
#else
    struct bt_hci_cp_le_set_adv_data *set_data;
    struct net_buf *                  buf;
    int                               i;

    buf = bt_hci_cmd_create(hci_op, sizeof(*set_data));
    if (!buf) {
        return -ENOBUFS;
    }

    set_data = net_buf_add(buf, sizeof(*set_data));

    memset(set_data, 0, sizeof(*set_data));

    for (i = 0; i < ad_len; i++) {
        /* Check if ad fit in the remaining buffer */
        if (set_data->len + ad[i].data_len + 2 > 31) {
            net_buf_unref(buf);
            return -EINVAL;
        }

        set_data->data[set_data->len++] = ad[i].data_len + 1;
        set_data->data[set_data->len++] = ad[i].type;

        memcpy(&set_data->data[set_data->len], ad[i].data, ad[i].data_len);
        set_data->len += ad[i].data_len;
    }

    return bt_hci_cmd_send_sync(hci_op, buf, NULL);
#endif
}

static int set_ad_data(u16_t hci_op, const uint8_t *ad_data, int ad_len)
{
#ifdef CONFIG_BT_USING_HCI_API
    if (hci_op == BT_HCI_OP_LE_SET_ADV_DATA) {
        return hci_api_le_set_ad_data(ad_len, (uint8_t *)ad_data);
    } else if (hci_op == BT_HCI_OP_LE_SET_SCAN_RSP_DATA) {
        return hci_api_le_set_sd_data(ad_len, (uint8_t *)ad_data);
    }
#else
    struct bt_hci_cp_le_set_adv_data *set_data;
    struct net_buf *                  buf;

    buf = bt_hci_cmd_create(hci_op, sizeof(*set_data));
    if (!buf) {
        return -ENOBUFS;
    }

    if (ad_len > 31)
        return -EINVAL;

    set_data = net_buf_add(buf, sizeof(*set_data));

    memset(set_data, 0, sizeof(*set_data));
    memcpy(set_data->data, ad_data, ad_len);
    set_data->len = ad_len;

    return bt_hci_cmd_send_sync(hci_op, buf, NULL);
#endif

    return 0;
}

int bt_le_adv_start(const struct bt_le_adv_param *param,
                    const struct bt_data *ad, size_t ad_len,
                    const struct bt_data *sd, size_t sd_len)
{
    struct bt_hci_cp_le_set_adv_param set_param;
#ifndef CONFIG_BT_USING_HCI_API
    struct net_buf *                  buf;
#endif
    int                               err;

    if (!valid_adv_param(param)) {
        return -EINVAL;
    }

    if (atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        return -EALREADY;
    }

    err = set_ad(BT_HCI_OP_LE_SET_ADV_DATA, ad, ad_len);
    if (err) {
        return err;
    }

    /*
     * We need to set SCAN_RSP when enabling advertising type that allows
     * for Scan Requests.
     *
     * If sd was not provided but we enable connectable undirected
     * advertising sd needs to be cleared from values set by previous calls.
     * Clearing sd is done by calling set_ad() with NULL data and zero len.
     * So following condition check is unusual but correct.
     */
    if (sd || (param->options & BT_LE_ADV_OPT_CONNECTABLE)) {
        err = set_ad(BT_HCI_OP_LE_SET_SCAN_RSP_DATA, sd, sd_len);
        if (err) {
            return err;
        }
    }

    memset(&set_param, 0, sizeof(set_param));

    set_param.min_interval = sys_cpu_to_le16(param->interval_min);
    set_param.max_interval = sys_cpu_to_le16(param->interval_max);
    set_param.channel_map  = 0x07;

    if (param->options & BT_LE_ADV_OPT_CONNECTABLE) {
        if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
            err = le_set_private_addr();
            if (err) {
                return err;
            }

            if (BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
                set_param.own_addr_type = BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
            } else {
                set_param.own_addr_type = BT_ADDR_LE_RANDOM;
            }
        } else {
            /*
             * If Static Random address is used as Identity
             * address we need to restore it before advertising
             * is enabled. Otherwise NRPA used for active scan
             * could be used for advertising.
             */
            if (atomic_test_bit(bt_dev.flags, BT_DEV_ID_STATIC_RANDOM)) {
                set_random_address(&bt_dev.id_addr.a);
            }

            set_param.own_addr_type = bt_dev.id_addr.type;
        }

        set_param.type = BT_LE_ADV_IND;
    } else {
#if 0
        if (param->own_addr) {
            /* Only NRPA is allowed */
            if (!BT_ADDR_IS_NRPA(param->own_addr)) {
                return -EINVAL;
            }

            err = set_random_address(param->own_addr);
        } else {
            err = le_set_private_addr();
        }

        if (err) {
            return err;
        }

        /*set_param.own_addr_type = BT_ADDR_LE_RANDOM;*/
        set_param.own_addr_type = BT_ADDR_LE_PUBLIC;  /* tmall use public address for adv */
#endif

        if (sd) {
            set_param.type = BT_LE_ADV_SCAN_IND;
        } else {
            set_param.type = BT_LE_ADV_NONCONN_IND;
        }
    }
#ifdef CONFIG_BT_USING_HCI_API
    err = hci_api_le_adv_param(set_param.min_interval,
                               set_param.max_interval,
                               set_param.type,
                               set_param.own_addr_type,
                               set_param.direct_addr.type,
                               set_param.direct_addr.a.val,
                               set_param.channel_map,
                               set_param.filter_policy);
#else
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_ADV_PARAM, sizeof(set_param));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, &set_param, sizeof(set_param));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_ADV_PARAM, buf, NULL);
#endif
    if (err) {
        return err;
    }

    err = set_advertise_enable(true);
    if (err) {
        return err;
    }

    if (!(param->options & BT_LE_ADV_OPT_ONE_TIME)) {
        atomic_set_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING);
    }

    return 0;
}

int bt_le_adv_stop(void)
{
    int err;

    /* Make sure advertising is not re-enabled later even if it's not
     * currently enabled (i.e. BT_DEV_ADVERTISING is not set).
     */
    atomic_clear_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING);

    if (!atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        return 0;
    }

    err = set_advertise_enable(false);
    if (err) {
        return err;
    }

    if (!IS_ENABLED(CONFIG_BT_PRIVACY)) {
        /* If active scan is ongoing set NRPA */
        if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING) &&
            atomic_test_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN)) {
            le_set_private_addr();
        }
    }

    return 0;
}

int bt_le_adv_start_instant(const struct bt_le_adv_param *param,
        const uint8_t *ad_data, size_t ad_len,
        const uint8_t *sd_data, size_t sd_len)
{
    struct bt_hci_cp_le_set_adv_param set_param;
    int                               err;

    bt_le_adv_stop_instant();

    if (!valid_adv_param(param)) {
        return -EINVAL;
    }

    if (atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        return -EALREADY;
    }

    err = set_ad_data(BT_HCI_OP_LE_SET_ADV_DATA, ad_data, ad_len);
    if (err) {
        return err;
    }

    /*
     * We need to set SCAN_RSP when enabling advertising type that allows
     * for Scan Requests.
     *
     * If sd was not provided but we enable connectable undirected
     * advertising sd needs to be cleared from values set by previous calls.
     * Clearing sd is done by calling set_ad() with NULL data and zero len.
     * So following condition check is unusual but correct.
     */
    if (sd_len || (param->options & BT_LE_ADV_OPT_CONNECTABLE)) {
        err = set_ad_data(BT_HCI_OP_LE_SET_SCAN_RSP_DATA, sd_data, sd_len);
        if (err) {
            return err;
        }
    }

    memset(&set_param, 0, sizeof(set_param));

    set_param.min_interval = sys_cpu_to_le16(param->interval_min);
    set_param.max_interval = sys_cpu_to_le16(param->interval_max);
    set_param.channel_map  = 0x07;

    if (param->options & BT_LE_ADV_OPT_CONNECTABLE) {
        if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
            err = le_set_private_addr();
            if (err) {
                return err;
            }

            if (BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
                set_param.own_addr_type = BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
            } else {
                set_param.own_addr_type = BT_ADDR_LE_RANDOM;
            }
        } else {
            /*
             * If Static Random address is used as Identity
             * address we need to restore it before advertising
             * is enabled. Otherwise NRPA used for active scan
             * could be used for advertising.
             */
            if (atomic_test_bit(bt_dev.flags, BT_DEV_ID_STATIC_RANDOM)) {
                set_random_address(&bt_dev.id_addr.a);
            }

            set_param.own_addr_type = bt_dev.id_addr.type;
        }

        set_param.type = BT_LE_ADV_IND;
    } else {
#if 0
        if (param->own_addr) {
            /* Only NRPA is allowed */
            if (!BT_ADDR_IS_NRPA(param->own_addr)) {
                return -EINVAL;
            }

            err = set_random_address(param->own_addr);
        } else {
            err = le_set_private_addr();
        }

        if (err) {
            return err;
        }

        /*set_param.own_addr_type = BT_ADDR_LE_RANDOM;*/
        set_param.own_addr_type = BT_ADDR_LE_PUBLIC;  /* tmall use public address for adv */
#endif

        if (sd_len) {
            set_param.type = BT_LE_ADV_SCAN_IND;
        } else {
            set_param.type = BT_LE_ADV_NONCONN_IND;
        }
    }
    
#ifdef CONFIG_BT_USING_HCI_API
        err = hci_api_le_adv_param(set_param.min_interval,
                                   set_param.max_interval,
                                   set_param.type,
                                   set_param.own_addr_type,
                                   set_param.direct_addr.type,
                                   set_param.direct_addr.a.val,
                                   set_param.channel_map,
                                   set_param.filter_policy);
#else
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_ADV_PARAM, sizeof(set_param));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, &set_param, sizeof(set_param));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_ADV_PARAM, buf, NULL);
#endif
    if (err) {
        return err;
    }

    err = set_advertise_enable(true);
    if (err) {
        return err;
    }
    
    if (!(param->options & BT_LE_ADV_OPT_ONE_TIME)) {
        atomic_set_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING);
    }
    
    return 0;
}

int bt_le_adv_stop_instant(void)
{
    return bt_le_adv_stop();
}

static bool valid_le_scan_param(const struct bt_le_scan_param *param)
{
    if (param->type != BT_HCI_LE_SCAN_PASSIVE &&
        param->type != BT_HCI_LE_SCAN_ACTIVE) {
        return false;
    }

    if (param->filter_dup != BT_HCI_LE_SCAN_FILTER_DUP_DISABLE &&
        param->filter_dup != BT_HCI_LE_SCAN_FILTER_DUP_ENABLE) {
        return false;
    }

    if (param->interval < 0x0004 || param->interval > 0x4000) {
        return false;
    }

    if (param->window < 0x0004 || param->window > 0x4000) {
        return false;
    }

    if (param->window > param->interval) {
        return false;
    }

    return true;
}

int bt_le_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb)
{
    int err;

    /* Check that the parameters have valid values */
    if (!valid_le_scan_param(param)) {
        return -EINVAL;
    }

    /* Return if active scan is already enabled */
    if (atomic_test_and_set_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return -EALREADY;
    }

    if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING)) {
        err = set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE);
        if (err) {
            atomic_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN);
            return err;
        }
    }

    if (param->filter_dup) {
        atomic_set_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);
    } else {
        atomic_clear_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);
    }

    err = start_le_scan(param->type, param->interval, param->window);
    if (err) {
        atomic_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN);
        return err;
    }

    scan_dev_found_cb = cb;

    return 0;
}

int bt_le_scan_stop(void)
{
    /* Return if active scanning is already disabled */
    if (!atomic_test_and_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return -EALREADY;
    }

    scan_dev_found_cb = NULL;

    return bt_le_scan_update(false);
}

struct net_buf *bt_buf_get_rx(enum bt_buf_type type, s32_t timeout)
{
    struct net_buf *buf;

    __ASSERT(type == BT_BUF_EVT || type == BT_BUF_ACL_IN,
             "Invalid buffer type requested");

#if defined(CONFIG_BT_HCI_ACL_FLOW_CONTROL)
    if (type == BT_BUF_EVT) {
        buf = net_buf_alloc(&hci_rx_pool, timeout);
    } else {
        buf = net_buf_alloc(&acl_in_pool, timeout);
    }
#else
    buf = net_buf_alloc(&hci_rx_pool, timeout);
#endif

    if (buf) {
        net_buf_reserve(buf, CONFIG_BT_HCI_RESERVE);
        bt_buf_set_type(buf, type);
    }
    return buf;
}

struct net_buf *bt_buf_get_cmd_complete(s32_t timeout)
{
    struct net_buf *buf;
    unsigned int key;

    key = irq_lock();
    buf = bt_dev.sent_cmd;
    bt_dev.sent_cmd = NULL;
    irq_unlock(key);

    BT_DBG("sent_cmd %p", buf);

    if (buf) {
        bt_buf_set_type(buf, BT_BUF_EVT);
        buf->len = 0;
        net_buf_reserve(buf, CONFIG_BT_HCI_RESERVE);

        return buf;
    }

    return bt_buf_get_rx(BT_BUF_EVT, timeout);
}

void bt_storage_register(const struct bt_storage *storage)
{
    bt_storage = storage;
}

static int bt_storage_clear_all(void)
{
    if (IS_ENABLED(CONFIG_BT_CONN)) {
        bt_conn_disconnect_all();
    }

    if (IS_ENABLED(CONFIG_BT_SMP)) {
        bt_keys_clear_all();
    }

    if (bt_storage) {
        return bt_storage->clear(NULL);
    }

    return 0;
}

int bt_storage_clear(const bt_addr_le_t *addr)
{
    if (!addr) {
        return bt_storage_clear_all();
    }

    if (IS_ENABLED(CONFIG_BT_CONN)) {
        struct bt_conn *conn = bt_conn_lookup_addr_le(addr);
        if (conn) {
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            bt_conn_unref(conn);
        }
    }

    if (IS_ENABLED(CONFIG_BT_SMP)) {
        struct bt_keys *keys = bt_keys_find_addr(addr);
        if (keys) {
            bt_keys_clear(keys);
        }
    }

    if (bt_storage) {
        return bt_storage->clear(addr);
    }

    return 0;
}

u16_t bt_hci_get_cmd_opcode(struct net_buf *buf)
{
    return cmd(buf)->opcode;
}

int bt_pub_key_gen(struct bt_pub_key_cb *new_cb)
{
    struct bt_pub_key_cb *cb;
    int                   err;

    /*
     * We check for both "LE Read Local P-256 Public Key" and
     * "LE Generate DH Key" support here since both commands are needed for
     * ECC support. If "LE Generate DH Key" is not supported then there
     * is no point in reading local public key.
     */
    if (IS_ENABLED(CONFIG_BT_TINYCRYPT_ECC)){
    if (!(bt_dev.supported_commands[34] & 0x02) ||
        !(bt_dev.supported_commands[34] & 0x04)) {
        BT_WARN("ECC HCI commands not available");
        return -ENOTSUP;
    }
    }
    new_cb->_next = pub_key_cb;
    pub_key_cb    = new_cb;

    if (atomic_test_and_set_bit(bt_dev.flags, BT_DEV_PUB_KEY_BUSY)) {
        return 0;
    }

    atomic_clear_bit(bt_dev.flags, BT_DEV_HAS_PUB_KEY);

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_P256_PUBLIC_KEY, NULL, NULL);
    if (err) {
        BT_ERR("Sending LE P256 Public Key command failed");
        atomic_clear_bit(bt_dev.flags, BT_DEV_PUB_KEY_BUSY);
        pub_key_cb = NULL;
        return err;
    }

    for (cb = pub_key_cb; cb; cb = cb->_next) {
        if (cb != new_cb) {
            cb->func(NULL);
        }
    }

    return 0;
}

const u8_t *bt_pub_key_get(void)
{
    if (atomic_test_bit(bt_dev.flags, BT_DEV_HAS_PUB_KEY)) {
        return pub_key;
    }

    return NULL;
}

int bt_dh_key_gen(const u8_t remote_pk[64], bt_dh_key_cb_t cb)
{
    struct bt_hci_cp_le_generate_dhkey *cp;
    struct net_buf *                    buf;
    int                                 err;

    if (dh_key_cb || atomic_test_bit(bt_dev.flags, BT_DEV_PUB_KEY_BUSY)) {
        return -EBUSY;
    }

    if (!atomic_test_bit(bt_dev.flags, BT_DEV_HAS_PUB_KEY)) {
        return -EADDRNOTAVAIL;
    }

    dh_key_cb = cb;

    buf = bt_hci_cmd_create(BT_HCI_OP_LE_GENERATE_DHKEY, sizeof(*cp));
    if (!buf) {
        dh_key_cb = NULL;
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    memcpy(cp->key, remote_pk, sizeof(cp->key));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_GENERATE_DHKEY, buf, NULL);
    if (err) {
        dh_key_cb = NULL;
        return err;
    }

    return 0;
}

int bt_le_oob_get_local(struct bt_le_oob *oob)
{
    if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
        int err;

        /* Invalidate RPA so a new one is generated */
        atomic_clear_bit(bt_dev.flags, BT_DEV_RPA_VALID);

        err = le_set_private_addr();
        if (err) {
            return err;
        }

        bt_addr_le_copy(&oob->addr, &bt_dev.random_addr);
    } else {
        bt_addr_le_copy(&oob->addr, &bt_dev.id_addr);
    }

    return 0;
}

int bt_mac_addr_get(bt_addr_le_t *addr)
{
    struct bt_le_oob oob;
    if(!addr)
        return -1;
    if(bt_le_oob_get_local(&oob) != 0) {
        return -1;
    }
    memcpy(addr, &(oob.addr), sizeof(oob.addr));
    return 0;
}
