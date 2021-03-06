/** @file
 *  @brief Bluetooth Mesh shell
 *
 */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef CONFIG_BT_MESH_SHELL

#include <stdlib.h>
#include <ctype.h>
#include <zephyr.h>
#include <misc/printk.h>
#include "errno.h"

#include <bluetooth.h>
#include <api/mesh.h>

/* Private includes for raw Network & Transport layer access */
#include "mesh.h"
#include "net.h"
#include "transport.h"
#include "foundation.h"
#include "ais_ota.h"
#include "common/log.h"

#define CID_NVAL 0xffff

#define CUR_FAULTS_MAX 4

#define NET_PRESSURE_TEST_STRING "helloworld"

/* Default net, app & dev key values, unless otherwise specified */
static const u8_t default_key[16] = {
	0x01,
	0x23,
	0x45,
	0x67,
	0x89,
	0xab,
	0xcd,
	0xef,
	0x01,
	0x23,
	0x45,
	0x67,
	0x89,
	0xab,
	0xcd,
	0xef,
};

static struct
{
	u16_t local;
	u16_t dst;
	u16_t net_idx;
	u16_t app_idx;
} net = {
	.local = BT_MESH_ADDR_UNASSIGNED,
	.dst = BT_MESH_ADDR_UNASSIGNED,
};

static u8_t cur_faults[CUR_FAULTS_MAX];
static u8_t reg_faults[CUR_FAULTS_MAX * 2];

extern struct k_sem prov_input_sem;
extern u8_t prov_input[8];
extern u8_t prov_input_size;

struct mesh_shell_cmd *bt_mesh_get_shell_cmd_list();

#ifdef CONFIG_BT_MESH_PROV
static bt_mesh_prov_bearer_t prov_bear;
static void cmd_pb2(bt_mesh_prov_bearer_t bearer, const char *s);
#endif

#ifdef CONFIG_BT_MESH_CFG_CLI
static struct bt_mesh_cfg_cli cfg_cli = {};
#endif

#ifdef CONFIG_BT_MESH_HEALTH_CLI
void show_faults(u8_t test_id, u16_t cid, u8_t *faults, size_t fault_count)
{
	size_t i;

	if (!fault_count)
	{
		printk("Health Test ID 0x%02x Company ID 0x%04x: no faults\n",
			   test_id, cid);
		return;
	}

	printk("Health Test ID 0x%02x Company ID 0x%04x Fault Count %zu:\n",
		   test_id, cid, fault_count);

	for (i = 0; i < fault_count; i++)
	{
		printk("\t0x%02x\n", faults[i]);
	}
}

static void health_current_status(struct bt_mesh_health_cli *cli, u16_t addr,
								  u8_t test_id, u16_t cid, u8_t *faults,
								  size_t fault_count)
{
	printk("Health Current Status from 0x%04x\n", addr);
	show_faults(test_id, cid, faults, fault_count);
}

static struct bt_mesh_health_cli health_cli = {
	.current_status = health_current_status,
};
#endif

static u8_t dev_uuid[16] = {0xdd, 0xdd};

static struct bt_mesh_model root_models[] = {
#ifdef CONFIG_BT_MESH_CFG_SRV
	BT_MESH_MODEL_CFG_SRV(),
#endif
#ifdef CONFIG_BT_MESH_CFG_CLI
	BT_MESH_MODEL_CFG_CLI(&cfg_cli),
#endif
#ifdef CONFIG_BT_MESH_HEALTH_SRV
	BT_MESH_MODEL_HEALTH_SRV(),
#endif
#ifdef CONFIG_BT_MESH_HEALTH_CLI
	BT_MESH_MODEL_HEALTH_CLI(&health_cli),
#endif
#ifdef CONFIG_BT_MESH_GEN_ONOFF_CLI
	MESH_MODEL_GEN_ONOFF_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_BATTERY_CLI
	MESH_MODEL_GEN_BATTERY_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_LEVEL_CLI
	MESH_MODEL_GEN_LEVEL_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_DEF_TRANS_TIME_CLI
	MESH_MODEL_GEN_DEF_TRAN_TIME_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_POWER_ONOFF_CLI
	MESH_MODEL_GEN_POWER_ONOFF_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_POWER_LEVEL_CLI
	MESH_MODEL_GEN_POWER_LEVEL_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_LOCATION_CLI
	MESH_MODEL_GEN_LOCATION_CLI(),
#endif
#ifdef CONFIG_BT_MESH_GEN_PROP_CLI
	MESH_MODEL_GEN_PROP_CLI(),
#endif
#ifdef CONFIG_BT_MESH_SENSOR_CLI
//    MESH_MODEL_SENSOR_CLI(),
#endif
#ifdef CONFIG_BT_MESH_TIME_CLI
//	MESH_MODEL_TIME_CLI(),
#endif
#ifdef CONFIG_BT_MESH_SCENE_CLI
	MESH_MODEL_SCENE_CLI(),
#endif
#ifdef CONFIG_BT_MESH_SCHEDULER_CLI
	MESH_MODEL_SCHEDULER_CLI(),
#endif
#ifdef CONFIG_BT_MESH_LIGHT_LIGHTNESS_CLI
	MESH_MODEL_LIGHT_LIGHTNESS_CLI(),
#endif
#ifdef CONFIG_BT_MESH_LIGHT_CTL_CLI
	MESH_MODEL_LIGHT_CTL_CLI(),
#endif
#ifdef CONFIG_BT_MESH_LIGHT_HSL_CLI
	MESH_MODEL_LIGHT_HSL_CLI(),
#endif
#ifdef CONFIG_BT_MESH_LIGHT_XYL_CLI
//        MESH_MODEL_LIGHT_XYL_CLI(),
#endif
#ifdef CONFIG_BT_MESH_LIGHT_LC_CLI
	MESH_MODEL_LIGHT_LC_CLI(),
#endif
};
#if 0
static void vendor_model_get(struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
	return;
}


static const struct bt_mesh_model_op vendor_model_op[] = {
	{ BT_MESH_MODEL_OP_3(0xD0, CONFIG_MESH_VENDOR_COMPANY_ID), 0, vendor_model_get },
};

static struct bt_mesh_model_pub vendor_model_pub = {
    .msg = NET_BUF_SIMPLE(2 + 2),
};
static struct bt_mesh_model vnd_models[] = {
	BT_MESH_MODEL_VND(CONFIG_MESH_VENDOR_COMPANY_ID, 0x0000, vendor_model_op, &vendor_model_pub, NULL)
};
#endif

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE, 0),
};

static const struct bt_mesh_comp comp = {
	.cid = CONFIG_MESH_VENDOR_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

static u8_t hex2val(char c)
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	else if (c >= 'a' && c <= 'f')
	{
		return c - 'a' + 10;
	}
	else if (c >= 'A' && c <= 'F')
	{
		return c - 'A' + 10;
	}
	else
	{
		return 0;
	}
}

static size_t hex2bin(const char *hex, u8_t *bin, size_t bin_len)
{
	size_t len = 0;

	while (*hex && len < bin_len)
	{
		bin[len] = hex2val(*hex++) << 4;

		if (!*hex)
		{
			len++;
			break;
		}

		bin[len++] |= hex2val(*hex++);
	}

	return len;
}

static void prov_complete(u16_t net_idx, u16_t addr)
{
	printk("Local node provisioned, net_idx 0x%04x address 0x%04x\n",
		   net_idx, addr);
	net.net_idx = net_idx,
	net.local = addr;
	net.dst = addr;
}

void genie_prov_complete_notify(u16_t net_idx, u16_t addr)
{
	printk("genie node provisioned notify, net_idx 0x%04x address 0x%04x\n",
		   net_idx, addr);
	net.net_idx = net_idx,
	net.local = addr;
	net.dst = addr;
}

static void prov_reset(void)
{
	printk("The local node has been reset and needs reprovisioning\n");
}

static int output_number(bt_mesh_output_action_t action, u32_t number)
{
	printk("OOB Number: %u\n", number);
	return 0;
}

static int output_string(const char *str)
{
	printk("OOB String: %s\n", str);
	return 0;
}

static bt_mesh_input_action_t input_act;
static u8_t input_size;

static int cmd_input_num(int argc, char *argv[])
{
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	if (input_act != BT_MESH_ENTER_NUMBER)
	{
		printk("A number hasn't been requested!\n");
		return 0;
	}

	if (strlen(argv[1]) < input_size)
	{
		printk("Too short input (%u digits required)\n",
			   input_size);
		return 0;
	}

	err = bt_mesh_input_number(strtoul(argv[1], NULL, 10));
	if (err)
	{
		printk("Numeric input failed (err %d)\n", err);
		return 0;
	}

	input_act = BT_MESH_NO_INPUT;
	return 0;
}

static int cmd_input_str(int argc, char *argv[])
{
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	if (input_act != BT_MESH_ENTER_STRING)
	{
		printk("A string hasn't been requested!\n");
		return 0;
	}

	if (strlen(argv[1]) < input_size)
	{
		printk("Too short input (%u characters required)\n",
			   input_size);
		return 0;
	}

	err = bt_mesh_input_string(argv[1]);
	if (err)
	{
		printk("String input failed (err %d)\n", err);
		return 0;
	}

	input_act = BT_MESH_NO_INPUT;
	return 0;
}

static int input(bt_mesh_input_action_t act, u8_t size)
{
	switch (act)
	{
	case BT_MESH_ENTER_NUMBER:
		printk("Enter a number (max %u digits) with: input-num <num>\n",
			   size);
		break;
	case BT_MESH_ENTER_STRING:
		printk("Enter a string (max %u chars) with: input-str <str>\n",
			   size);
		break;
	default:
		printk("Unknown input action %u (size %u) requested!\n",
			   act, size);
		return -EINVAL;
	}

	input_act = act;
	input_size = size;
	return 0;
}

