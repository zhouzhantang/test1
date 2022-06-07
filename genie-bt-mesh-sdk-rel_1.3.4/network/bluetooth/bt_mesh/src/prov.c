/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <errno.h>
#include <atomic.h>
#include <misc/util.h>
#include <misc/byteorder.h>

#include <net/buf.h>
#include <bluetooth.h>
#include <conn.h>
#include <api/mesh.h>
#include <uuid.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_PROV)
#include "common/log.h"

#ifdef MESH_DEBUG_PROV
#define ALI_PROV_TAG "\t[ALI_PROV]"
#define PROV_D(f, ...) printf("\033[0;33m%d " ALI_PROV_TAG "[D] %s " f "\033[0m\n", (u32_t)aos_now_ms(), __func__, ##__VA_ARGS__)
#else
#define PROV_D(f, ...)
#endif
#include "ecc.h"

#include "port/mesh_hal_ble.h"
#include "mesh_crypto.h"
#include "adv.h"
#include "mesh.h"
#include "net.h"
#include "access.h"
#include "foundation.h"
#include "proxy.h"
#include "prov.h"

#include "bt_mesh_custom_log.h"

/* 4 transmissions, 20ms interval */
#define PROV_XMIT_COUNT 0
#define PROV_XMIT_INT 20

#define AUTH_METHOD_NO_OOB 0x00
#define AUTH_METHOD_STATIC 0x01
#define AUTH_METHOD_OUTPUT 0x02
#define AUTH_METHOD_INPUT 0x03

#define OUTPUT_OOB_BLINK 0x00
#define OUTPUT_OOB_BEEP 0x01
#define OUTPUT_OOB_VIBRATE 0x02
#define OUTPUT_OOB_NUMBER 0x03
#define OUTPUT_OOB_STRING 0x04

#define INPUT_OOB_PUSH 0x00
#define INPUT_OOB_TWIST 0x01
#define INPUT_OOB_NUMBER 0x02
#define INPUT_OOB_STRING 0x03

#define PROV_ERR_NONE 0x00
#define PROV_ERR_NVAL_PDU 0x01
#define PROV_ERR_NVAL_FMT 0x02
#define PROV_ERR_UNEXP_PDU 0x03
#define PROV_ERR_CFM_FAILED 0x04
#define PROV_ERR_RESOURCES 0x05
#define PROV_ERR_DECRYPT 0x06
#define PROV_ERR_UNEXP_ERR 0x07
#define PROV_ERR_ADDR 0x08

#define PROV_INVITE 0x00
#define PROV_CAPABILITIES 0x01
#define PROV_START 0x02
#define PROV_PUB_KEY 0x03
#define PROV_INPUT_COMPLETE 0x04
#define PROV_CONFIRM 0x05
#define PROV_RANDOM 0x06
#define PROV_DATA 0x07
#define PROV_COMPLETE 0x08
#define PROV_FAILED 0x09

#define PROV_ALG_P256 0x00

#define GPCF(gpc) (gpc & 0x03)
#define GPC_START(last_seg) (((last_seg) << 2) | 0x00)
#define GPC_ACK 0x01
#define GPC_CONT(seg_id) (((seg_id) << 2) | 0x02)
#define GPC_CTL(op) (((op) << 2) | 0x03)

#define START_PAYLOAD_MAX 20
#define CONT_PAYLOAD_MAX 23

#define START_LAST_SEG(gpc) (gpc >> 2)
#define CONT_SEG_INDEX(gpc) (gpc >> 2)

#define BEARER_CTL(gpc) (gpc >> 2)
#define LINK_OPEN 0x00
#define LINK_ACK 0x01
#define LINK_CLOSE 0x02

#define CLOSE_REASON_SUCCESS 0x00
#define CLOSE_REASON_TIMEOUT 0x01
#define CLOSE_REASON_FAILED 0x02

#define XACT_SEG_DATA(_seg) (&slink.rx.buf->data[20 + ((_seg - 1) * 23)])
#define XACT_SEG_RECV(_seg) (slink.rx.seg &= ~(1 << (_seg)))

#define XACT_NVAL 0xff

enum
{
    REMOTE_PUB_KEY, /* Remote key has been received */
    LOCAL_PUB_KEY,  /* Local public key is available */
    LINK_ACTIVE,    /* Link has been opened */
    HAVE_DHKEY,     /* DHKey has been calcualted */
    SEND_CONFIRM,   /* Waiting to send Confirm value */
    WAIT_NUMBER,    /* Waiting for number input from user */
    WAIT_STRING,    /* Waiting for string input from user */

    NUM_FLAGS,
};

struct prov_link
{
    ATOMIC_DEFINE(flags, NUM_FLAGS);
#if defined(CONFIG_BT_MESH_PB_GATT)
    bt_mesh_conn_t conn; /* GATT connection */
#endif
    u8_t dhkey[32]; /* Calculated DHKey */
    u8_t expect;    /* Next expected PDU */

    u8_t oob_method;
    u8_t oob_action;
    u8_t oob_size;

    u8_t conf[16]; /* Remote Confirmation */
    u8_t rand[16]; /* Local Random */
    u8_t auth[16]; /* Authentication Value */

    u8_t conf_salt[16];    /* ConfirmationSalt */
    u8_t conf_key[16];     /* ConfirmationKey */
    u8_t conf_inputs[145]; /* ConfirmationInputs */
    u8_t prov_salt[16];    /* Provisioning Salt */

#if defined(CONFIG_BT_MESH_PB_ADV)
    u32_t id; /* Link ID */

    struct
    {
        u8_t id;       /* Transaction ID */
        u8_t prev_id;  /* Previous Transaction ID */
        u8_t seg;      /* Bit-field of unreceived segments */
        u8_t last_seg; /* Last segment (to check length) */
        u8_t fcs;      /* Expected FCS value */
        struct net_buf_simple *buf;
    } rx;

    struct
    {
        /* Start timestamp of the transaction */
        s64_t start;

        /* Transaction id*/
        u8_t id;

        /* Pending outgoing buffer(s) */
        struct net_buf *buf[3];

        /* Retransmit timer */
        struct k_delayed_work retransmit;
    } tx;
#endif
};

struct prov_rx
{
    u32_t link_id;
    u8_t xact_id;
    u8_t gpc;
};

#define RETRANSMIT_TIMEOUT K_MSEC(500)
#define BUF_TIMEOUT K_MSEC(400)
#define TRANSACTION_TIMEOUT K_SECONDS(30)

