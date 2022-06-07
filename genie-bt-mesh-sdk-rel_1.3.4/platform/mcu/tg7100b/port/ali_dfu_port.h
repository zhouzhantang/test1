#ifndef _ALI_DFU_PORT_H
#define _ALI_DFU_PORT_H
#include <stdbool.h>

void unlock_flash_all();
void dfu_reboot(void);
unsigned char dfu_check_checksum(short image_id, unsigned short *crc16_output);
int ali_dfu_image_update(short signature, int offset, int length, int *p_void);
void lock_flash(void);

#ifdef CONFIG_GENIE_OTA_PINGPONG
/**
 * @brief get the current runing partition.
 * @return the current runing partition.
 */
uint8_t genie_sal_ota_get_current_image_id(void);

/**
 * @brief switch the running partition, without reboot.
 * @param[in] the partition which switch to.
 * @return the runing partition when next boot.
 */
uint8_t genie_sal_ota_change_image_id(uint8_t target_id);
#endif

int erase_dfu_flash(void);

#endif