static const char *bearer2str(bt_mesh_prov_bearer_t bearer)
{
	switch (bearer)
	{
	case BT_MESH_PROV_ADV:
		return "PB-ADV";
	case BT_MESH_PROV_GATT:
		return "PB-GATT";
	case BT_MESH_PROV_ADV | BT_MESH_PROV_GATT:
		return "PB-ADV and PB-GATT";
	default:
		return "unknown";
	}
}

static void link_open(bt_mesh_prov_bearer_t bearer)
{
	printk("Provisioning link opened on %s\n", bearer2str(bearer));
}

static void link_close(bt_mesh_prov_bearer_t bearer)
{
	printk("Provisioning link closed on %s\n", bearer2str(bearer));
}

static u8_t static_val[16];

static struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
	.link_open = link_open,
	.link_close = link_close,
	.complete = prov_complete,
	.reset = prov_reset,
	//.static_val = NULL,
	//.static_val_len = 0,
	.output_size = 6,
	.output_actions = (BT_MESH_DISPLAY_NUMBER | BT_MESH_DISPLAY_STRING),
	.output_number = output_number,
	.output_string = output_string,
	.input_size = 6,
	.input_actions = (BT_MESH_ENTER_NUMBER | BT_MESH_ENTER_STRING),
	.input = input,
};

static int cmd_static_oob(int argc, char *argv[])
{
	if (argc < 2)
	{
		prov.static_val = NULL;
		prov.static_val_len = 0;
	}
	else
	{
		prov.static_val_len = hex2bin(argv[1], static_val, 16);
		if (prov.static_val_len)
		{
			prov.static_val = static_val;
		}
		else
		{
			prov.static_val = NULL;
		}
	}

	if (prov.static_val)
	{
		printk("Static OOB value set (length %u)\n",
			   prov.static_val_len);
	}
	else
	{
		printk("Static OOB value cleared\n");
	}

	return 0;
}

static int cmd_uuid(int argc, char *argv[])
{
	u8_t uuid[16];
	size_t len;

	if (argc < 2)
	{
		return -EINVAL;
	}

	len = hex2bin(argv[1], uuid, sizeof(uuid));
	if (len < 1)
	{
		return -EINVAL;
	}

	memcpy(dev_uuid, uuid, len);
	memset(dev_uuid + len, 0, sizeof(dev_uuid) - len);

	printk("Device UUID set\n");

	return 0;
}

static int cmd_reset(int argc, char *argv[])
{
	bt_mesh_reset();
	printk("Local node reset complete\n");
	return 0;
}

static u8_t str2u8(const char *str)
{
	if (isdigit(str[0]))
	{
		return strtoul(str, NULL, 0);
	}

	return (!strcmp(str, "on") || !strcmp(str, "enable"));
}

static bool str2bool(const char *str)
{
	return str2u8(str);
}

#if defined(CONFIG_BT_MESH_LOW_POWER)
static int cmd_lpn(int argc, char *argv[])
{
	static bool enabled;
	int err;

	if (argc < 2)
	{
		printk("%s\n", enabled ? "enabled" : "disabled");
		return 0;
	}

	if (str2bool(argv[1]))
	{
		if (enabled)
		{
			printk("LPN already enabled\n");
			return 0;
		}

		err = bt_mesh_lpn_set(true);
		if (err)
		{
			printk("Enabling LPN failed (err %d)\n", err);
		}
		else
		{
			enabled = true;
		}
	}
	else
	{
		if (!enabled)
		{
			printk("LPN already disabled\n");
			return 0;
		}

		err = bt_mesh_lpn_set(false);
		if (err)
		{
			printk("Enabling LPN failed (err %d)\n", err);
		}
		else
		{
			enabled = false;
		}
	}

	return 0;
}

static int cmd_poll(int argc, char *argv[])
{
	int err;

	err = bt_mesh_lpn_poll();
	if (err)
	{
		printk("Friend Poll failed (err %d)\n", err);
	}

	return 0;
}
static int cmd_lpn_add_sub(int argc, char *argv[])
{
	int err;

	err = bt_mesh_lpn_group_add(BT_MESH_ADDR_GENIE_ALL_NODES);
	if (err)
	{
		printk("add LPN sub failed (err %d)\n", err);
	}

	return 0;
}
u16_t shell_sub_group[2] = {
	0xC000,
	BT_MESH_ADDR_GENIE_ALL_NODES};
static int cmd_lpn_del_sub(int argc, char *argv[])
{
	int err;

	err = bt_mesh_lpn_group_del(shell_sub_group, 2);
	if (err)
	{
		printk("delete LPN sub failed (err %d)\n", err);
	}

	return 0;
}

static void lpn_cb(u16_t friend_addr, bool established)
{
	if (established)
	{
		printk("Friendship (as LPN) established to Friend 0x%04x, at %d\n",
			   friend_addr, k_uptime_get_32());
	}
	else
	{
		printk("Friendship (as LPN) lost with Friend 0x%04x, at %d\n",
			   friend_addr, k_uptime_get_32());
	}
}

#endif /* MESH_LOW_POWER */

static void bt_ready(int err)
{
	int ret;

	if (err)
	{
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	ret = bt_mesh_init(&prov, &comp);

	if (ret)
	{
		printk("Mesh initialization failed (err %d)\n", ret);
	}

	printk("Mesh initialized\n");
	printk("Use \"pb-adv on\" or \"pb-gatt on\" to enable advertising\n");

#if IS_ENABLED(CONFIG_BT_MESH_LOW_POWER)
	bt_mesh_lpn_set_cb(lpn_cb);
#endif

#ifdef CONFIG_BT_MESH_PROV
	cmd_pb2(prov_bear, "on");
#endif
}

extern int hci_driver_init(void);

static int cmd_init(int argc, char *argv[])
{
	int err = 0;
	hci_driver_init();
	ais_ota_bt_storage_init();

#ifndef CONFIG_MESH_STACK_ALONE
	err = bt_enable(bt_ready);
#endif
	return err;
}

#if defined(CONFIG_BT_MESH_GATT_PROXY)
static int cmd_ident(int argc, char *argv[])
{
	int err;

	err = bt_mesh_proxy_identity_enable();
	if (err)
	{
		printk("Failed advertise using Node Identity (err %d)\n", err);
	}

	return 0;
}
#endif /* MESH_GATT_PROXY */

static int cmd_dst(int argc, char *argv[])
{
	if (argc < 2)
	{
		printk("Destination address: 0x%04x%s\n", net.dst,
			   net.dst == net.local ? " (local)" : "");
		return 0;
	}

	if (!strcmp(argv[1], "local"))
	{
		net.dst = net.local;
	}
	else
	{
		net.dst = strtoul(argv[1], NULL, 0);
	}

	printk("Destination address set to 0x%04x%s\n", net.dst,
		   net.dst == net.local ? " (local)" : "");
	return 0;
}

static int cmd_netidx(int argc, char *argv[])
{
	if (argc < 2)
	{
		printk("NetIdx: 0x%04x\n", net.net_idx);
		return 0;
	}

	net.net_idx = strtoul(argv[1], NULL, 0);
	printk("NetIdx set to 0x%04x\n", net.net_idx);
	return 0;
}

static int cmd_appidx(int argc, char *argv[])
{
	if (argc < 2)
	{
		printk("AppIdx: 0x%04x\n", net.app_idx);
		return 0;
	}

	net.app_idx = strtoul(argv[1], NULL, 0);
	printk("AppIdx set to 0x%04x\n", net.app_idx);
	return 0;
}

static int cmd_net_send(int argc, char *argv[])
{
	struct net_buf_simple *msg = NET_BUF_SIMPLE(32);
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = net.net_idx,
		.addr = net.dst,
		.app_idx = net.app_idx,

	};
	struct bt_mesh_net_tx tx = {
		.ctx = &ctx,
		.src = net.local,
		.xmit = bt_mesh_net_transmit_get(),
		.sub = bt_mesh_subnet_get(net.net_idx),
	};
	size_t len;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	if (!tx.sub)
	{
		printk("No matching subnet for NetKey Index 0x%04x\n",
			   net.net_idx);
		return 0;
	}

	net_buf_simple_init(msg, 0);
	len = hex2bin(argv[1], msg->data, net_buf_simple_tailroom(msg) - 4);
	net_buf_simple_add(msg, len);

	err = bt_mesh_trans_send(&tx, msg, NULL, NULL);
	if (err)
	{
		printk("Failed to send (err %d)\n", err);
	}

	return 0;
}

static bool str_is_digit(char *s)
{
	while (*s != '\0')
	{
		if (!isdigit(*s))
			return false;
		else
			s++;
	}

	return true;
}

static bool str_is_xdigit(char *s)
{
	if (strncmp(s, "0x", strlen("0x")) == 0)
	{
		s += strlen("0x");
	}

	while (*s != '\0')
	{
		if (!isxdigit(*s))
			return false;
		else
			s++;
	}

	return true;
}

static int prepare_test_msg(struct net_buf_simple *msg)
{
	net_buf_simple_init(msg, 0);
	memcpy(msg->data, (const void *)NET_PRESSURE_TEST_STRING,
		   sizeof(NET_PRESSURE_TEST_STRING) - 1);
	net_buf_simple_add(msg, sizeof(NET_PRESSURE_TEST_STRING) - 1);
	return 0;
}