#if defined(CONFIG_BT_MESH_PB_GATT)
#define PROV_BUF_HEADROOM 5
#else
#define PROV_BUF_HEADROOM 0
static struct net_buf_simple *rx_buf = NET_BUF_SIMPLE(65);
#endif

#define PROV_BUF(len) NET_BUF_SIMPLE(PROV_BUF_HEADROOM + len)

/*[Genie begin] change by lgy at 2020-09-10*/
/*rename link to slink fix build redeclared error*/
static struct prov_link slink;
/*[Genie end] change by lgy at 2020-09-10*/

static const struct bt_mesh_prov *prov;

static void close_link(u8_t err, u8_t reason);

#if defined(CONFIG_BT_MESH_PB_ADV)
static void buf_sent(int err, void *user_data)
{
    if (!slink.tx.buf[0])
    {
        return;
    }

    k_delayed_work_submit(&slink.tx.retransmit, RETRANSMIT_TIMEOUT);
}

static struct bt_mesh_send_cb buf_sent_cb = {
    .end = buf_sent,
};

static void free_segments(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(slink.tx.buf); i++)
    {
        struct net_buf *buf = slink.tx.buf[i];

        if (!buf)
        {
            break;
        }

        slink.tx.buf[i] = NULL;
        /* Mark as canceled */
        BT_MESH_ADV(buf)->busy = 0;
        net_buf_unref(buf);
    }
}

static void prov_clear_tx(void)
{
    BT_DBG("");

    k_delayed_work_cancel(&slink.tx.retransmit);

    free_segments();
}

static void reset_link(void)
{
    prov_clear_tx();

    if (prov->link_close)
    {
        prov->link_close(BT_MESH_PROV_ADV);
    }

    /* Clear everything except the retransmit delayed work config */
    memset(&slink, 0, offsetof(struct prov_link, tx.retransmit));

    slink.rx.prev_id = XACT_NVAL;

    if (bt_mesh_pub_key_get())
    {
        atomic_set_bit(slink.flags, LOCAL_PUB_KEY);
    }

#if defined(CONFIG_BT_MESH_PB_GATT)
    slink.rx.buf = bt_mesh_proxy_get_buf();
#else
    net_buf_simple_init(rx_buf, 0);
    slink.rx.buf = rx_buf;
#endif

    /* Disable Attention Timer if it was set */
    if (slink.conf_inputs[0])
    {
        bt_mesh_attention(NULL, 0);
    }
}

void bt_mesh_prov_reset_link(void)
{
    if (atomic_test_bit(slink.flags, LINK_ACTIVE))
    {
        reset_link();
    }
}

static struct net_buf *adv_buf_create(void)
{
    struct net_buf *buf;

    buf = bt_mesh_adv_create(BT_MESH_ADV_PROV, PROV_XMIT_COUNT,
                             PROV_XMIT_INT, BUF_TIMEOUT);
    if (!buf)
    {
        BT_ERR("Out of provisioning buffers");
        return NULL;
    }

    return buf;
}

static u8_t pending_ack = XACT_NVAL;

static void ack_complete(u16_t duration, int err, void *user_data)
{
    BT_DBG("xact %u complete", (u8_t)pending_ack);
    pending_ack = XACT_NVAL;
}

static void gen_prov_ack_send(u8_t xact_id)
{
    static const struct bt_mesh_send_cb cb = {
        .start = ack_complete,
    };
    const struct bt_mesh_send_cb *complete;
    struct net_buf *buf;

    BT_DBG("xact_id %u", xact_id);

    if (pending_ack == xact_id)
    {
        BT_DBG("Not sending duplicate ack");
        return;
    }

    buf = adv_buf_create();
    if (!buf)
    {
        return;
    }

    if (pending_ack == XACT_NVAL)
    {
        pending_ack = xact_id;
        complete = &cb;
    }
    else
    {
        complete = NULL;
    }

    net_buf_add_be32(buf, slink.id);
    net_buf_add_u8(buf, xact_id);
    net_buf_add_u8(buf, GPC_ACK);

    bt_mesh_adv_send(buf, complete, NULL);
    net_buf_unref(buf);
}

static void send_reliable(void)
{
    int i;

    slink.tx.start = k_uptime_get();

    for (i = 0; i < ARRAY_SIZE(slink.tx.buf); i++)
    {
        struct net_buf *buf = slink.tx.buf[i];

        if (!buf)
        {
            break;
        }

        if (i + 1 < ARRAY_SIZE(slink.tx.buf) && slink.tx.buf[i + 1])
        {
            bt_mesh_adv_send(buf, NULL, NULL);
        }
        else
        {
            bt_mesh_adv_send(buf, &buf_sent_cb, NULL);
        }
    }
}

static int bearer_ctl_send(u8_t op, void *data, u8_t data_len)
{
    struct net_buf *buf;

    BT_DBG("op 0x%02x data_len %u", op, data_len);

    prov_clear_tx();

    buf = adv_buf_create();
    if (!buf)
    {
        return -ENOBUFS;
    }

    net_buf_add_be32(buf, slink.id);
    /* Transaction ID, always 0 for Bearer messages */
    net_buf_add_u8(buf, 0x00);
    net_buf_add_u8(buf, GPC_CTL(op));
    net_buf_add_mem(buf, data, data_len);

    slink.tx.buf[0] = buf;
    send_reliable();

    return 0;
}

static u8_t last_seg(u8_t len)
{
    if (len <= START_PAYLOAD_MAX)
    {
        return 0;
    }

    len -= START_PAYLOAD_MAX;

    return 1 + (len / CONT_PAYLOAD_MAX);
}

static inline u8_t next_transaction_id(void)
{
    if (slink.tx.id != 0 && slink.tx.id != 0xFF)
    {
        return ++slink.tx.id;
    }

    slink.tx.id = 0x80;
    return slink.tx.id;
}

