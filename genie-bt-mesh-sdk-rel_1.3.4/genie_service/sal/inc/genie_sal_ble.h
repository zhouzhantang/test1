#ifndef __GENIE_SAL_BLE_H__
#define __GENIE_SAL_BLE_H__

#include <genie_sal_ble_common.h>
#include <genie_sal_ble_host.h>
#include <genie_sal_ble_mesh.h>

#define GENIE_HAL_BLE_SEND_MAX_DATA_LEN (92)
#define GENIE_HAL_BLE_SCAN_TIMEOUT (5000)

extern uint8_t g_mesh_log_mode;

typedef void (*genie_sal_ble_get_rssi_cb)(unsigned char mac[], char rssi);
int genie_sal_ble_get_rssi(uint8_t mac[], genie_sal_ble_get_rssi_cb get_rssi_callback);
int genie_sal_ble_send_msg(uint8_t element_id, uint8_t *p_data, uint8_t len);
int genie_sal_ble_set_factory_flag(void);
int genie_sal_ble_init(void);

#endif