#define DEFAULT_PKT_INTERVAL 100
#define DEFAULT_PKT_CNT 3
extern long long k_now_ms();
extern void k_sleep(s32_t ms);
/* cmd: net-pressure-test <dst> <window> <packets-per-window> <duration> [cnt] */
static int cmd_net_pressure_test(int argc, char *argv[])
{
	struct net_buf_simple *msg = NET_BUF_SIMPLE(32);
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = net.net_idx,
		.app_idx = net.app_idx,

	};
	struct bt_mesh_net_tx tx = {
		.ctx = &ctx,
		.src = net.local,
		.xmit = BT_MESH_TRANSMIT(DEFAULT_PKT_CNT, DEFAULT_PKT_INTERVAL),
		.sub = bt_mesh_subnet_get(net.net_idx),
	};
	int err, window = 0, pkts = 0, dur = 0, i = 0, cnt = DEFAULT_PKT_CNT;
	long long start_time;

	if (argc < 5)
	{
		printk("%s failed, invalid argument number.\r\n", __func__);
		return -EINVAL;
	}

	if (!str_is_xdigit(argv[1]) || !str_is_digit(argv[2]) ||
		!str_is_digit(argv[3]) || !str_is_digit(argv[4]))
	{
		printk("%s failed, invalid argument.\r\n", __func__);
		return -EINVAL;
	}
	else
	{
		ctx.addr = strtoul(argv[1], NULL, 16);
		window = strtoul(argv[2], NULL, 0);
		pkts = strtoul(argv[3], NULL, 0);
		dur = strtoul(argv[4], NULL, 0);
	}

	if ((argc == 6) && str_is_digit(argv[5]))
	{
		cnt = strtoul(argv[5], NULL, 0);
		tx.xmit = BT_MESH_TRANSMIT(cnt, DEFAULT_PKT_INTERVAL);
	}

	if (((window * 1000) / pkts) < (cnt * DEFAULT_PKT_INTERVAL + 100))
	{
		printk("Cannot start the test, since the test "
			   "pkt interval is not set properly.\r\n");
		return -EINVAL;
	}

	printk("%s started\r\n", argv[0]);

	start_time = k_now_ms();
	while ((k_now_ms() - start_time) < (dur * 1000))
	{
		prepare_test_msg(msg);
		err = bt_mesh_trans_send(&tx, msg, NULL, NULL);
		k_sleep((window * 1000) / pkts);
		if (err)
		{
			printk("%s failed to send (err %d)\r\n", __func__, err);
		}
		else
		{
			i++;
			printk("%s test packet sent (No. %d)\r\n", __func__, i);
		}
	}

	printk("%s ended.\r\n", argv[0]);
	return 0;
}

#ifdef CONFIG_BT_MESH_BQB
static uint8_t char2u8(char c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	else if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	else if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	else
		return 0;
}

static void str2hex(uint8_t hex[], char *s, uint8_t cnt)
{
	uint8_t i;

	if (!s)
		return;

	for (i = 0; (*s != '\0') && (i < cnt); i++, s += 2)
	{
		hex[i] = ((char2u8(*s) & 0x0f) << 4) | ((char2u8(*(s + 1))) & 0x0f);
	}
}

int cmd_net_send2(int argc, char *argv[])
{
	u8_t pts_key_val[16] = {0};

	struct net_buf_simple *msg = NET_BUF_SIMPLE(32);
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		//.net_idx = net.net_idx,
		//.addr = net.dst,
		//.app_idx = net.app_idx,

	};
	struct bt_mesh_net_tx tx = {
		.ctx = &ctx,
		//.src = net.local,
		//.xmit = bt_mesh_net_transmit_get(),
		//.sub = bt_mesh_subnet_get(net.net_idx),
	};
	size_t len;
	int err;
	uint8_t aid = 0;

	ctx.send_ttl = (uint8_t)atol(argv[1]);
	ctx.net_idx = (uint16_t)atol(argv[2]);
	ctx.app_idx = (uint16_t)atol(argv[3]);
	ctx.addr = (uint16_t)atol(argv[4]);
	tx.src = (uint16_t)atol(argv[5]);
	tx.xmit = bt_mesh_net_transmit_get();
	tx.sub = bt_mesh_subnet_get(ctx.net_idx);
	aid = (uint8_t)atol(argv[6]);
	//str2hex(key_val, argv[7], 16);
	printk("ctx.send_ttl:            0x%04x\n"
		   "\tctx.net_idx:              0x%04x\n"
		   "\tctx.app_idx:              0x%04x\n"
		   "\tctx.addr:                 0x%04x\n"
		   "\ttx.src :                  0x%04x\n"
		   "\taid:                      0x%04x\n"
		   "\key_val:                   %s\n",

		   ctx.send_ttl, ctx.net_idx, ctx.app_idx, ctx.addr, tx.src, aid,
		   bt_hex(pts_key_val, 16));

	if (!tx.sub)
	{
		printk("No matching subnet for NetKey Index 0x%04x\n",
			   net.net_idx);
		return;
	}

	//str2hex(key_val, argv[6], 16);

	bt_mesh_app_key_set_manual(ctx.net_idx, ctx.app_idx, pts_key_val, aid);

	net_buf_simple_init(msg, 0);
	len = hex2bin(argv[7], msg->data, net_buf_simple_tailroom(msg) - 4);
	net_buf_simple_add(msg, len);

	err = bt_mesh_trans_send(&tx, msg, NULL, NULL);
	if (err)
	{
		printk("Failed to send (err %d)\n", err);
	}
}
#endif

#if defined(CONFIG_BT_MESH_IV_UPDATE_TEST)
static int cmd_iv_update(int argc, char *argv[])
{
	if (bt_mesh_iv_update())
	{
		printk("Transitioned to IV Update In Progress state\n");
	}
	else
	{
		printk("Transitioned to IV Update Normal state\n");
	}

	printk("IV Index is 0x%08x\n", bt_mesh.iv_index);

	return 0;
}

static int cmd_iv_update_test(int argc, char *argv[])
{
	bool enable;

	if (argc < 2)
	{
		return -EINVAL;
	}

	enable = str2bool(argv[1]);
	if (enable)
	{
		printk("Enabling IV Update test mode\n");
	}
	else
	{
		printk("Disabling IV Update test mode\n");
	}

	bt_mesh_iv_update_test(enable);

	return 0;
}
#endif

static int cmd_rpl_clear(int argc, char *argv[])
{
	bt_mesh_rpl_clear();
	return 0;
}
#ifdef CONFIG_BT_MESH_CFG_CLI

static int cmd_get_comp(int argc, char *argv[])
{
	struct net_buf_simple *comp = NET_BUF_SIMPLE(32);
	u8_t status, page = 0x00;
	int err;

	if (argc > 1)
	{
		page = strtol(argv[1], NULL, 0);
	}

	net_buf_simple_init(comp, 0);
	err = bt_mesh_cfg_comp_data_get(net.net_idx, net.dst, page,
									&status, comp);
	if (err)
	{
		printk("Getting composition failed (err %d)\n", err);
		return 0;
	}

	if (status != 0x00)
	{
		printk("Got non-success status 0x%02x\n", status);
		return 0;
	}

	printk("Got Composition Data for 0x%04x:\n", net.dst);
	printk("\tCID      0x%04x\n", net_buf_simple_pull_le16(comp));
	printk("\tPID      0x%04x\n", net_buf_simple_pull_le16(comp));
	printk("\tVID      0x%04x\n", net_buf_simple_pull_le16(comp));
	printk("\tCRPL     0x%04x\n", net_buf_simple_pull_le16(comp));
	printk("\tFeatures 0x%04x\n", net_buf_simple_pull_le16(comp));

	while (comp->len > 4)
	{
		u8_t sig, vnd;
		u16_t loc;
		int i;

		loc = net_buf_simple_pull_le16(comp);
		sig = net_buf_simple_pull_u8(comp);
		vnd = net_buf_simple_pull_u8(comp);

		printk("\n\tElement @ 0x%04x:\n", loc);

		if (comp->len < ((sig * 2) + (vnd * 4)))
		{
			printk("\t\t...truncated data!\n");
			break;
		}

		if (sig)
		{
			printk("\t\tSIG Models:\n");
		}
		else
		{
			printk("\t\tNo SIG Models\n");
		}

		for (i = 0; i < sig; i++)
		{
			u16_t mod_id = net_buf_simple_pull_le16(comp);

			printk("\t\t\t0x%04x\n", mod_id);
		}

		if (vnd)
		{
			printk("\t\tVendor Models:\n");
		}
		else
		{
			printk("\t\tNo Vendor Models\n");
		}

		for (i = 0; i < vnd; i++)
		{
			u16_t cid = net_buf_simple_pull_le16(comp);
			u16_t mod_id = net_buf_simple_pull_le16(comp);

			printk("\t\t\tCompany 0x%04x: 0x%04x\n", cid, mod_id);
		}
	}

	return 0;
}

static int cmd_beacon(int argc, char *argv[])
{
	u8_t status;
	int err;

	if (argc < 2)
	{
		err = bt_mesh_cfg_beacon_get(net.net_idx, net.dst, &status);
	}
	else
	{
		u8_t val = str2u8(argv[1]);

		err = bt_mesh_cfg_beacon_set(net.net_idx, net.dst, val,
									 &status);
	}

	if (err)
	{
		printk("Unable to send Beacon Get/Set message (err %d)\n", err);
		return 0;
	}

	printk("Beacon state is 0x%02x\n", status);

	return 0;
}

static int cmd_ttl(int argc, char *argv[])
{
	u8_t ttl;
	int err;

	if (argc < 2)
	{
		err = bt_mesh_cfg_ttl_get(net.net_idx, net.dst, &ttl);
	}
	else
	{
		u8_t val = strtoul(argv[1], NULL, 0);

		err = bt_mesh_cfg_ttl_set(net.net_idx, net.dst, val, &ttl);
	}

	if (err)
	{
		printk("Unable to send Default TTL Get/Set (err %d)\n", err);
		return 0;
	}

	printk("Default TTL is 0x%02x\n", ttl);

	return 0;
}

