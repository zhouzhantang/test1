/** @file
 *  @brief Bluetooth Mesh Configuration Client Model APIs.
 */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __BT_MESH_CFG_CLI_H
#define __BT_MESH_CFG_CLI_H

/**
 * @brief Bluetooth Mesh
 * @defgroup bt_mesh_cfg_cli Bluetooth Mesh Configuration Client Model
 * @ingroup bt_mesh
 * @{
 */

/** Mesh Configuration Client Model Context */
struct bt_mesh_cfg_cli
{
	struct bt_mesh_model *model;

	struct k_sem op_sync;
	u32_t op_pending;
	void *op_param;
};

extern const struct bt_mesh_model_op bt_mesh_cfg_cli_op[];

/* Helper macro to define the Configuration Client Model. */
#define BT_MESH_MODEL_CFG_CLI(cli_data)     \
	BT_MESH_MODEL(BT_MESH_MODEL_ID_CFG_CLI, \
				  bt_mesh_cfg_cli_op, NULL, cli_data)

int bt_mesh_cfg_comp_data_get(u16_t net_idx, u16_t addr, u8_t page,
							  u8_t *status, struct net_buf_simple *comp);

int bt_mesh_cfg_beacon_get(u16_t net_idx, u16_t addr, u8_t *status);

int bt_mesh_cfg_beacon_set(u16_t net_idx, u16_t addr, u8_t val, u8_t *status);

int bt_mesh_cfg_ttl_get(u16_t net_idx, u16_t addr, u8_t *ttl);

int bt_mesh_cfg_ttl_set(u16_t net_idx, u16_t addr, u8_t val, u8_t *ttl);

int bt_mesh_cfg_friend_get(u16_t net_idx, u16_t addr, u8_t *status);

int bt_mesh_cfg_friend_set(u16_t net_idx, u16_t addr, u8_t val, u8_t *status);

int bt_mesh_cfg_gatt_proxy_get(u16_t net_idx, u16_t addr, u8_t *status);

int bt_mesh_cfg_gatt_proxy_set(u16_t net_idx, u16_t addr, u8_t val,
							   u8_t *status);

int bt_mesh_cfg_relay_get(u16_t net_idx, u16_t addr, u8_t *status,
						  u8_t *transmit);

int bt_mesh_cfg_relay_set(u16_t net_idx, u16_t addr, u8_t new_relay,
						  u8_t new_transmit, u8_t *status, u8_t *transmit);

int bt_mesh_cfg_net_key_add(u16_t net_idx, u16_t addr, u16_t key_net_idx,
							const u8_t net_key[16], u8_t *status);

int bt_mesh_cfg_net_key_del(u16_t net_idx, u16_t addr, u16_t key_net_idx, u8_t *status);

int bt_mesh_cfg_net_key_get(u16_t net_idx, u16_t addr, u8_t *status);

int bt_mesh_cfg_net_key_update(u16_t net_idx, u16_t addr, u16_t key_net_idx,
							   const u8_t net_key[16], u8_t *status);

int bt_mesh_cfg_app_key_add(u16_t net_idx, u16_t addr, u16_t key_net_idx,
							u16_t key_app_idx, const u8_t app_key[16], u8_t *status);

int bt_mesh_cfg_app_key_del(u16_t net_idx, u16_t addr, u16_t key_net_idx,
							u16_t key_app_idx, u8_t *status);

int bt_mesh_cfg_app_key_update(u16_t net_idx, u16_t addr, u16_t key_net_idx,
							   u16_t key_app_idx, const u8_t app_key[16], u8_t *status);

int bt_mesh_cfg_mod_app_bind(u16_t net_idx, u16_t addr, u16_t elem_addr,
							 u16_t mod_app_idx, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_app_bind_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								 u16_t mod_app_idx, u16_t mod_id, u16_t cid,
								 u8_t *status);

int bt_mesh_cfg_mod_app_unbind(u16_t net_idx, u16_t addr, u16_t elem_addr,
							   u16_t mod_app_idx, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_app_unbind_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								   u16_t mod_app_idx, u16_t mod_id, u16_t cid,
								   u8_t *status);

int bt_mesh_cfg_mod_app_get(u16_t net_idx, u16_t addr, u16_t elem_addr,
							u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_app_get_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t mod_id, u16_t cid, u8_t *status);

int bt_mesh_cfg_node_ident_get(u16_t net_idx, u16_t addr, u16_t elem_addr, u8_t *status);

int bt_mesh_cfg_node_ident_set(u16_t net_idx, u16_t addr, u16_t elem_addr, u8_t ident_state, u8_t *status);

int bt_mesh_cfg_node_reset(u16_t net_idx, u16_t addr, u8_t *status);

int bt_mesh_cfg_network_transmit_get(u16_t net_idx, u16_t addr, u8_t *status);

int bt_mesh_cfg_network_transmit_set(u16_t net_idx, u16_t addr, u8_t transmit, u8_t *status);

int bt_mesh_cfg_lpn_timeout_get(u16_t net_idx, u16_t addr, u16_t lpn_addr, u8_t *status);

struct bt_mesh_cfg_mod_pub
{
	u16_t addr;
	u8_t *uuid;
	u16_t app_idx;
	bool cred_flag;
	u8_t ttl;
	u8_t period;
	u8_t transmit;
};

int bt_mesh_cfg_mod_pub_get(u16_t net_idx, u16_t addr, u16_t elem_addr,
							u16_t mod_id, struct bt_mesh_cfg_mod_pub *pub,
							u8_t *status);