static int prov_send_adv(struct net_buf_simple *msg)
{
    struct net_buf *start, *buf;
    u8_t seg_len, seg_id;
    u8_t xact_id;

    BT_DBG("len %u: %s", msg->len, bt_hex(msg->data, msg->len));
    //PROV_D("len %u: %s", msg->len, bt_hex(msg->data, msg->len));
    PROV_D("len %u", msg->len);

    prov_clear_tx();

    start = adv_buf_create();
    if (!start)
    {
        return -ENOBUFS;
    }

    xact_id = next_transaction_id();
    net_buf_add_be32(start, slink.id);
    net_buf_add_u8(start, xact_id);

    net_buf_add_u8(start, GPC_START(last_seg(msg->len)));
    net_buf_add_be16(start, msg->len);
    net_buf_add_u8(start, bt_mesh_fcs_calc(msg->data, msg->len));

    slink.tx.buf[0] = start;

    seg_len = min(msg->len, START_PAYLOAD_MAX);
    BT_DBG("seg 0 len %u: %s", seg_len, bt_hex(msg->data, seg_len));
    net_buf_add_mem(start, msg->data, seg_len);
    net_buf_simple_pull(msg, seg_len);

    buf = start;
    for (seg_id = 1; msg->len > 0; seg_id++)
    {
        if (seg_id >= ARRAY_SIZE(slink.tx.buf))
        {
            BT_ERR("Too big message");
            free_segments();
            return -E2BIG;
        }

        buf = adv_buf_create();
        if (!buf)
        {
            free_segments();
            return -ENOBUFS;
        }

        slink.tx.buf[seg_id] = buf;

        seg_len = min(msg->len, CONT_PAYLOAD_MAX);

        BT_DBG("seg_id %u len %u: %s", seg_id, seg_len,
               bt_hex(msg->data, seg_len));

        net_buf_add_be32(buf, slink.id);
        net_buf_add_u8(buf, xact_id);
        net_buf_add_u8(buf, GPC_CONT(seg_id));
        net_buf_add_mem(buf, msg->data, seg_len);
        net_buf_simple_pull(msg, seg_len);
    }

    send_reliable();

    return 0;
}

#endif /* CONFIG_BT_MESH_PB_ADV */

#if defined(CONFIG_BT_MESH_PB_GATT)
static int prov_send_gatt(struct net_buf_simple *msg)
{
    if (!slink.conn)
    {
        return -ENOTCONN;
    }

    return bt_mesh_proxy_send(slink.conn, BT_MESH_PROXY_PROV, msg);
}
#endif /* CONFIG_BT_MESH_PB_GATT */

static inline int prov_send(struct net_buf_simple *buf)
{
#if defined(CONFIG_BT_MESH_PB_GATT)
    if (slink.conn)
    {
        return prov_send_gatt(buf);
    }
#endif
#if defined(CONFIG_BT_MESH_PB_ADV)
    return prov_send_adv(buf);
#else
    return 0;
#endif
}

static void prov_buf_init(struct net_buf_simple *buf, u8_t type)
{
    net_buf_simple_init(buf, PROV_BUF_HEADROOM);
    net_buf_simple_add_u8(buf, type);
}

static void prov_send_fail_msg(u8_t err)
{
    struct net_buf_simple *buf = PROV_BUF(2);

    prov_buf_init(buf, PROV_FAILED);
    net_buf_simple_add_u8(buf, err);
    prov_send(buf);
}

static void prov_invite(const u8_t *data)
{
    struct net_buf_simple *buf = PROV_BUF(12);

    BT_DBG("Attention Duration: %u seconds", data[0]);
    PROV_D(", 1--->2");

    if (data[0])
    {
        bt_mesh_attention(NULL, data[0]);
    }

    slink.conf_inputs[0] = data[0];

    prov_buf_init(buf, PROV_CAPABILITIES);

    /* Number of Elements supported */
    net_buf_simple_add_u8(buf, bt_mesh_elem_count());

    /* Supported algorithms - FIPS P-256 Eliptic Curve */
    net_buf_simple_add_be16(buf, MESH_BIT(PROV_ALG_P256));

    /* Public Key Type */
    net_buf_simple_add_u8(buf, prov->public_key_type);

    /* Static OOB Type */
    net_buf_simple_add_u8(buf, MESH_BIT(0));

    /* Output OOB Size */
    net_buf_simple_add_u8(buf, prov->output_size);

    /* Output OOB Action */
    net_buf_simple_add_be16(buf, prov->output_actions);

    /* Input OOB Size */
    net_buf_simple_add_u8(buf, prov->input_size);

    /* Input OOB Action */
    net_buf_simple_add_be16(buf, prov->input_actions);

    memcpy(&slink.conf_inputs[1], &buf->data[1], 11);

    if (prov_send(buf))
    {
        BT_ERR("Failed to send capabilities");
        close_link(PROV_ERR_RESOURCES, CLOSE_REASON_FAILED);
        return;
    }

    slink.expect = PROV_START;

#if defined(CONFIG_BT_MESH_PB_GATT)
    if (slink.conn)
    {
        genie_event(GENIE_EVT_SDK_MESH_PROV_START, NULL);
    }
#endif
}

static void prov_capabilities(const u8_t *data)
{
#if defined(CONFIG_BT_DEBUG_LOG)
    u16_t algorithms, output_action, input_action;

    BT_DBG("Elements: %u", data[0]);

    algorithms = sys_get_be16(&data[1]);
    BT_DBG("Algorithms:        %u", algorithms);

    BT_DBG("Public Key Type:   0x%02x", data[3]);
    BT_DBG("Static OOB Type:   0x%02x", data[4]);
    BT_DBG("Output OOB Size:   %u", data[5]);

    output_action = sys_get_be16(&data[6]);
    BT_DBG("Output OOB Action: 0x%04x", output_action);

    BT_DBG("Input OOB Size:    %u", data[8]);

    input_action = sys_get_be16(&data[9]);
    BT_DBG("Input OOB Action:  0x%04x", input_action);
#endif
}

#if 0
static bt_mesh_output_action_t output_action(u8_t action)
{
    switch (action) {
        case OUTPUT_OOB_BLINK:
            return BT_MESH_BLINK;
        case OUTPUT_OOB_BEEP:
            return BT_MESH_BEEP;
        case OUTPUT_OOB_VIBRATE:
            return BT_MESH_VIBRATE;
        case OUTPUT_OOB_NUMBER:
            return BT_MESH_DISPLAY_NUMBER;
        case OUTPUT_OOB_STRING:
            return BT_MESH_DISPLAY_STRING;
        default:
            return BT_MESH_NO_OUTPUT;
    }
}

static bt_mesh_input_action_t input_action(u8_t action)
{
    switch (action) {
        case INPUT_OOB_PUSH:
            return BT_MESH_PUSH;
        case INPUT_OOB_TWIST:
            return BT_MESH_TWIST;
        case INPUT_OOB_NUMBER:
            return BT_MESH_ENTER_NUMBER;
        case INPUT_OOB_STRING:
            return BT_MESH_ENTER_STRING;
        default:
            return BT_MESH_NO_INPUT;
    }
}
#endif