static int cmd_friend(int argc, char *argv[])
{
	u8_t frnd;
	int err;

	if (argc < 2)
	{
		err = bt_mesh_cfg_friend_get(net.net_idx, net.dst, &frnd);
	}
	else
	{
		u8_t val = str2u8(argv[1]);

		err = bt_mesh_cfg_friend_set(net.net_idx, net.dst, val, &frnd);
	}

	if (err)
	{
		printk("Unable to send Friend Get/Set (err %d)\n", err);
		return 0;
	}

	printk("Friend is set to 0x%02x\n", frnd);

	return 0;
}

static int cmd_gatt_proxy(int argc, char *argv[])
{
	u8_t proxy;
	int err;

	if (argc < 2)
	{
		err = bt_mesh_cfg_gatt_proxy_get(net.net_idx, net.dst, &proxy);
	}
	else
	{
		u8_t val = str2u8(argv[1]);

		err = bt_mesh_cfg_gatt_proxy_set(net.net_idx, net.dst, val,
										 &proxy);
	}

	if (err)
	{
		printk("Unable to send GATT Proxy Get/Set (err %d)\n", err);
		return 0;
	}

	printk("GATT Proxy is set to 0x%02x\n", proxy);

	return 0;
}

static int cmd_relay(int argc, char *argv[])
{
	u8_t relay, transmit;
	int err;

	if (argc < 2)
	{
		err = bt_mesh_cfg_relay_get(net.net_idx, net.dst, &relay,
									&transmit);
	}
	else
	{
		u8_t val = str2u8(argv[1]);
		u8_t count, interval, new_transmit;

		if (val)
		{
			if (argc > 2)
			{
				count = strtoul(argv[2], NULL, 0);
			}
			else
			{
				count = 2;
			}

			if (argc > 3)
			{
				interval = strtoul(argv[3], NULL, 0);
			}
			else
			{
				interval = 20;
			}

			new_transmit = BT_MESH_TRANSMIT(count, interval);
		}
		else
		{
			new_transmit = 0;
		}

		err = bt_mesh_cfg_relay_set(net.net_idx, net.dst, val,
									new_transmit, &relay, &transmit);
	}

	if (err)
	{
		printk("Unable to send Relay Get/Set (err %d)\n", err);
		return 0;
	}

	printk("Relay is 0x%02x, Transmit 0x%02x (count %u interval %ums)\n",
		   relay, transmit, BT_MESH_TRANSMIT_COUNT(transmit),
		   BT_MESH_TRANSMIT_INT(transmit));

	return 0;
}

static int cmd_net_key_add(int argc, char *argv[])
{
	u8_t key_val[16];
	u16_t key_net_idx;
	u8_t status;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);

	if (argc > 2)
	{
		size_t len;

		len = hex2bin(argv[3], key_val, sizeof(key_val));
		memset(key_val, 0, sizeof(key_val) - len);
	}
	else
	{
		memcpy(key_val, default_key, sizeof(key_val));
	}

	err = bt_mesh_cfg_net_key_add(net.net_idx, net.dst, key_net_idx,
								  key_val, &status);
	if (err)
	{
		printk("Unable to send NetKey Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("NetKeyAdd failed with status 0x%02x\n", status);
	}
	else
	{
		printk("NetKey added with NetKey Index 0x%03x\n", key_net_idx);
	}

	return 0;
}

static int cmd_net_key_get(int argc, char *argv[])
{
	u8_t status;
	int err;

	err = bt_mesh_cfg_net_key_get(net.net_idx, net.dst, &status);

	if (err)
	{
		printk("Unable to send NetKey Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("NetKeyGet failed with status 0x%02x\n", status);
	}
	return 0;
}

static int cmd_net_key_update(int argc, char *argv[])
{
	u8_t key_val[16];
	u16_t key_net_idx;
	u8_t status;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);

	if (argc > 2)
	{
		size_t len;

		len = hex2bin(argv[3], key_val, sizeof(key_val));
		memset(key_val, 0, sizeof(key_val) - len);
	}
	else
	{
		memcpy(key_val, default_key, sizeof(key_val));
	}

	err = bt_mesh_cfg_net_key_update(net.net_idx, net.dst, key_net_idx,
									 key_val, &status);
	if (err)
	{
		printk("Unable to send NetKey update (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("NetKeyUpdate failed with status 0x%02x\n", status);
	}
	else
	{
		printk("NetKey updated with NetKey Index 0x%03x\n", key_net_idx);
	}

	return 0;
}

static int cmd_net_key_del(int argc, char *argv[])
{
	u16_t key_net_idx;
	u8_t status;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);

	err = bt_mesh_cfg_net_key_del(net.net_idx, net.dst, key_net_idx, &status);
	if (err)
	{
		printk("Unable to send NetKey delete (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("NetKey delete failed with status 0x%02x\n", status);
	}
	else
	{
		printk("NetKey deleted with NetKey Index 0x%03x\n", key_net_idx);
	}

	return 0;
}

static int cmd_app_key_add(int argc, char *argv[])
{
	u8_t key_val[16];
	u16_t key_net_idx, key_app_idx;
	u8_t status;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);
	key_app_idx = strtoul(argv[2], NULL, 0);

	if (argc > 3)
	{
		size_t len;

		len = hex2bin(argv[3], key_val, sizeof(key_val));
		memset(key_val, 0, sizeof(key_val) - len);
	}
	else
	{
		memcpy(key_val, default_key, sizeof(key_val));
	}

	err = bt_mesh_cfg_app_key_add(net.net_idx, net.dst, key_net_idx,
								  key_app_idx, key_val, &status);
	if (err)
	{
		printk("Unable to send App Key Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("AppKeyAdd failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey added, NetKeyIndex 0x%04x AppKeyIndex 0x%04x\n",
			   key_net_idx, key_app_idx);
	}

	return 0;
}

static int cmd_app_key_del(int argc, char *argv[])
{
	u16_t key_net_idx, key_app_idx;
	u8_t status;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);
	key_app_idx = strtoul(argv[2], NULL, 0);

	err = bt_mesh_cfg_app_key_del(net.net_idx, net.dst, key_net_idx, key_app_idx, &status);
	if (err)
	{
		printk("Unable to send App Key Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("AppKeyAdd failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey added, NetKeyIndex 0x%04x AppKeyIndex 0x%04x\n",
			   key_net_idx, key_app_idx);
	}

	return 0;
}

static int cmd_app_key_update(int argc, char *argv[])
{
	u8_t key_val[16];
	u16_t key_net_idx, key_app_idx;
	u8_t status;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);
	key_app_idx = strtoul(argv[2], NULL, 0);

	if (argc > 3)
	{
		size_t len;

		len = hex2bin(argv[3], key_val, sizeof(key_val));
		memset(key_val, 0, sizeof(key_val) - len);
	}
	else
	{
		memcpy(key_val, default_key, sizeof(key_val));
	}

	err = bt_mesh_cfg_app_key_update(net.net_idx, net.dst, key_net_idx,
									 key_app_idx, key_val, &status);
	if (err)
	{
		printk("Unable to send App Key Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("AppKeyAdd failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey added, NetKeyIndex 0x%04x AppKeyIndex 0x%04x\n",
			   key_net_idx, key_app_idx);
	}

	return 0;
}

static int cmd_app_key_get(int argc, char *argv[])
{
	u16_t key_net_idx, key_app_idx = 0;
	u8_t status;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	key_net_idx = strtoul(argv[1], NULL, 0);

	err = bt_mesh_cfg_app_key_get(net.net_idx, net.dst, key_net_idx, &status);
	if (err)
	{
		printk("Unable to send App Key Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("AppKeyAdd failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey added, NetKeyIndex 0x%04x AppKeyIndex 0x%04x\n",
			   key_net_idx, key_app_idx);
	}

	return 0;
}

