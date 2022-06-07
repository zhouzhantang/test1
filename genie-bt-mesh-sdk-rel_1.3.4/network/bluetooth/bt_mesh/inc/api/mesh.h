/** @file
 *  @brief Bluetooth Mesh Profile APIs.
 */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __BT_MESH_H
#define __BT_MESH_H

#include <stddef.h>
#include <net/buf.h>

#include <mesh/access.h>
#include <mesh/main.h>
#include <mesh/cfg_srv.h>
#include <mesh/health_srv.h>

/*[Genie begin] add by wenbing.cwb at 2021-01-21*/
#ifdef CONFIG_BT_MESH_CTRL_RELAY
#include <mesh/ctrl_relay.h>
#endif
/*[Genie end] add by wenbing.cwb at 2021-01-21*/

#if defined(CONFIG_BT_MESH_CFG_CLI)
#include <mesh/cfg_cli.h>
#endif

#if defined(CONFIG_BT_MESH_HEALTH_CLI)
#include <mesh/health_cli.h>
#endif

#if defined(CONFIG_BT_MESH_GATT_PROXY)
#include <mesh/proxy.h>
#endif

#if defined(CONFIG_BT_MESH_SHELL)
#include <mesh/mesh_shell.h>
#endif

#include <sal/inc/genie_sal_ble_mesh.h>
#include <genie_mesh_api.h>

#endif /* __BT_MESH_H */