static int prov_auth(u8_t method, u8_t action, u8_t size)
{
#if 0
    bt_mesh_output_action_t output;
    bt_mesh_input_action_t input;
#endif

    switch (method)
    {
    case AUTH_METHOD_STATIC:
        if (action || size)
        {
            return -EINVAL;
        }
        memcpy(slink.auth + 16 - prov->static_val_len,
               prov->static_val, prov->static_val_len);
        memset(slink.auth, 0, sizeof(slink.auth) - prov->static_val_len);
        return 0;
#if 0 //refuse other method
        case AUTH_METHOD_NO_OOB:
            if (action || size) {
                return -EINVAL;
            }

            memset(slink.auth, 0, sizeof(slink.auth));
            return 0;

        case AUTH_METHOD_OUTPUT:
            output = output_action(action);
            if (!output) {
                return -EINVAL;
            }

            if (!(prov->output_actions & output)) {
                return -EINVAL;
            }

            if (size > prov->output_size) {
                return -EINVAL;
            }

            if (output == BT_MESH_DISPLAY_STRING) {
                unsigned char str[9];
                u8_t i;

                bt_mesh_rand(str, size);

                /* Normalize to '0' .. '9' & 'A' .. 'Z' */
                for (i = 0; i < size; i++) {
                    str[i] %= 36;
                    if (str[i] < 10) {
                        str[i] += '0';
                    } else {
                        str[i] += 'A' - 10;
                    }
                }
                str[size] = '\0';

                memcpy(slink.auth, str, size);
                memset(slink.auth + size, 0, sizeof(slink.auth) - size);

                return prov->output_string((char *)str);
            } else {
                u32_t div[8] = { 10, 100, 1000, 10000, 100000,
                                 1000000, 10000000, 100000000
                               };
                u32_t num;

                bt_mesh_rand(&num, sizeof(num));
                num %= div[size - 1];

                sys_put_be32(num, &slink.auth[12]);
                memset(slink.auth, 0, 12);

                return prov->output_number(output, num);
            }

        case AUTH_METHOD_INPUT:
            input = input_action(action);
            if (!input) {
                return -EINVAL;
            }

            if (!(prov->input_actions & input)) {
                return -EINVAL;
            }

            if (size > prov->input_size) {
                return -EINVAL;
            }

            if (input == BT_MESH_ENTER_STRING) {
                atomic_set_bit(slink.flags, WAIT_STRING);
            } else {
                atomic_set_bit(slink.flags, WAIT_NUMBER);
            }

            return prov->input(input, size);
#endif
    default:
        return -EINVAL;
    }
}

static void prov_start(const u8_t *data)
{
    BT_DBG("Algorithm:   0x%02x", data[0]);
    BT_DBG("Public Key:  0x%02x", data[1]);
    BT_DBG("Auth Method: 0x%02x", data[2]);
    BT_DBG("Auth Action: 0x%02x", data[3]);
    BT_DBG("Auth Size:   0x%02x", data[4]);
    PROV_D(", 2--->3");

    if (data[0] != PROV_ALG_P256)
    {
        BT_ERR("Unknown algorithm 0x%02x", data[0]);
        /* added for PTS NODE-PROV-BI-03-C*/
        prov_send_fail_msg(PROV_ERR_NVAL_FMT);
        slink.expect = PROV_FAILED;
        return;
    }

    if (data[1] != prov->public_key_type)
    {
        BT_ERR("Invalid public key value: 0x%02x", data[1]);
        prov_send_fail_msg(PROV_ERR_NVAL_FMT);
        /* added for PTS NODE-PROV-BI-03-C*/
        slink.expect = PROV_FAILED;
        return;
    }

    memcpy(&slink.conf_inputs[12], data, 5);

    slink.expect = PROV_PUB_KEY;

    if (prov_auth(data[2], data[3], data[4]) < 0)
    {
        BT_ERR("Invalid authentication method: 0x%02x; "
               "action: 0x%02x; size: 0x%02x",
               data[2], data[3],
               data[4]);
        prov_send_fail_msg(PROV_ERR_NVAL_FMT);
    }
}