static int cmd_mod_app_bind(int argc, char *argv[])
{
	u16_t elem_addr, mod_app_idx, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	mod_app_idx = strtoul(argv[2], NULL, 0);
	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_app_bind_vnd(net.net_idx, net.dst,
										   elem_addr, mod_app_idx,
										   mod_id, cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_app_bind(net.net_idx, net.dst, elem_addr,
									   mod_app_idx, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model App Bind (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Model App Bind failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey successfully bound\n");
	}

	return 0;
}

static int cmd_mod_app_unbind(int argc, char *argv[])
{
	u16_t elem_addr, mod_app_idx, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	mod_app_idx = strtoul(argv[2], NULL, 0);
	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_app_unbind_vnd(net.net_idx, net.dst,
											 elem_addr, mod_app_idx,
											 mod_id, cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_app_unbind(net.net_idx, net.dst, elem_addr,
										 mod_app_idx, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model App Unbind (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Model App Unbind failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey successfully Unbound\n");
	}

	return 0;
}

static int cmd_mod_app_get(int argc, char *argv[])
{
	u16_t elem_addr, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	mod_id = strtoul(argv[2], NULL, 0);

	if (argc > 3)
	{
		cid = strtoul(argv[3], NULL, 0);
		err = bt_mesh_cfg_mod_app_get_vnd(net.net_idx, net.dst,
										  elem_addr, mod_id, cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_app_get(net.net_idx, net.dst, elem_addr, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model App Get (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Model App Get failed with status 0x%02x\n", status);
	}
	else
	{
		printk("AppKey successfully Unbound\n");
	}

	return 0;
}

static int cmd_mod_sub_add(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	sub_addr = strtoul(argv[2], NULL, 0);
	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_sub_add_vnd(net.net_idx, net.dst,
										  elem_addr, sub_addr, mod_id,
										  cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_add(net.net_idx, net.dst, elem_addr,
									  sub_addr, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Add failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("Model subscription was successful\n");
	}

	return 0;
}

static int cmd_mod_sub_del(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	sub_addr = strtoul(argv[2], NULL, 0);
	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_sub_del_vnd(net.net_idx, net.dst,
										  elem_addr, sub_addr, mod_id,
										  cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_del(net.net_idx, net.dst, elem_addr,
									  sub_addr, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription Delete (err %d)\n",
			   err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Delete failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("Model subscription deltion was successful\n");
	}

	return 0;
}

static int cmd_mod_sub_del_all(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr = 0, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	mod_id = strtoul(argv[2], NULL, 0);

	if (argc > 3)
	{
		cid = strtoul(argv[3], NULL, 0);
		err = bt_mesh_cfg_mod_sub_del_all_vnd(net.net_idx, net.dst,
											  elem_addr, sub_addr, mod_id,
											  cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_del_all(net.net_idx, net.dst, elem_addr,
										  sub_addr, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription Delete All (err %d)\n",
			   err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Delete All failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("Model subscription all deltion was successful\n");
	}

	return 0;
}

static int cmd_mod_sub_overwrite(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	sub_addr = strtoul(argv[2], NULL, 0);
	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_sub_overwrite_vnd(net.net_idx, net.dst,
												elem_addr, sub_addr, mod_id,
												cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_overwrite(net.net_idx, net.dst, elem_addr,
											sub_addr, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription Overwrite (err %d)\n",
			   err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Overwrite failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("Model subscription Overwrite was successful\n");
	}

	return 0;
}
static int cmd_mod_sub_get(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr = 0, mod_id, cid;
	u8_t status;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);
	mod_id = strtoul(argv[2], NULL, 0);

	if (argc > 3)
	{
		cid = strtoul(argv[3], NULL, 0);
		err = bt_mesh_cfg_mod_sub_get_vnd(net.net_idx, net.dst,
										  elem_addr, sub_addr, mod_id,
										  cid, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_get(net.net_idx, net.dst, elem_addr,
									  sub_addr, mod_id, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription Get (err %d)\n",
			   err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Get failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("Model subscription get was successful\n");
	}

	return 0;
}
static int cmd_mod_sub_add_va(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr, mod_id, cid;
	u8_t label[16];
	u8_t status;
	size_t len;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);

	len = hex2bin(argv[2], label, sizeof(label));
	memset(label + len, 0, sizeof(label) - len);

	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_sub_va_add_vnd(net.net_idx, net.dst,
											 elem_addr, label, mod_id,
											 cid, &sub_addr, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_va_add(net.net_idx, net.dst,
										 elem_addr, label, mod_id,
										 &sub_addr, &status);
	}

	if (err)
	{
		printk("Unable to send Mod Sub VA Add (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Mod Sub VA Add failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("0x%04x subscribed to Label UUID %s (va 0x%04x)\n",
			   elem_addr, argv[2], sub_addr);
	}

	return 0;
}

static int cmd_mod_sub_del_va(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr, mod_id, cid;
	u8_t label[16];
	u8_t status;
	size_t len;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);

	len = hex2bin(argv[2], label, sizeof(label));
	memset(label + len, 0, sizeof(label) - len);

	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_sub_va_del_vnd(net.net_idx, net.dst,
											 elem_addr, label, mod_id,
											 cid, &sub_addr, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_va_del(net.net_idx, net.dst,
										 elem_addr, label, mod_id,
										 &sub_addr, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription Delete (err %d)\n",
			   err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Delete failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("0x%04x unsubscribed from Label UUID %s (va 0x%04x)\n",
			   elem_addr, argv[2], sub_addr);
	}

	return 0;
}

static int cmd_mod_sub_overwrite_va(int argc, char *argv[])
{
	u16_t elem_addr, sub_addr, mod_id, cid;
	u8_t label[16];
	u8_t status;
	size_t len;
	int err;

	if (argc < 4)
	{
		return -EINVAL;
	}

	elem_addr = strtoul(argv[1], NULL, 0);

	len = hex2bin(argv[2], label, sizeof(label));
	memset(label + len, 0, sizeof(label) - len);

	mod_id = strtoul(argv[3], NULL, 0);

	if (argc > 4)
	{
		cid = strtoul(argv[4], NULL, 0);
		err = bt_mesh_cfg_mod_sub_va_overwrite_vnd(net.net_idx, net.dst,
												   elem_addr, label, mod_id,
												   cid, &sub_addr, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_sub_va_overwrite(net.net_idx, net.dst,
											   elem_addr, label, mod_id,
											   &sub_addr, &status);
	}

	if (err)
	{
		printk("Unable to send Model Subscription VA overwrite (err %d)\n",
			   err);
		return 0;
	}

	if (status)
	{
		printk("Model Subscription Overwrite failed with status 0x%02x\n",
			   status);
	}
	else
	{
		printk("0x%04x unsubscribed from Label UUID %s (va 0x%04x)\n",
			   elem_addr, argv[2], sub_addr);
	}

	return 0;
}

static int mod_pub_get(u16_t addr, u16_t mod_id, u16_t cid)
{
	struct bt_mesh_cfg_mod_pub pub;
	u8_t status;
	int err;

	if (cid == CID_NVAL)
	{
		err = bt_mesh_cfg_mod_pub_get(net.net_idx, net.dst, addr,
									  mod_id, &pub, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_pub_get_vnd(net.net_idx, net.dst, addr,
										  mod_id, cid, &pub, &status);
	}

	if (err)
	{
		printk("Model Publication Get failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Model Publication Get failed (status 0x%02x)\n",
			   status);
		return 0;
	}

	printk("Model Publication for Element 0x%04x, Model 0x%04x:\n"
		   "\tPublish Address:                0x%04x\n"
		   "\tAppKeyIndex:                    0x%04x\n"
		   "\tCredential Flag:                %u\n"
		   "\tPublishTTL:                     %u\n"
		   "\tPublishPeriod:                  0x%02x\n"
		   "\tPublishRetransmitCount:         %u\n"
		   "\tPublishRetransmitInterval:      %ums\n",
		   addr, mod_id, pub.addr, pub.app_idx, pub.cred_flag, pub.ttl,
		   pub.period, BT_MESH_PUB_TRANSMIT_COUNT(pub.transmit),
		   BT_MESH_PUB_TRANSMIT_INT(pub.transmit));

	return 0;
}

static int mod_pub_set(u16_t addr, u16_t mod_id, u16_t cid, char *argv[])
{
	struct bt_mesh_cfg_mod_pub pub;
	u8_t status, count;
	u16_t interval;
	int err;
	u8_t uuid[16];

	memset(&pub, 0, sizeof(pub));

	if (strlen(argv[0]) > 20)
	{
		hex2bin(argv[0], uuid, sizeof(uuid));
		pub.uuid = uuid;
		printf("uuid %s\r\n", bt_hex(pub.uuid, sizeof(pub.uuid)));
	}
	else
	{
		pub.addr = strtoul(argv[0], NULL, 0);
	}

	pub.app_idx = strtoul(argv[1], NULL, 0);
	pub.cred_flag = str2bool(argv[2]);
	pub.ttl = strtoul(argv[3], NULL, 0);
	pub.period = strtoul(argv[4], NULL, 0);

	count = strtoul(argv[5], NULL, 0);
	if (count > 7)
	{
		printk("Invalid retransmit count\n");
		return -EINVAL;
	}

	interval = strtoul(argv[6], NULL, 0);
	if (interval > (31 * 50) || (interval % 50))
	{
		printk("Invalid retransmit interval %u\n", interval);
		return -EINVAL;
	}

	pub.transmit = BT_MESH_PUB_TRANSMIT(count, interval);

	if (cid == CID_NVAL)
	{
		err = bt_mesh_cfg_mod_pub_set(net.net_idx, net.dst, addr,
									  mod_id, &pub, &status);
	}
	else
	{
		err = bt_mesh_cfg_mod_pub_set_vnd(net.net_idx, net.dst, addr,
										  mod_id, cid, &pub, &status);
	}

	if (err)
	{
		printk("Model Publication Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Model Publication Set failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Model Publication successfully set\n");
	}

	return 0;
}

static int cmd_mod_pub(int argc, char *argv[])
{
	u16_t addr, mod_id, cid;

	if (argc < 3)
	{
		return -EINVAL;
	}

	addr = strtoul(argv[1], NULL, 0);
	mod_id = strtoul(argv[2], NULL, 0);

	argc -= 3;
	argv += 3;

	if (argc == 1 || argc == 9)
	{
		cid = strtoul(argv[0], NULL, 0);
		argc--;
		argv++;
	}
	else
	{
		cid = CID_NVAL;
	}

	if (argc > 0)
	{
		if (argc < 7)
		{
			return -EINVAL;
		}

		return mod_pub_set(addr, mod_id, cid, argv);
	}
	else
	{
		return mod_pub_get(addr, mod_id, cid);
	}
}
static int cmd_lpn_timeout_get(int argc, char *argv[])
{
	u8_t status;
	int err;
	u16_t lpn_addr;

	lpn_addr = strtoul(argv[1], NULL, 0);

	err = bt_mesh_cfg_lpn_timeout_get(net.net_idx, net.dst, lpn_addr, &status);

	if (err)
	{
		printk("send Network transmit Get/Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Network transmit Get/Set failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Network transmit Get/Set successfully\n");
	}
	return 0;
}

static void hb_sub_print(struct bt_mesh_cfg_hb_sub *sub)
{
	printk("Heartbeat Subscription:\n"
		   "\tSource:      0x%04x\n"
		   "\tDestination: 0x%04x\n"
		   "\tPeriodLog:   0x%02x\n"
		   "\tCountLog:    0x%02x\n"
		   "\tMinHops:     %u\n"
		   "\tMaxHops:     %u\n",
		   sub->src, sub->dst, sub->period, sub->count,
		   sub->min, sub->max);
}

static int hb_sub_get(int argc, char *argv[])
{
	struct bt_mesh_cfg_hb_sub sub;
	u8_t status;
	int err;

	err = bt_mesh_cfg_hb_sub_get(net.net_idx, net.dst, &sub, &status);
	if (err)
	{
		printk("Heartbeat Subscription Get failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Heartbeat Subscription Get failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		hb_sub_print(&sub);
	}

	return 0;
}

static int hb_sub_set(int argc, char *argv[])
{
	struct bt_mesh_cfg_hb_sub sub;
	u8_t status;
	int err;

	sub.src = strtoul(argv[1], NULL, 0);
	sub.dst = strtoul(argv[2], NULL, 0);
	sub.period = strtoul(argv[3], NULL, 0);

	err = bt_mesh_cfg_hb_sub_set(net.net_idx, net.dst, &sub, &status);
	if (err)
	{
		printk("Heartbeat Subscription Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Heartbeat Subscription Set failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		hb_sub_print(&sub);
	}

	return 0;
}

static int cmd_hb_sub(int argc, char *argv[])
{
	if (argc > 1)
	{
		if (argc < 4)
		{
			return -EINVAL;
		}

		return hb_sub_set(argc, argv);
	}
	else
	{
		return hb_sub_get(argc, argv);
	}
}

static int hb_pub_get(int argc, char *argv[])
{
	struct bt_mesh_cfg_hb_pub pub;
	u8_t status;
	int err;

	err = bt_mesh_cfg_hb_pub_get(net.net_idx, net.dst, &pub, &status);
	if (err)
	{
		printk("Heartbeat Publication Get failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Heartbeat Publication Get failed (status 0x%02x)\n",
			   status);
		return 0;
	}

	printk("Heartbeat publication:\n");
	printk("\tdst 0x%04x count 0x%02x period 0x%02x\n",
		   pub.dst, pub.count, pub.period);
	printk("\tttl 0x%02x feat 0x%04x net_idx 0x%04x\n",
		   pub.ttl, pub.feat, pub.net_idx);

	return 0;
}

static int hb_pub_set(int argc, char *argv[])
{
	struct bt_mesh_cfg_hb_pub pub;
	u8_t status;
	int err;

	pub.dst = strtoul(argv[1], NULL, 0);
	pub.count = strtoul(argv[2], NULL, 0);
	pub.period = strtoul(argv[3], NULL, 0);
	pub.ttl = strtoul(argv[4], NULL, 0);
	pub.feat = strtoul(argv[5], NULL, 0);
	pub.net_idx = strtoul(argv[6], NULL, 0);

	err = bt_mesh_cfg_hb_pub_set(net.net_idx, net.dst, &pub, &status);
	if (err)
	{
		printk("Heartbeat Publication Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Heartbeat Publication Set failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Heartbeat publication successfully set\n");
	}

	return 0;
}

static int cmd_hb_pub(int argc, char *argv[])
{
	if (argc > 1)
	{
		if (argc < 7)
		{
			return -EINVAL;
		}

		return hb_pub_set(argc, argv);
	}
	else
	{
		return hb_pub_get(argc, argv);
	}
}

/*[Genie begin] add by wenbing.cwb at 2021-01-21*/
#ifdef CONFIG_BT_MESH_CTRL_RELAY
static int ctrl_relay_get(int argc, char *argv[])
{
	struct ctrl_relay_param cr;
	u8_t status;
	int err;

	err = bt_mesh_cfg_ctrl_relay_get(net.net_idx, net.dst, &cr, &status);
	if (err)
	{
		printk("Ctrl Relay Get failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Ctrl Relay Get failed (status 0x%02x)\n", status);
		return 0;
	}

	printk("Ctrl Relay Cfg:\n");
	printk("enable:%d N:%d rssi:%d\n", cr.enable, cr.trd_n, cr.rssi);
	printk("status_period:%d check_period:%d request_period:%d\n", cr.sta_period, cr.chk_period, cr.req_period);

	return 0;
}

static int ctrl_relay_set(int argc, char *argv[])
{
	struct ctrl_relay_param cr;
	u8_t status;
	int err;

	cr.enable = strtoul(argv[1], NULL, 0);
	cr.trd_n = strtoul(argv[2], NULL, 0);
	cr.rssi = strtoul(argv[3], NULL, 0);
	cr.sta_period = strtoul(argv[4], NULL, 0);
	cr.chk_period = strtoul(argv[5], NULL, 0);
	cr.req_period = strtoul(argv[6], NULL, 0);

	err = bt_mesh_cfg_ctrl_relay_set(net.net_idx, net.dst, &cr, &status);
	if (err)
	{
		printk("Ctrl Relay Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Ctrl Relay Set failed (status 0x%02x)\n", status);
	}
	else
	{
		printk("Ctrl Relay successfully set\n");
	}

	return 0;
}

static int cmd_ctrl_relay(int argc, char *argv[])
{
	if (argc > 1)
	{
		if (argc < 7)
		{
			return -EINVAL;
		}

		return ctrl_relay_set(argc, argv);
	}
	else
	{
		return ctrl_relay_get(argc, argv);
	}
}
#endif
/*[Genie end] add by wenbing.cwb at 2021-01-21*/
#endif /*CONFIG_BT_MESH_CFG_CLI*/

static int cmd_node_ident(int argc, char *argv[])
{
	u16_t netkey_idx;
	u8_t ident_state;
	u8_t status;
	int err;

	netkey_idx = strtoul(argv[1], NULL, 0);
	if (argc == 2)
	{
		err = bt_mesh_cfg_node_ident_get(netkey_idx, net.dst, net.dst, &status);
	}
	else
	{
		ident_state = strtoul(argv[2], NULL, 0);
		err = bt_mesh_cfg_node_ident_set(netkey_idx, net.dst, net.dst, ident_state, &status);
	}

	if (err)
	{
		printk("Node Identify Set/Get failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Node Identify Set/Get failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Node Identify Set/Get successfully\n");
	}

	return 0;
}

static int cmd_node_reset(int argc, char *argv[])
{
	u8_t status;
	int err;

	err = bt_mesh_cfg_node_reset(net.net_idx, net.dst, &status);

	if (err)
	{
		printk("send Node Reset failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Node Reset failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Node Reset successfully\n");
	}
	return 0;
}

static int cmd_network_transmit(int argc, char *argv[])
{
	u8_t status;
	int err;
	u8_t count, interval;
	u8_t transmit;

	if (argc < 3)
	{
		err = bt_mesh_cfg_network_transmit_get(net.net_idx, net.dst, &status);
	}
	else
	{
		count = strtoul(argv[1], NULL, 0);
		if (count > 7)
		{
			printk("Invalid retransmit count\n");
			return -EINVAL;
		}

		interval = strtoul(argv[2], NULL, 0);
		if (interval > (31 * 10) || (interval % 10))
		{
			printk("Invalid retransmit interval %u\n", interval);
			return -EINVAL;
		}

		transmit = BT_MESH_TRANSMIT(count, interval);
		err = bt_mesh_cfg_network_transmit_set(net.net_idx, net.dst, transmit, &status);
	}

	if (err)
	{
		printk("send Network transmit Get/Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Network transmit Get/Set failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Network transmit Get/Set successfully\n");
	}
	return 0;
}

static int cmd_key_refresh(int argc, char *argv[])
{
	u8_t status;
	int err;
	u8_t phase;

	if (argc < 2)
	{
		err = bt_mesh_cfg_krp_get(net.net_idx, net.dst, net.net_idx, &status);
	}
	else
	{
		phase = strtoul(argv[1], NULL, 0);
		err = bt_mesh_cfg_krp_set(net.net_idx, net.dst, net.net_idx, phase, &status);
	}

	if (err)
	{
		printk("send Network transmit Get/Set failed (err %d)\n", err);
		return 0;
	}

	if (status)
	{
		printk("Network transmit Get/Set failed (status 0x%02x)\n",
			   status);
	}
	else
	{
		printk("Network transmit Get/Set successfully\n");
	}
	return 0;
}

static int cmd_beacon_kr(int argc, char *argv[])
{
	u8_t state;

	if (argc < 2)
	{
		printk("cmd_beacon_kr Wrong parameter\r\n");
		return -EINVAL;
	}
	state = str2u8(argv[1]);

	bt_mesh.sub[0].kr_flag = state;
	bt_mesh_kr_update(&bt_mesh.sub[0], state, false);
	//bt_mesh_net_sec_update(&bt_mesh.sub[0]);
	bt_mesh_net_beacon_update(&bt_mesh.sub[0]);

	printf("cmd_beacon_kr kr_flag = %d\r\n", bt_mesh.sub[0].kr_flag);
	return 0;
}

#if defined(CONFIG_BT_MESH_PROV)
static int cmd_pb(bt_mesh_prov_bearer_t bearer, int argc, char *argv[])
{
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	if (str2bool(argv[1]))
	{
		err = bt_mesh_prov_enable(bearer);
		if (err)
		{
			printk("Failed to enable %s (err %d)\n",
				   bearer2str(bearer), err);
		}
		else
		{
			printk("%s enabled\n", bearer2str(bearer));
		}
	}
	else
	{
		err = bt_mesh_prov_disable(bearer);
		if (err)
		{
			printk("Failed to disable %s (err %d)\n",
				   bearer2str(bearer), err);
		}
		else
		{
			printk("%s disabled\n", bearer2str(bearer));
		}
	}

	return 0;
}
#endif

#if defined(CONFIG_BT_MESH_PB_ADV)
static int cmd_pb_adv(int argc, char *argv[])
{
#if defined(CONFIG_BT_MESH_PROV)
	return cmd_pb(BT_MESH_PROV_ADV, argc, argv);
#endif
	return 0;
}
#endif /* CONFIG_BT_MESH_PB_ADV */

#if defined(CONFIG_BT_MESH_PB_GATT)
static int cmd_pb_gatt(int argc, char *argv[])
{
#if defined(CONFIG_BT_MESH_PROV)
	return cmd_pb(BT_MESH_PROV_GATT, argc, argv);
#endif
	return 0;
}
#endif /* CONFIG_BT_MESH_PB_GATT */

static int cmd_provision(int argc, char *argv[])
{
	u16_t net_idx, addr;
	u32_t iv_index;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	net_idx = strtoul(argv[1], NULL, 0);
	addr = strtoul(argv[2], NULL, 0);

	if (argc > 3)
	{
		iv_index = strtoul(argv[1], NULL, 0);
	}
	else
	{
		iv_index = 0;
	}

	err = bt_mesh_provision(default_key, net_idx, 0, iv_index, 0, addr,
							default_key);
	if (err)
	{
		printk("Provisioning failed (err %d)\n", err);
	}

	return 0;
}

int cmd_timeout(int argc, char *argv[])
{
	s32_t timeout;

	if (argc < 2)
	{
		timeout = bt_mesh_cfg_cli_timeout_get();
		if (timeout == K_FOREVER)
		{
			printk("Message timeout: forever\n");
		}
		else
		{
			printk("Message timeout: %u seconds\n",
				   timeout / 1000);
		}

		return 0;
	}

	timeout = strtol(argv[1], NULL, 0);
	if (timeout < 0 || timeout > (INT32_MAX / 1000))
	{
		timeout = K_FOREVER;
	}
	else
	{
		timeout = timeout * 1000;
	}

	bt_mesh_cfg_cli_timeout_set(timeout);
	if (timeout == K_FOREVER)
	{
		printk("Message timeout: forever\n");
	}
	else
	{
		printk("Message timeout: %u seconds\n",
			   timeout / 1000);
	}

	return 0;
}

#ifdef CONFIG_BT_MESH_HEALTH_CLI
static int cmd_fault_get(int argc, char *argv[])
{
	u8_t faults[32];
	size_t fault_count;
	u8_t test_id;
	u16_t cid;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	cid = strtoul(argv[1], NULL, 0);
	fault_count = sizeof(faults);

	err = bt_mesh_health_fault_get(net.net_idx, net.dst, net.app_idx, cid,
								   &test_id, faults, &fault_count);
	if (err)
	{
		printk("Failed to send Health Fault Get (err %d)\n", err);
	}
	else
	{
		show_faults(test_id, cid, faults, fault_count);
	}

	return 0;
}

static int cmd_fault_clear(int argc, char *argv[])
{
	u8_t faults[32];
	size_t fault_count;
	u8_t test_id;
	u16_t cid;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	cid = strtoul(argv[1], NULL, 0);
	fault_count = sizeof(faults);

	err = bt_mesh_health_fault_clear(net.net_idx, net.dst, net.app_idx,
									 cid, &test_id, faults, &fault_count);
	if (err)
	{
		printk("Failed to send Health Fault Clear (err %d)\n", err);
	}
	else
	{
		show_faults(test_id, cid, faults, fault_count);
	}

	return 0;
}

static int cmd_fault_clear_unack(int argc, char *argv[])
{
	u16_t cid;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	cid = strtoul(argv[1], NULL, 0);

	err = bt_mesh_health_fault_clear(net.net_idx, net.dst, net.app_idx,
									 cid, NULL, NULL, NULL);
	if (err)
	{
		printk("Health Fault Clear Unacknowledged failed (err %d)\n",
			   err);
	}

	return 0;
}

static int cmd_fault_test(int argc, char *argv[])
{
	u8_t faults[32];
	size_t fault_count;
	u8_t test_id;
	u16_t cid;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	cid = strtoul(argv[1], NULL, 0);
	test_id = strtoul(argv[2], NULL, 0);
	fault_count = sizeof(faults);

	err = bt_mesh_health_fault_test(net.net_idx, net.dst, net.app_idx,
									cid, test_id, faults, &fault_count);
	if (err)
	{
		printk("Failed to send Health Fault Test (err %d)\n", err);
	}
	else
	{
		show_faults(test_id, cid, faults, fault_count);
	}

	return 0;
}

static int cmd_fault_test_unack(int argc, char *argv[])
{
	u16_t cid;
	u8_t test_id;
	int err;

	if (argc < 3)
	{
		return -EINVAL;
	}

	cid = strtoul(argv[1], NULL, 0);
	test_id = strtoul(argv[2], NULL, 0);

	err = bt_mesh_health_fault_test(net.net_idx, net.dst, net.app_idx,
									cid, test_id, NULL, NULL);
	if (err)
	{
		printk("Health Fault Test Unacknowledged failed (err %d)\n",
			   err);
	}

	return 0;
}

static int cmd_period_get(int argc, char *argv[])
{
	u8_t divisor;
	int err;

	err = bt_mesh_health_period_get(net.net_idx, net.dst, net.app_idx,
									&divisor);
	if (err)
	{
		printk("Failed to send Health Period Get (err %d)\n", err);
	}
	else
	{
		printk("Health FastPeriodDivisor: %u\n", divisor);
	}

	return 0;
}

static int cmd_period_set(int argc, char *argv[])
{
	u8_t divisor, updated_divisor;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	divisor = strtoul(argv[1], NULL, 0);

	err = bt_mesh_health_period_set(net.net_idx, net.dst, net.app_idx,
									divisor, &updated_divisor);
	if (err)
	{
		printk("Failed to send Health Period Set (err %d)\n", err);
	}
	else
	{
		printk("Health FastPeriodDivisor: %u\n", updated_divisor);
	}

	return 0;
}

static int cmd_period_set_unack(int argc, char *argv[])
{
	u8_t divisor;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	divisor = strtoul(argv[1], NULL, 0);

	err = bt_mesh_health_period_set(net.net_idx, net.dst, net.app_idx,
									divisor, NULL);
	if (err)
	{
		printk("Failed to send Health Period Set (err %d)\n", err);
	}

	return 0;
}

static int cmd_attention_get(int argc, char *argv[])
{
	u8_t attention;
	int err;

	err = bt_mesh_health_attention_get(net.net_idx, net.dst, net.app_idx,
									   &attention);
	if (err)
	{
		printk("Failed to send Health Attention Get (err %d)\n", err);
	}
	else
	{
		printk("Health Attention Timer: %u\n", attention);
	}

	return 0;
}

static int cmd_attention_set(int argc, char *argv[])
{
	u8_t attention, updated_attention;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	attention = strtoul(argv[1], NULL, 0);

	err = bt_mesh_health_attention_set(net.net_idx, net.dst, net.app_idx,
									   attention, &updated_attention);
	if (err)
	{
		printk("Failed to send Health Attention Set (err %d)\n", err);
	}
	else
	{
		printk("Health Attention Timer: %u\n", updated_attention);
	}

	return 0;
}

static int cmd_attention_set_unack(int argc, char *argv[])
{
	u8_t attention;
	int err;

	if (argc < 2)
	{
		return -EINVAL;
	}

	attention = strtoul(argv[1], NULL, 0);

	err = bt_mesh_health_attention_set(net.net_idx, net.dst, net.app_idx,
									   attention, NULL);
	if (err)
	{
		printk("Failed to send Health Attention Set (err %d)\n", err);
	}

	return 0;
}
#endif

extern int bt_mesh_fault_update(struct bt_mesh_elem *elem);
static int cmd_add_fault(int argc, char *argv[])
{
	u8_t fault_id;
	u8_t i;

	if (argc < 2)
	{
		return -EINVAL;
	}

	fault_id = strtoul(argv[1], NULL, 0);
	if (!fault_id)
	{
		printk("The Fault ID must be non-zero!\n");
		return -EINVAL;
	}

	for (i = 0; i < sizeof(cur_faults); i++)
	{
		if (!cur_faults[i])
		{
			cur_faults[i] = fault_id;
			break;
		}
	}

	if (i == sizeof(cur_faults))
	{
		printk("Fault array is full. Use \"del-fault\" to clear it\n");
		return 0;
	}

	for (i = 0; i < sizeof(reg_faults); i++)
	{
		if (!reg_faults[i])
		{
			reg_faults[i] = fault_id;
			break;
		}
	}

	if (i == sizeof(reg_faults))
	{
		printk("No space to store more registered faults\n");
	}

	bt_mesh_fault_update(&elements[0]);

	return 0;
}

static int cmd_del_fault(int argc, char *argv[])
{
	u8_t fault_id;
	u8_t i;

	if (argc < 2)
	{
		memset(cur_faults, 0, sizeof(cur_faults));
		printk("All current faults cleared\n");
		bt_mesh_fault_update(&elements[0]);
		return 0;
	}

	fault_id = strtoul(argv[1], NULL, 0);
	if (!fault_id)
	{
		printk("The Fault ID must be non-zero!\n");
		return -EINVAL;
	}

	for (i = 0; i < sizeof(cur_faults); i++)
	{
		if (cur_faults[i] == fault_id)
		{
			cur_faults[i] = 0;
			printk("Fault cleared\n");
		}
	}

	bt_mesh_fault_update(&elements[0]);

	return 0;
}

static void print_all_help(struct mesh_shell_cmd *cmds)
{
	while (cmds->cmd_name != NULL)
	{
		printk("%s", cmds->cmd_name);
		if (cmds->help != NULL)
		{
			printk(": %s\r\n", cmds->help);
		}
		else
		{
			printk("\r\n");
		}
		cmds++;
	}
}

static int cmd_display_help(int argc, char *argv[])
{
	char *cmd = NULL, *help_str = NULL;
	struct mesh_shell_cmd *cmd_entry;

	cmd_entry = bt_mesh_get_shell_cmd_list();

	if ((argc == 0) || (argc == 1 && strcmp(argv[0], "help") == 0))
	{
		print_all_help(cmd_entry);
		return 0;
	}

	cmd = argv[0];
	if (!cmd_entry)
	{
		printk("No command supported.\n");
		return 0;
	}

	while (cmd_entry->cmd_name != NULL)
	{
		if (strcmp(cmd_entry->cmd_name, cmd) != 0)
		{
			cmd_entry++;
			continue;
		}
		help_str = cmd_entry->help;
		break;
	}

	if (help_str != NULL)
	{
		printk("%s: %s\n", cmd_entry->cmd_name, help_str);
	}

	return 0;
}

#ifdef CONFIG_BT_MESH_PROV
static void cmd_pb2(bt_mesh_prov_bearer_t bearer, const char *s)
{
	int err;

	if (str2bool(s))
	{
		err = bt_mesh_prov_enable(bearer);
		if (err)
		{
			printk("Failed to enable %s (err %d)\n",
				   bearer2str(bearer), err);
		}
		else
		{
			printk("%s enabled\n", bearer2str(bearer));
		}
	}
	else
	{
		err = bt_mesh_prov_disable(bearer);
		if (err)
		{
			printk("Failed to disable %s (err %d)\n",
				   bearer2str(bearer), err);
		}
		else
		{
			printk("%s disabled\n", bearer2str(bearer));
		}
	}
}

static int cmd_bunch_pb_adv(int argc, char *argv[])
{
	cmd_uuid(argc, argv);
	prov_bear = BT_MESH_PROV_ADV;
	cmd_init(0, NULL);
	return 0;
}

static int cmd_bunch_pb_gatt(int argc, char *argv[])
{
	cmd_uuid(argc, argv);
	prov_bear = BT_MESH_PROV_GATT;
	cmd_init(0, NULL);
	return 0;
}
#endif

static const struct mesh_shell_cmd mesh_commands[] = {
#if 0
	{"init", cmd_init, NULL},
	{"timeout", cmd_timeout, "[timeout in seconds]"},
#if defined(CONFIG_BT_MESH_PB_ADV)
	{"pb-adv", cmd_pb_adv, "<val: off, on>"},
#endif
#if defined(CONFIG_BT_MESH_PB_GATT)
	{"pb-gatt", cmd_pb_gatt, "<val: off, on>"},
#endif
	{"reset", cmd_reset, NULL},
	{"uuid", cmd_uuid, "<UUID: 1-16 hex values>"},
	{"input-num", cmd_input_num, "<number>"},
	{"input-str", cmd_input_str, "<string>"},
	{"static-oob", cmd_static_oob, "[val: 1-16 hex values]"},

	{"provision", cmd_provision, "<NetKeyIndex> <addr> [IVIndex]"},
#if defined(CONFIG_BT_MESH_LOW_POWER)
	{"lpn", cmd_lpn, "<value: off, on>"},
	{"poll", cmd_poll, NULL},
	{"add-sub", cmd_lpn_add_sub, NULL},
	{"del-sub", cmd_lpn_del_sub, NULL},
#endif
#if defined(CONFIG_BT_MESH_GATT_PROXY)
	{"ident", cmd_ident, NULL},
#endif
	{"dst", cmd_dst, "[destination address]"},
	{"netidx", cmd_netidx, "[NetIdx]"},
	{"appidx", cmd_appidx, "[AppIdx]"},

	/* Commands which access internal APIs, for testing only */
	{"net-send", cmd_net_send, "<hex string>"},
#if defined(CONFIG_BT_MESH_IV_UPDATE_TEST)
	{"iv-update", cmd_iv_update, NULL},
	{"iv-update-test", cmd_iv_update_test, "<value: off, on>"},
#endif
	{"rpl-clear", cmd_rpl_clear, NULL},
#endif

#ifdef CONFIG_BT_MESH_CFG_CLI
	/* Configuration Client Model operations */
#if 0
	{"get-comp", cmd_get_comp, "[page]"},
	{"beacon", cmd_beacon, "[val: off, on]"},
	{"ttl", cmd_ttl, "[ttl: 0x00, 0x02-0x7f]"},
	{"friend", cmd_friend, "[val: off, on]"},
	{"gatt-proxy", cmd_gatt_proxy, "[val: off, on]"},
	{"relay", cmd_relay, "[val: off, on] [count: 0-7] [interval: 0-32]"},
	{"net-key-add", cmd_net_key_add, "<NetKeyIndex> [val]"},
	{"net-key-get", cmd_net_key_get, "NULL"},
	{"net-key-update", cmd_net_key_update, "<NetKeyIndex> [val]"},
	{"net-key-del", cmd_net_key_del, "<NetKeyIndex> "},
	{"app-key-add", cmd_app_key_add, "<NetKeyIndex> <AppKeyIndex> [val]"},
	{"app-key-del", cmd_app_key_del, "<NetKeyIndex> <AppKeyIndex>"},
	{"app-key-get", cmd_app_key_get, "<NetKeyIndex>"},
	{"app-key-update", cmd_app_key_update, "<NetKeyIndex> <AppKeyIndex> [val]"},
	{"mod-app-bind", cmd_mod_app_bind,
	 "<addr> <AppIndex> <Model ID> [Company ID]"},
	{"mod-app-unbind", cmd_mod_app_unbind,
	 "<addr> <AppIndex> <Model ID> [Company ID]"},
	{"mod-app-get", cmd_mod_app_get, "<addr> <Model ID> [Company ID]"},
	{"mod-pub", cmd_mod_pub, "<addr> <mod id> [cid] [< [PubAddr] [uuid]> "
							 "<AppKeyIndex> <cred> <ttl> <period> <count> <interval>]"},
	{"mod-sub-add", cmd_mod_sub_add,
	 "<elem addr> <sub addr> <Model ID> [Company ID]"},
	{"mod-sub-del", cmd_mod_sub_del,
	 "<elem addr> <sub addr> <Model ID> [Company ID]"},
	{"mod-sub-del-all", cmd_mod_sub_del_all,
	 "<elem addr>  <Model ID> [Company ID]"},
	{"mod-sub-overwrite", cmd_mod_sub_overwrite,
	 "<elem addr> <sub addr> <Model ID> [Company ID]"},
	{"mod-sub-get", cmd_mod_sub_get,
	 "<elem addr> <Model ID> [Company ID]"},
	{"mod-sub-add-va", cmd_mod_sub_add_va,
	 "<elem addr> <Label UUID> <Model ID> [Company ID]"},
	{"mod-sub-del-va", cmd_mod_sub_del_va,
	 "<elem addr> <Label UUID> <Model ID> [Company ID]"},
	{"mod-sub-overwrite-va", cmd_mod_sub_overwrite_va,
	 "<elem addr> <Label UUID> <Model ID> [Company ID]"},
	{"hb-sub", cmd_hb_sub, "[<src> <dst> <period>]"},
	{"hb-pub", cmd_hb_pub,
	 "[<dst> <count> <period> <ttl> <features> <NetKeyIndex>]"},
#endif
/*[Genie begin] add by wenbing.cwb at 2021-01-21*/
#ifdef CONFIG_BT_MESH_CTRL_RELAY
	{"ctrl-relay", cmd_ctrl_relay,
	 "[<enable> <N> <rssi> <status period> <check period> <request peroid>]"},
#endif
/*[Genie end] add by wenbing.cwb at 2021-01-21*/
#if 0
	{"node-ident", cmd_node_ident, "<netkey-idx>, [identify state]"},
	{"node-reset", cmd_node_reset, "NULL"},
	{"net-transmit", cmd_network_transmit, "<transmit count>, <transmit interval steps>"},
	{"lpn-timeout-get", cmd_lpn_timeout_get, "<lpn_addr>"},
	{"key-refresh", cmd_key_refresh, "[KRP Transition]"},
	{"beacon-kr", cmd_beacon_kr, "<state>"},
#endif
#endif

#if 0
#ifdef CONFIG_BT_MESH_HEALTH_CLI
	/* Health Client Model Operations */
	{"fault-get", cmd_fault_get, "<Company ID>"},
	{"fault-clear", cmd_fault_clear, "<Company ID>"},
	{"fault-clear-unack", cmd_fault_clear_unack, "<Company ID>"},
	{"fault-test", cmd_fault_test, "<Company ID> <Test ID>"},
	{"fault-test-unack", cmd_fault_test_unack, "<Company ID> <Test ID>"},
	{"period-get", cmd_period_get, NULL},
	{"period-set", cmd_period_set, "<divisor>"},
	{"period-set-unack", cmd_period_set_unack, "<divisor>"},
	{"attention-get", cmd_attention_get, NULL},
	{"attention-set", cmd_attention_set, "<timer>"},
	{"attention-set-unack", cmd_attention_set_unack, "<timer>"},
#endif

#ifdef CONFIG_BT_MESH_HEALTH_SRV
	/* Health Server Model Operations */
	{"add-fault", cmd_add_fault, "<Fault ID>"},
	{"del-fault", cmd_del_fault, "[Fault ID]"},
#endif
	{"help", cmd_display_help, "[help]"},

#ifdef CONFIG_BT_MESH_PROV
	{"hk0", cmd_bunch_pb_adv, "<UUID: 1-16 hex values>"},
	{"hk1", cmd_bunch_pb_gatt, "<UUID: 1-16 hex values>"},
#endif

	{"net-pressure-test", cmd_net_pressure_test, "<dst> <window(s)> <pkt-per-window> <test duration(s)> [cnt]"},
#endif

	{NULL, NULL, NULL}};

struct mesh_shell_cmd *bt_mesh_get_shell_cmd_list()
{
	return mesh_commands;
}

#endif // CONFIG_BT_MESH_SHELL