int bt_mesh_cfg_mod_pub_get_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t mod_id, u16_t cid,
								struct bt_mesh_cfg_mod_pub *pub, u8_t *status);

int bt_mesh_cfg_mod_pub_set(u16_t net_idx, u16_t addr, u16_t elem_addr,
							u16_t mod_id, struct bt_mesh_cfg_mod_pub *pub,
							u8_t *status);

int bt_mesh_cfg_mod_pub_set_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t mod_id, u16_t cid,
								struct bt_mesh_cfg_mod_pub *pub, u8_t *status);

int bt_mesh_cfg_mod_sub_add(u16_t net_idx, u16_t addr, u16_t elem_addr,
							u16_t sub_addr, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_sub_add_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t sub_addr, u16_t mod_id, u16_t cid,
								u8_t *status);

int bt_mesh_cfg_mod_sub_del(u16_t net_idx, u16_t addr, u16_t elem_addr,
							u16_t sub_addr, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_sub_del_all(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t sub_addr, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_sub_del_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t sub_addr, u16_t mod_id, u16_t cid,
								u8_t *status);

int bt_mesh_cfg_mod_sub_del_all_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
									u16_t sub_addr, u16_t mod_id, u16_t cid,
									u8_t *status);

int bt_mesh_cfg_mod_sub_overwrite(u16_t net_idx, u16_t addr, u16_t elem_addr,
								  u16_t sub_addr, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_sub_overwrite_vnd(u16_t net_idx, u16_t addr,
									  u16_t elem_addr, u16_t sub_addr,
									  u16_t mod_id, u16_t cid, u8_t *status);

int bt_mesh_cfg_mod_sub_get(u16_t net_idx, u16_t addr, u16_t elem_addr,
							u16_t sub_addr, u16_t mod_id, u8_t *status);

int bt_mesh_cfg_mod_sub_get_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								u16_t sub_addr, u16_t mod_id, u16_t cid, u8_t *status);

int bt_mesh_cfg_mod_sub_va_add(u16_t net_idx, u16_t addr, u16_t elem_addr,
							   const u8_t label[16], u16_t mod_id,
							   u16_t *virt_addr, u8_t *status);

int bt_mesh_cfg_mod_sub_va_add_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								   const u8_t label[16], u16_t mod_id,
								   u16_t cid, u16_t *virt_addr, u8_t *status);

int bt_mesh_cfg_mod_sub_va_del(u16_t net_idx, u16_t addr, u16_t elem_addr,
							   const u8_t label[16], u16_t mod_id,
							   u16_t *virt_addr, u8_t *status);

int bt_mesh_cfg_mod_sub_va_del_vnd(u16_t net_idx, u16_t addr, u16_t elem_addr,
								   const u8_t label[16], u16_t mod_id,
								   u16_t cid, u16_t *virt_addr, u8_t *status);

int bt_mesh_cfg_mod_sub_va_overwrite(u16_t net_idx, u16_t addr,
									 u16_t elem_addr, const u8_t label[16],
									 u16_t mod_id, u16_t *virt_addr,
									 u8_t *status);

int bt_mesh_cfg_mod_sub_va_overwrite_vnd(u16_t net_idx, u16_t addr,
										 u16_t elem_addr, const u8_t label[16],
										 u16_t mod_id, u16_t cid,
										 u16_t *virt_addr, u8_t *status);

int bt_mesh_cfg_krp_get(u16_t net_idx, u16_t addr, u16_t key_net_idx, u8_t *status);

int bt_mesh_cfg_krp_set(u16_t net_idx, u16_t addr, u16_t key_net_idx, u8_t KRP_state, u8_t *status);

int bt_mesh_cfg_app_key_get(u16_t net_idx, u16_t addr, u16_t key_net_idx, u8_t *status);

struct bt_mesh_cfg_hb_sub
{
	u16_t src;
	u16_t dst;
	u8_t period;
	u8_t count;
	u8_t min;
	u8_t max;
};

int bt_mesh_cfg_hb_sub_set(u16_t net_idx, u16_t addr,
						   struct bt_mesh_cfg_hb_sub *sub, u8_t *status);

int bt_mesh_cfg_hb_sub_get(u16_t net_idx, u16_t addr,
						   struct bt_mesh_cfg_hb_sub *sub, u8_t *status);

struct bt_mesh_cfg_hb_pub
{
	u16_t dst;
	u8_t count;
	u8_t period;
	u8_t ttl;
	u16_t feat;
	u16_t net_idx;
};

int bt_mesh_cfg_hb_pub_set(u16_t net_idx, u16_t addr,
						   const struct bt_mesh_cfg_hb_pub *pub, u8_t *status);

int bt_mesh_cfg_hb_pub_get(u16_t net_idx, u16_t addr,
						   struct bt_mesh_cfg_hb_pub *pub, u8_t *status);

/*[Genie begin] add by wenbing.cwb at 2021-01-21*/
#ifdef CONFIG_BT_MESH_CTRL_RELAY
int bt_mesh_cfg_ctrl_relay_get(u16_t net_idx, u16_t addr,
							   struct ctrl_relay_param *cr, u8_t *status);

int bt_mesh_cfg_ctrl_relay_set(u16_t net_idx, u16_t addr,
							   const struct ctrl_relay_param *cr, u8_t *status);
#endif
/*[Genie end] add by wenbing.cwb at 2021-01-21*/

s32_t bt_mesh_cfg_cli_timeout_get(void);
void bt_mesh_cfg_cli_timeout_set(s32_t timeout);

#endif /* __BT_MESH_CFG_CLI_H */