static void send_confirm(void)
{
    struct net_buf_simple *cfm = PROV_BUF(17);

    BT_DBG("ConfInputs[0]   %s", bt_hex(slink.conf_inputs, 64));
    BT_DBG("ConfInputs[64]  %s", bt_hex(&slink.conf_inputs[64], 64));
    BT_DBG("ConfInputs[128] %s", bt_hex(&slink.conf_inputs[128], 17));

    if (bt_mesh_prov_conf_salt(slink.conf_inputs, slink.conf_salt))
    {
        BT_ERR("Unable to generate confirmation salt");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("ConfirmationSalt: %s", bt_hex(slink.conf_salt, 16));

    if (bt_mesh_prov_conf_key(slink.dhkey, slink.conf_salt, slink.conf_key))
    {
        BT_ERR("Unable to generate confirmation key");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("ConfirmationKey: %s", bt_hex(slink.conf_key, 16));

    if (bt_mesh_rand(slink.rand, 16))
    {
        BT_ERR("Unable to generate random number");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("LocalRandom: %s", bt_hex(slink.rand, 16));

    prov_buf_init(cfm, PROV_CONFIRM);

    memcpy(slink.auth, genie_crypto_get_auth(slink.rand), STATIC_OOB_LENGTH);

    if (bt_mesh_prov_conf(slink.conf_key, slink.rand, slink.auth,
                          net_buf_simple_add(cfm, 16)))
    {
        BT_ERR("Unable to generate confirmation value");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    if (prov_send(cfm))
    {
        BT_ERR("Failed to send Provisioning Confirm");
        close_link(PROV_ERR_RESOURCES, CLOSE_REASON_FAILED);
        return;
    }

    slink.expect = PROV_RANDOM;
}

static void send_input_complete(void)
{
    struct net_buf_simple *buf = PROV_BUF(1);

    prov_buf_init(buf, PROV_INPUT_COMPLETE);
    prov_send(buf);
}

int bt_mesh_input_number(u32_t num)
{
    BT_DBG("%u", num);

    if (!atomic_test_and_clear_bit(slink.flags, WAIT_NUMBER))
    {
        return -EINVAL;
    }

    sys_put_be32(num, &slink.auth[12]);

    send_input_complete();

    if (!atomic_test_bit(slink.flags, HAVE_DHKEY))
    {
        return 0;
    }

    if (atomic_test_and_clear_bit(slink.flags, SEND_CONFIRM))
    {
        send_confirm();
    }

    return 0;
}

int bt_mesh_input_string(const char *str)
{
    BT_DBG("%s", str);

    if (!atomic_test_and_clear_bit(slink.flags, WAIT_STRING))
    {
        return -EINVAL;
    }

    strncpy((char *)slink.auth, str, prov->input_size);

    send_input_complete();

    if (!atomic_test_bit(slink.flags, HAVE_DHKEY))
    {
        return 0;
    }

    if (atomic_test_and_clear_bit(slink.flags, SEND_CONFIRM))
    {
        send_confirm();
    }

    return 0;
}

static void prov_dh_key_cb(const u8_t key[32])
{
    BT_DBG("%p", key);

    if (!key)
    {
        BT_ERR("DHKey generation failed");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    sys_memcpy_swap(slink.dhkey, key, 32);

    BT_DBG("DHkey: %s", bt_hex(slink.dhkey, 32));

    atomic_set_bit(slink.flags, HAVE_DHKEY);

    if (atomic_test_bit(slink.flags, WAIT_NUMBER) ||
        atomic_test_bit(slink.flags, WAIT_STRING))
    {
        return;
    }

    if (atomic_test_and_clear_bit(slink.flags, SEND_CONFIRM))
    {
        send_confirm();
    }
}

static void send_pub_key(void)
{
    struct net_buf_simple *buf = PROV_BUF(65);
    const u8_t *key;

    key = bt_mesh_pub_key_get();
    if (!key)
    {
        BT_ERR("No public key available");
        close_link(PROV_ERR_RESOURCES, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("Local Public Key: %s", bt_hex(key, 64));

    prov_buf_init(buf, PROV_PUB_KEY);

    /* Swap X and Y halves independently to big-endian */
    sys_memcpy_swap(net_buf_simple_add(buf, 32), key, 32);
    sys_memcpy_swap(net_buf_simple_add(buf, 32), &key[32], 32);

    memcpy(&slink.conf_inputs[81], &buf->data[1], 64);

    prov_send(buf);

    /* Copy remote key in little-endian for bt_dh_key_gen().
     * X and Y halves are swapped independently.
     */
    net_buf_simple_init(buf, 0);
    sys_memcpy_swap(buf->data, &slink.conf_inputs[17], 32);
    sys_memcpy_swap(&buf->data[32], &slink.conf_inputs[49], 32);

    //if (bt_dh_key_gen(buf->data, prov_dh_key_cb)) {
    if (bt_mesh_dh_key_gen(buf->data, prov_dh_key_cb))
    {
        BT_ERR("Failed to generate DHKey");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    slink.expect = PROV_CONFIRM;
}

static void prov_pub_key(const u8_t *data)
{
    BT_DBG("Remote Public Key: %s", bt_hex(data, 64));
    PROV_D(", 3--->4");

    memcpy(&slink.conf_inputs[17], data, 64);

    if (!atomic_test_bit(slink.flags, LOCAL_PUB_KEY))
    {
        /* Clear retransmit timer */
#if defined(CONFIG_BT_MESH_PB_ADV)
        prov_clear_tx();
#endif
        atomic_set_bit(slink.flags, REMOTE_PUB_KEY);
        BT_WARN("Waiting for local public key");
        return;
    }

    send_pub_key();
}

static void pub_key_ready(const u8_t *pkey)
{
    if (!pkey)
    {
        BT_WARN("Public key not available");
        return;
    }

    BT_DBG("Local public key ready");

    atomic_set_bit(slink.flags, LOCAL_PUB_KEY);

    if (atomic_test_and_clear_bit(slink.flags, REMOTE_PUB_KEY))
    {
        send_pub_key();
    }
}

static void prov_input_complete(const u8_t *data)
{
    BT_DBG("");
}

static void prov_confirm(const u8_t *data)
{
    BT_DBG("Remote Confirm: %s", bt_hex(data, 16));
    PROV_D(", 4--->5");

    memcpy(slink.conf, data, 16);

    if (!atomic_test_bit(slink.flags, HAVE_DHKEY))
    {
#if defined(CONFIG_BT_MESH_PB_ADV)
        prov_clear_tx();
#endif
        atomic_set_bit(slink.flags, SEND_CONFIRM);
    }
    else
    {
        send_confirm();
    }
}

static void prov_random(const u8_t *data)
{
    struct net_buf_simple *rnd = PROV_BUF(16);
    u8_t conf_verify[16];

    BT_DBG("Remote Random: %s", bt_hex(data, 16));
    PROV_D(", 5--->6");

    memcpy(slink.auth, genie_crypto_get_auth(data), STATIC_OOB_LENGTH);

    if (bt_mesh_prov_conf(slink.conf_key, data, slink.auth, conf_verify))
    {
        BT_ERR("Unable to calculate confirmation verification");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    if (memcmp(conf_verify, slink.conf, 16))
    {
        BT_ERR("Invalid confirmation value");
        BT_DBG("Received:   %s", bt_hex(slink.conf, 16));
        BT_DBG("Calculated: %s", bt_hex(conf_verify, 16));
        /* modified for NODE/PROV/BV-10-C in PTS 7.4.1*/
        prov_send_fail_msg(PROV_ERR_CFM_FAILED);
        slink.expect = PROV_FAILED;
        //close_link(PROV_ERR_CFM_FAILED, CLOSE_REASON_FAILED);
        return;
    }

    prov_buf_init(rnd, PROV_RANDOM);
    net_buf_simple_add_mem(rnd, slink.rand, 16);

    if (prov_send(rnd))
    {
        BT_ERR("Failed to send Provisioning Random");
        close_link(PROV_ERR_RESOURCES, CLOSE_REASON_FAILED);
        return;
    }

    if (bt_mesh_prov_salt(slink.conf_salt, data, slink.rand,
                          slink.prov_salt))
    {
        BT_ERR("Failed to generate provisioning salt");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("ProvisioningSalt: %s", bt_hex(slink.prov_salt, 16));

    slink.expect = PROV_DATA;
}

static void prov_data(const u8_t *data)
{
    struct net_buf_simple *msg = PROV_BUF(1);
    u8_t session_key[16];
    u8_t nonce[13];
    u8_t dev_key[16];
    u8_t pdu[25];
    u8_t flags;
    u32_t iv_index;
    u16_t addr;
    u16_t net_idx;
    int err;

    BT_DBG("");
    PROV_D(", 6--->7");

    err = bt_mesh_session_key(slink.dhkey, slink.prov_salt, session_key);
    if (err)
    {
        BT_ERR("Unable to generate session key");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("SessionKey: %s", bt_hex(session_key, 16));

    err = bt_mesh_prov_nonce(slink.dhkey, slink.prov_salt, nonce);
    if (err)
    {
        BT_ERR("Unable to generate session nonce");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    BT_DBG("Nonce: %s", bt_hex(nonce, 13));

    err = bt_mesh_prov_decrypt(session_key, nonce, data, pdu);
    if (err)
    {
        BT_ERR("Unable to decrypt provisioning data");
        /* modified for NODE/PROV/BV-10-C in PTS 7.4.1*/
        prov_send_fail_msg(PROV_ERR_DECRYPT);
        slink.expect = PROV_FAILED;
        //close_link(PROV_ERR_DECRYPT, CLOSE_REASON_FAILED);
        return;
    }

    err = bt_mesh_dev_key(slink.dhkey, slink.prov_salt, dev_key);
    if (err)
    {
        BT_ERR("Unable to generate device key");
        close_link(PROV_ERR_UNEXP_ERR, CLOSE_REASON_FAILED);
        return;
    }

    net_idx = sys_get_be16(&pdu[16]);
    flags = pdu[18];
    iv_index = sys_get_be32(&pdu[19]);
    addr = sys_get_be16(&pdu[23]);

    BT_DBG("net_idx %u iv_index 0x%08x, addr 0x%04x",
           net_idx, iv_index, addr);

    genie_event(GENIE_EVT_SDK_MESH_PROV_DATA, &addr);

    prov_buf_init(msg, PROV_COMPLETE);
    prov_send(msg);

    /* Ignore any further PDUs on this slink */
    slink.expect = 0;

    bt_mesh_provision(pdu, net_idx, flags, iv_index, 0, addr, dev_key);
}

static void prov_complete(const u8_t *data)
{
    BT_DBG("");
    PROV_D(", 9--->10");
}

static void prov_failed(const u8_t *data)
{
    BT_WARN("Error: 0x%02x", data[0]);
}

static const struct
{
    void (*func)(const u8_t *data);
    u16_t len;
} prov_handlers[] = {
    {prov_invite, 1},
    {prov_capabilities, 11},
    {prov_start, 5},
    {prov_pub_key, 64},
    {prov_input_complete, 0},
    {prov_confirm, 16},
    {prov_random, 16},
    {prov_data, 33},
    {prov_complete, 0},
    {prov_failed, 1},
};

static void close_link(u8_t err, u8_t reason)
{
    PROV_D(", 10--->0");
#if defined(CONFIG_BT_MESH_PB_GATT)
    if (slink.conn)
    {
        bt_mesh_pb_gatt_close(slink.conn);
        return;
    }
#endif

#if defined(CONFIG_BT_MESH_PB_ADV)
    if (err)
    {
        prov_send_fail_msg(err);
    }

    slink.rx.seg = 0;
    bearer_ctl_send(LINK_CLOSE, &reason, sizeof(reason));
#endif

    atomic_clear_bit(slink.flags, LINK_ACTIVE);

    /* Disable Attention Timer if it was set */
    if (slink.conf_inputs[0])
    {
        bt_mesh_attention(NULL, 0);
    }
}

#if defined(CONFIG_BT_MESH_PB_ADV)
static void prov_retransmit(struct k_work *work)
{
    int i;

    BT_DBG("");

    if (!atomic_test_bit(slink.flags, LINK_ACTIVE))
    {
        BT_WARN("Link not active");
        return;
    }

    if (k_uptime_get() - slink.tx.start > TRANSACTION_TIMEOUT)
    {
        BT_WARN("Giving up transaction");
        reset_link();
        return;
    }

    for (i = 0; i < ARRAY_SIZE(slink.tx.buf); i++)
    {
        struct net_buf *buf = slink.tx.buf[i];

        if (!buf)
        {
            break;
        }

        if (BT_MESH_ADV(buf)->busy)
        {
            continue;
        }

        BT_DBG("%u bytes: %s", buf->len, bt_hex(buf->data, buf->len));

        if (i + 1 < ARRAY_SIZE(slink.tx.buf) && slink.tx.buf[i + 1])
        {
            bt_mesh_adv_send(buf, NULL, NULL);
        }
        else
        {
            bt_mesh_adv_send(buf, &buf_sent_cb, NULL);
        }
    }
}

static void link_open(struct prov_rx *rx, struct net_buf_simple *buf)
{
    BT_DBG("len %u", buf->len);

    if (buf->len < 16)
    {
        BT_ERR("Too short bearer open message (len %u)", buf->len);
        return;
    }

    if (atomic_test_bit(slink.flags, LINK_ACTIVE))
    {
        BT_WARN("Ignoring bearer open: slink already active");
        return;
    }

    if (memcmp(buf->data, prov->uuid, 16))
    {
        BT_DBG("Bearer open message not for us");
        return;
    }

    PROV_D("link_id: 0x%08x, 0--->1", rx->link_id);
    if (prov->link_open)
    {
        prov->link_open(BT_MESH_PROV_ADV);
    }

    slink.id = rx->link_id;
    atomic_set_bit(slink.flags, LINK_ACTIVE);
    net_buf_simple_init(slink.rx.buf, 0);

    bearer_ctl_send(LINK_ACK, NULL, 0);

    genie_event(GENIE_EVT_SDK_MESH_PROV_START, &rx->link_id);

    slink.expect = PROV_INVITE;
}

static void link_ack(struct prov_rx *rx, struct net_buf_simple *buf)
{
    BT_DBG("len %u", buf->len);
}

static void link_close(struct prov_rx *rx, struct net_buf_simple *buf)
{
    BT_DBG("len %u", buf->len);
    reset_link();
}

static void gen_prov_ctl(struct prov_rx *rx, struct net_buf_simple *buf)
{
    BT_DBG("op 0x%02x len %u", BEARER_CTL(rx->gpc), buf->len);

    switch (BEARER_CTL(rx->gpc))
    {
    case LINK_OPEN:
        link_open(rx, buf);
        break;
    case LINK_ACK:
        if (!atomic_test_bit(slink.flags, LINK_ACTIVE))
        {
            return;
        }

        link_ack(rx, buf);
        break;
    case LINK_CLOSE:
        if (!atomic_test_bit(slink.flags, LINK_ACTIVE))
        {
            return;
        }

        link_close(rx, buf);
        break;
    default:
        BT_ERR("Unknown bearer opcode: 0x%02x", BEARER_CTL(rx->gpc));
        return;
    }
}

static void prov_msg_recv(void)
{
    u8_t type = slink.rx.buf->data[0];

    BT_DBG("type 0x%02x len %u", type, slink.rx.buf->len);
    BT_DBG("data %s", bt_hex(slink.rx.buf->data, slink.rx.buf->len));

    if (!bt_mesh_fcs_check(slink.rx.buf, slink.rx.fcs))
    {
        BT_ERR("Incorrect FCS");
        return;
    }

    gen_prov_ack_send(slink.rx.id);
    slink.rx.prev_id = slink.rx.id;
    slink.rx.id = 0;

    if (type != PROV_FAILED && type != slink.expect)
    {
        BT_WARN("Unexpected msg 0x%02x != 0x%02x", type, slink.expect);
        prov_send_fail_msg(PROV_ERR_UNEXP_PDU);
        /* added for NODE/PROV/BV-10-C in PTS 7.4.1*/
        slink.expect = PROV_FAILED;
        return;
    }

    if (type >= ARRAY_SIZE(prov_handlers))
    {
        BT_ERR("Unknown provisioning PDU type 0x%02x", type);
        close_link(PROV_ERR_NVAL_PDU, CLOSE_REASON_FAILED);
        return;
    }

    if (1 + prov_handlers[type].len != slink.rx.buf->len)
    {
        BT_ERR("Invalid length %u for type 0x%02x",
               slink.rx.buf->len, type);
        close_link(PROV_ERR_NVAL_FMT, CLOSE_REASON_FAILED);
        return;
    }

    prov_handlers[type].func(&slink.rx.buf->data[1]);
}

static void gen_prov_cont(struct prov_rx *rx, struct net_buf_simple *buf)
{
    u8_t seg = CONT_SEG_INDEX(rx->gpc);

    BT_DBG("len %u, seg_index %u", buf->len, seg);

    if (!slink.rx.seg && slink.rx.prev_id == rx->xact_id)
    {
        BT_WARN("Resending ack");
        gen_prov_ack_send(rx->xact_id);
        return;
    }

    if (rx->xact_id != slink.rx.id)
    {
        BT_WARN("Data for unknown transaction (%u != %u)",
                rx->xact_id, slink.rx.id);
        return;
    }

    if (seg > slink.rx.last_seg)
    {
        BT_ERR("Invalid segment index %u", seg);
        close_link(PROV_ERR_NVAL_FMT, CLOSE_REASON_FAILED);
        return;
    }
    else if (seg == slink.rx.last_seg)
    {
        u8_t expect_len;

        expect_len = (slink.rx.buf->len - 20 -
                      (23 * (slink.rx.last_seg - 1)));
        if (expect_len != buf->len)
        {
            BT_ERR("Incorrect last seg len: %u != %u",
                   expect_len, buf->len);
            close_link(PROV_ERR_NVAL_FMT, CLOSE_REASON_FAILED);
            return;
        }
    }

    if (!(slink.rx.seg & MESH_BIT(seg)))
    {
        BT_WARN("Ignoring already received segment");
        return;
    }

    memcpy(XACT_SEG_DATA(seg), buf->data, buf->len);
    XACT_SEG_RECV(seg);

    if (!slink.rx.seg)
    {
        prov_msg_recv();
    }
}

static void gen_prov_ack(struct prov_rx *rx, struct net_buf_simple *buf)
{
    BT_DBG("len %u", buf->len);

    if (!slink.tx.buf[0])
    {
        return;
    }

    if (rx->xact_id == slink.tx.id)
    {
        prov_clear_tx();
    }
}

static void gen_prov_start(struct prov_rx *rx, struct net_buf_simple *buf)
{
    if (slink.rx.seg)
    {
        BT_WARN("Got Start while there are unreceived segments");
        return;
    }

    if (slink.rx.prev_id == rx->xact_id)
    {
        BT_WARN("Resending ack");
        gen_prov_ack_send(rx->xact_id);
        return;
    }

    slink.rx.buf->len = net_buf_simple_pull_be16(buf);
    slink.rx.id = rx->xact_id;
    slink.rx.fcs = net_buf_simple_pull_u8(buf);

    BT_DBG("len %u last_seg %u total_len %u fcs 0x%02x", buf->len,
           START_LAST_SEG(rx->gpc), slink.rx.buf->len, slink.rx.fcs);

    if (slink.rx.buf->len < 1)
    {
        BT_ERR("Ignoring zero-length provisioning PDU");
        close_link(PROV_ERR_NVAL_FMT, CLOSE_REASON_FAILED);
        return;
    }

    if (slink.rx.buf->len > slink.rx.buf->size)
    {
        BT_ERR("Too large provisioning PDU (%u bytes)",
               slink.rx.buf->len);
        close_link(PROV_ERR_NVAL_FMT, CLOSE_REASON_FAILED);
        return;
    }

    if (START_LAST_SEG(rx->gpc) > 0 && slink.rx.buf->len <= 20)
    {
        BT_ERR("Too small total length for multi-segment PDU");
        close_link(PROV_ERR_NVAL_FMT, CLOSE_REASON_FAILED);
        return;
    }

    slink.rx.seg = (1 << (START_LAST_SEG(rx->gpc) + 1)) - 1;
    slink.rx.last_seg = START_LAST_SEG(rx->gpc);
    memcpy(slink.rx.buf->data, buf->data, buf->len);
    XACT_SEG_RECV(0);

    if (!slink.rx.seg)
    {
        prov_msg_recv();
    }
}

static const struct
{
    void (*const func)(struct prov_rx *rx, struct net_buf_simple *buf);
    const u8_t require_link;
    const u8_t min_len;
} gen_prov[] = {
    {gen_prov_start, true, 3},
    {gen_prov_ack, true, 0},
    {gen_prov_cont, true, 0},
    {gen_prov_ctl, false, 0},
};

static void gen_prov_recv(struct prov_rx *rx, struct net_buf_simple *buf)
{
    if (buf->len < gen_prov[GPCF(rx->gpc)].min_len)
    {
        BT_ERR("Too short GPC message type %u", GPCF(rx->gpc));
        return;
    }

    if (!atomic_test_bit(slink.flags, LINK_ACTIVE) &&
        gen_prov[GPCF(rx->gpc)].require_link)
    {
        BT_DBG("Ignoring message that requires active slink");
        return;
    }

    gen_prov[GPCF(rx->gpc)].func(rx, buf);
}

void bt_mesh_pb_adv_recv(struct net_buf_simple *buf)
{
    struct prov_rx rx;

    if (!bt_prov_active() && bt_mesh_is_provisioned())
    {
        BT_DBG("Ignoring provisioning PDU - already provisioned");
        return;
    }

    if (buf->len < 6)
    {
        BT_WARN("Too short provisioning packet (len %u)", buf->len);
        return;
    }

    rx.link_id = net_buf_simple_pull_be32(buf);
    rx.xact_id = net_buf_simple_pull_u8(buf);
    rx.gpc = net_buf_simple_pull_u8(buf);

    BT_DBG("link_id 0x%08x xact_id %u", rx.link_id, rx.xact_id);

    if (atomic_test_bit(slink.flags, LINK_ACTIVE) && slink.id != rx.link_id)
    {
        BT_DBG("Ignoring mesh beacon for unknown slink");
        return;
    }

    gen_prov_recv(&rx, buf);
}
#endif /* CONFIG_BT_MESH_PB_ADV */

#if defined(CONFIG_BT_MESH_PB_GATT)
int bt_mesh_pb_gatt_recv(bt_mesh_conn_t conn, struct net_buf_simple *buf)
{
    u8_t type;

    BT_DBG("%s, %u bytes: %s", __func__, buf->len, bt_hex(buf->data, buf->len));

    if (slink.conn != conn)
    {
        BT_WARN("Data for unexpected connection");
        return -ENOTCONN;
    }

    if (buf->len < 1)
    {
        BT_WARN("Too short provisioning packet (len %u)", buf->len);
        return -EINVAL;
    }

    type = net_buf_simple_pull_u8(buf);
    if (type != PROV_FAILED && type != slink.expect)
    {
        BT_WARN("Unexpected msg 0x%02x != 0x%02x", type, slink.expect);
        return -EINVAL;
    }

    if (type >= ARRAY_SIZE(prov_handlers))
    {
        BT_ERR("Unknown provisioning PDU type 0x%02x", type);
        return -EINVAL;
    }

    if (prov_handlers[type].len != buf->len)
    {
        BT_ERR("Invalid length %u for type 0x%02x", buf->len, type);
        return -EINVAL;
    }

    prov_handlers[type].func(buf->data);

    return 0;
}

int bt_mesh_pb_gatt_open(bt_mesh_conn_t conn)
{
    BT_DBG("conn %p", conn);

    if (atomic_test_and_set_bit(slink.flags, LINK_ACTIVE))
    {
        return -EBUSY;
    }

    slink.conn = bt_mesh_conn_ref(conn);
    slink.expect = PROV_INVITE;

    if (prov->link_open)
    {
        prov->link_open(BT_MESH_PROV_GATT);
    }

    return 0;
}

int bt_mesh_pb_gatt_close(bt_mesh_conn_t conn)
{
    bool pub_key;

    BT_DBG("conn %p", conn);

    if (slink.conn != conn)
    {
        BT_ERR("Not connected");
        return -ENOTCONN;
    }

    /* Disable Attention Timer if it was set */
    if (slink.conf_inputs[0])
    {
        bt_mesh_attention(NULL, 0);
    }

    if (prov->link_close)
    {
        prov->link_close(BT_MESH_PROV_GATT);
    }

    bt_mesh_conn_unref(slink.conn);

    pub_key = atomic_test_bit(slink.flags, LOCAL_PUB_KEY);
    memset(&slink, 0, sizeof(slink));

    if (pub_key)
    {
        atomic_set_bit(slink.flags, LOCAL_PUB_KEY);
    }

    return 0;
}
#endif /* CONFIG_BT_MESH_PB_GATT */

const u8_t *bt_mesh_prov_get_uuid(void)
{
    return prov->uuid;
}

const struct bt_mesh_prov *bt_mesh_prov_get(void)
{
    return prov;
}

bool bt_prov_active(void)
{
    return atomic_test_bit(slink.flags, LINK_ACTIVE);
}

int bt_mesh_prov_init(const struct bt_mesh_prov *prov_info)
{
    static struct bt_mesh_pub_key_cb pub_key_cb = {
        .func = pub_key_ready,
    };
    int err;

    if (!prov_info)
    {
        BT_ERR("No provisioning context provided");
        return -EINVAL;
    }

    err = bt_mesh_pub_key_gen(&pub_key_cb);
    if (err)
    {
        BT_ERR("Failed to generate public key (%d)", err);
        return err;
    }

    prov = prov_info;

#if defined(CONFIG_BT_MESH_PB_ADV)
    k_delayed_work_init(&slink.tx.retransmit, prov_retransmit);
    slink.rx.prev_id = XACT_NVAL;

#if defined(CONFIG_BT_MESH_PB_GATT)
    slink.rx.buf = bt_mesh_proxy_get_buf();
#else
    net_buf_simple_init(rx_buf, 0);
    slink.rx.buf = rx_buf;
#endif

#endif /* CONFIG_BT_MESH_PB_ADV */

#if 0
    if (IS_ENABLED(CONFIG_BT_DEBUG)) {
        struct bt_mesh_uuid_128 uuid = { .uuid.type = BT_MESH_UUID_TYPE_128 };
        memcpy(uuid.val, prov->uuid, 16);
        BT_INFO("Device UUID: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                uuid.val[0], uuid.val[1], uuid.val[2], uuid.val[3],
                uuid.val[4], uuid.val[5], uuid.val[6], uuid.val[7],
                uuid.val[8], uuid.val[9], uuid.val[10], uuid.val[11],
                uuid.val[12], uuid.val[13], uuid.val[14], uuid.val[15],
               );
    }
#endif

    return 0;
}

void bt_mesh_prov_complete(u16_t net_idx, u16_t addr)
{
    if (prov->complete)
    {
        prov->complete(net_idx, addr);
    }
}

void bt_mesh_prov_reset(void)
{
    if (prov->reset)
    {
        prov->reset();
    }
}
