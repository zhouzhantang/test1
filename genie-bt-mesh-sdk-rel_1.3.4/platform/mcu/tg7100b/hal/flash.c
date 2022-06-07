/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "soc.h"
#include "drv_spiflash.h"
#include "spif.h"
#include "pin.h"
#include "hal/soc/flash.h"
#include "aos/kernel.h"

extern const hal_logic_partition_t hal_partitions[];
static spiflash_handle_t g_spiflash_handle;
static aos_mutex_t g_spiflash_mutex;
#define ROUND_DOWN(a, b) (((a) / (b)) * (b))
#define FLASH_ALIGN_MASK ~(sizeof(uint32_t) - 1)
#define FLASH_ALIGN sizeof(uint32_t)

#define MAC_PARAMS_OFFSET (0)
#define MAC_PARAMS_SIZE (0x10)

#define RSV_PARAMS_OFFSET (MAC_PARAMS_OFFSET + MAC_PARAMS_SIZE)
#define RSV_PARAMS_SIZE (0x10)

#define FREQ_PARAMS_OFFSET (RSV_PARAMS_OFFSET + RSV_PARAMS_SIZE)
#define FREQ_PARAMS_SIZE (0x8)

#define TRIPLE_OFFSET (FREQ_PARAMS_OFFSET + FREQ_PARAMS_SIZE)
#define TRIPLE_SIZE (32)

#define GROUP_ADDR_OFFSET (TRIPLE_OFFSET + TRIPLE_SIZE)
#define GROUP_ADDR_SIZE (18) //CONFIG_BT_MESH_MODEL_GROUP_COUNT * 2 + 2

#define SN_PARAMS_OFFSET (GROUP_ADDR_OFFSET + GROUP_ADDR_SIZE)
#define SN_PARAMS_SIZE (32)

#define OTP_TOTAL_DATA_SIZE (MAC_PARAMS_SIZE + RSV_PARAMS_SIZE + FREQ_PARAMS_SIZE + TRIPLE_SIZE + GROUP_ADDR_SIZE + SN_PARAMS_SIZE)

hal_logic_partition_t *hal_flash_get_info(hal_partition_t in_partition)
{
    hal_logic_partition_t *logic_partition;

    logic_partition = (hal_logic_partition_t *)&hal_partitions[in_partition];

    if (logic_partition == NULL || logic_partition->partition_description == NULL)
    {
        printf("pno %d err!\n", in_partition);
        return NULL;
    }

    if (g_spiflash_handle == NULL)
    {
        g_spiflash_handle = csi_spiflash_initialize(0, NULL);
        if (g_spiflash_handle == NULL)
        {
            return NULL;
        }
        aos_mutex_new(&g_spiflash_mutex);
    }
    return logic_partition;
}

int32_t hal_flash_erase(hal_partition_t in_partition, uint32_t off_set, uint32_t size)
{
    int ret = -1;
    hal_logic_partition_t *node = hal_flash_get_info(in_partition);
    if (g_spiflash_handle == NULL || node == NULL ||
        off_set + size > node->partition_length)
    {
        return -1;
    }
    if (g_spiflash_handle != NULL && node != NULL && (off_set % SPIF_SECTOR_SIZE) == 0)
    {
        int blkcnt = 1, tmp_size;
        tmp_size = size % SPIF_SECTOR_SIZE;
        if (tmp_size == 0)
        {
            blkcnt = size / SPIF_SECTOR_SIZE;
        }
        else
        {
            blkcnt += (size / SPIF_SECTOR_SIZE);
        }

        for (int i = 0; i < blkcnt; i++)
        {
            ret = csi_spiflash_erase_sector(g_spiflash_handle, node->partition_start_addr + off_set + i * SPIF_SECTOR_SIZE);
            if (ret < 0)
            {
                printf("erase addr:%x\n", (unsigned int)off_set);
                return -1;
            }
        }
    }

    return 0;
}

int32_t hal_flash_write(hal_partition_t in_partition, uint32_t *off_set, const void *in_buf, uint32_t in_buf_len)
{
    int ret = -1;
    uint32_t start_addr;
    uint32_t write_addr;
    uint8_t *pwrite_buf = (uint8_t *)in_buf;
    uint32_t write_len = in_buf_len;
    //uint8_t tmp_write[FLASH_ALIGN] = {0};

    hal_logic_partition_t *node = hal_flash_get_info(in_partition);

    if (g_spiflash_handle == NULL || node == NULL ||
        off_set == NULL || in_buf == NULL ||
        *off_set + in_buf_len > node->partition_length)
    {
        return -1;
    }

    aos_mutex_lock(&g_spiflash_mutex, AOS_WAIT_FOREVER);
    start_addr = node->partition_start_addr + *off_set;
    write_addr = start_addr;

    ret = csi_spiflash_program(g_spiflash_handle, write_addr, pwrite_buf, write_len);

    if (ret != write_len)
    {
        return -1;
    }

    *off_set += in_buf_len;
    aos_mutex_unlock(&g_spiflash_mutex);
    return 0;
}

int32_t hal_flash_read(hal_partition_t in_partition, uint32_t *off_set, void *out_buf, uint32_t out_buf_len)
{
    int ret = 0;
    uint32_t start_addr;
    uint32_t read_addr; //= out_buf
    uint32_t read_len = out_buf_len;
    //uint8_t tmp_read[FLASH_ALIGN] = {0};
    uint8_t *pread_buf = out_buf;
    hal_logic_partition_t *node = hal_flash_get_info(in_partition);

    if (g_spiflash_handle == NULL || node == NULL ||
        off_set == NULL || out_buf == NULL ||
        *off_set + out_buf_len > node->partition_length)
    {
        return -1;
    }

    aos_mutex_lock(&g_spiflash_mutex, AOS_WAIT_FOREVER);
    start_addr = node->partition_start_addr + *off_set;

    read_addr = start_addr;

    ret = csi_spiflash_read(g_spiflash_handle, read_addr, pread_buf, read_len);

    if (ret != out_buf_len)
    {
        return -1;
    }

    (void)ret;
    *off_set += out_buf_len;
    aos_mutex_unlock(&g_spiflash_mutex);
    return 0;
}

/**
 * Set security options on a logical partition
 *
 * @param[in]  partition  The target flash logical partition
 * @param[in]  offset     Point to the start address that the data is read, and
 *                        point to the last unread address after this function is
 *                        returned, so you can call this function serval times without
 *                        update this start address.
 * @param[in]  size       Size of enabled flash area
 *
 * @return  0 : On success, EIO : If an error occurred with any step
 */
int32_t hal_flash_enable_secure(hal_partition_t partition, uint32_t off_set, uint32_t size)
{
    return -1;
}

/**
 * Disable security options on a logical partition
 *
 * @param[in]  partition  The target flash logical partition
 * @param[in]  offset     Point to the start address that the data is read, and
 *                        point to the last unread address after this function is
 *                        returned, so you can call this function serval times without
 *                        update this start address.
 * @param[in]  size       Size of disabled flash area
 *
 * @return  0 : On success, EIO : If an error occurred with any step
 */
int32_t hal_flash_dis_secure(hal_partition_t partition, uint32_t off_set, uint32_t size)
{
    return -1;
}

int32_t hal_flash_read_otp(uint32_t off_set, void *out_buf, uint32_t out_buf_len)
{
    uint32_t offset = off_set;

    return hal_flash_read(HAL_PARTITION_OTP, &offset, out_buf, out_buf_len);
}

static int32_t hal_flash_erase_otp(uint32_t size)
{
    return hal_flash_erase(HAL_PARTITION_OTP, 0, size);
}

int32_t hal_flash_write_otp(uint32_t off_set, const void *in_buf, uint32_t in_buf_len)
{
    int32_t retval = 0;
    uint32_t offset = 0;
    uint8_t *p_backup_data = NULL;

    if (off_set + in_buf_len > OTP_TOTAL_DATA_SIZE)
    {
        printf("param err\n");
        return -1;
    }

    p_backup_data = aos_malloc(OTP_TOTAL_DATA_SIZE);
    if (NULL == p_backup_data)
    {
        printf("no mem\n");
        return -1;
    }

    retval = hal_flash_read_otp(0, p_backup_data, OTP_TOTAL_DATA_SIZE);
    if (retval < 0)
    {
        aos_free(p_backup_data);
        return retval;
    }

    hal_flash_erase_otp(OTP_TOTAL_DATA_SIZE);
    memcpy(p_backup_data + off_set, in_buf, in_buf_len);

    retval = hal_flash_write(HAL_PARTITION_OTP, &offset, p_backup_data, OTP_TOTAL_DATA_SIZE);

    aos_free(p_backup_data);
    return retval;
}

int32_t hal_flash_read_freq_params(void *out_buf, uint32_t out_buf_len)
{
    return hal_flash_read_otp(FREQ_PARAMS_OFFSET, out_buf, out_buf_len);
}

int32_t hal_flash_write_freq_params(const void *in_buf, uint32_t in_buf_len)
{
    return hal_flash_write_otp(FREQ_PARAMS_OFFSET, in_buf, in_buf_len);
}

int32_t hal_flash_read_sn_params(void *out_buf, uint32_t out_buf_len)
{
    return hal_flash_read_otp(SN_PARAMS_OFFSET, out_buf, out_buf_len);
}

int32_t hal_flash_write_sn_params(const void *in_buf, uint32_t in_buf_len)
{
    return hal_flash_write_otp(SN_PARAMS_OFFSET, in_buf, in_buf_len);
}

int32_t hal_flash_read_mac_params(void *out_buf, uint32_t out_buf_len)
{
    int32_t ret = 0;
    uint8_t read_mac[6];
    uint8_t *p_temp = (uint8_t *)out_buf;

    if (!p_temp || out_buf_len < 6)
        return -1;

    ret = hal_flash_read_otp(MAC_PARAMS_OFFSET, (void *)read_mac, out_buf_len);
    if (ret == 0)
    {
        //Map for match TG7100B Driver
        p_temp[0] = read_mac[4];
        p_temp[1] = read_mac[5];
        p_temp[2] = read_mac[0];
        p_temp[3] = read_mac[1];
        p_temp[4] = read_mac[2];
        p_temp[5] = read_mac[3];
    }

    return ret;
}

int32_t hal_flash_write_mac_params(const void *in_buf, uint32_t in_buf_len)
{
    uint8_t *p_mac = (uint8_t *)in_buf;
    uint8_t write_mac[6];
    if (!p_mac || in_buf_len != 6)
        return -1;

    //Map for match TG7100B Driver
    write_mac[0] = p_mac[2];
    write_mac[1] = p_mac[3];
    write_mac[2] = p_mac[4];
    write_mac[3] = p_mac[5];
    write_mac[4] = p_mac[0];
    write_mac[5] = p_mac[1];

    return hal_flash_write_otp(MAC_PARAMS_OFFSET, write_mac, in_buf_len);
}

int32_t hal_flash_read_triples(void *out_buf, uint32_t out_buf_len)
{
    return hal_flash_read_otp(TRIPLE_OFFSET, out_buf, out_buf_len);
}

int32_t hal_flash_write_triples(const void *in_buf, uint32_t in_buf_len)
{
    return hal_flash_write_otp(TRIPLE_OFFSET, in_buf, in_buf_len);
}

int32_t hal_flash_read_group_addr(void *out_buf, uint32_t out_buf_len)
{
    return hal_flash_read_otp(GROUP_ADDR_OFFSET, out_buf, out_buf_len);
}

int32_t hal_flash_write_group_addr(const void *in_buf, uint32_t in_buf_len)
{
    return hal_flash_write_otp(GROUP_ADDR_OFFSET, in_buf, in_buf_len);
}

int32_t hal_flash_erase_write(hal_partition_t in_partition, uint32_t *off_set, const void *in_buf, uint32_t in_buf_len)
{
    int32_t ret = hal_flash_erase(in_partition, *off_set, in_buf_len);
    ret |= hal_flash_write(in_partition, off_set, in_buf, in_buf_len);
    return ret;
}

int32_t hal_flash_addr2offset(hal_partition_t *in_partition, uint32_t *off_set, uint32_t addr)
{ //partition_start_addr
    hal_logic_partition_t *partition_info;

    for (int i = 0; i < HAL_PARTITION_MAX; i++)
    {
        partition_info = hal_flash_get_info(i);
        if (partition_info == NULL)
            continue;

        if ((addr >= partition_info->partition_start_addr) && (addr < (partition_info->partition_start_addr + partition_info->partition_length)))
        {
            *in_partition = i;
            *off_set = addr - partition_info->partition_start_addr;
            return 0;
        }
    }
    *in_partition = -1;
    *off_set = 0;
    return -1;
}

void hal_flash_write_test()
{
    int ret = 0;
    uint32_t offset = 0;
    uint8_t data[32] = {0};
    uint8_t data_read[32] = {0};
    int i = 0;
    for (i = 0; i < sizeof(data); i++)
    {
        data[i] = i;
    }

    hal_flash_erase(HAL_PARTITION_PARAMETER_2, 0, 4096);

    offset = 0;

    //printf("write test 32 by 32 %d, offset %d size %d, data %p\n", HAL_PARTITION_PARAMETER_2, offset, 4096, data);
    for (i = 0; i < 4096 / 32; i++)
    {
        ret = hal_flash_write(HAL_PARTITION_PARAMETER_2, &offset, (uint8_t *)(data), 32);
        if (ret)
        {
            printf("write 1 fail %d\n", ret);
            break;
        }
    }

    offset = 0;
    memset(data_read, 0, 32);
    for (i = 0; i < 4096 / 32; i++)
    {
        memset(data_read, 0, 32);
        ret = hal_flash_read(HAL_PARTITION_PARAMETER_2, &offset, (uint8_t *)(data_read), 32);
        if (ret)
        {
            printf("read 1 fail %d\n", ret);
            break;
        }
        if (memcmp(data, data_read, 32))
        {
            printf("write test fail, data missmatch\n");
            break;
        }
    }

    hal_flash_erase(HAL_PARTITION_PARAMETER_2, 0, 4096);

    offset = 0;

    //printf("write test 1 by 1 %d, offset %d size %d, data %p\n", HAL_PARTITION_PARAMETER_2, offset, 4096, data);
    for (i = 0; i < 4096; i++)
    {
        ret = hal_flash_write(HAL_PARTITION_PARAMETER_2, &offset, (uint8_t *)(data + i % 32), 1);
        if (ret)
        {
            printf("write 2 fail %d\n", ret);
            break;
        }
    }

    offset = 0;
    memset(data_read, 0, 32);
    for (i = 0; i < 4096; i++)
    {
        ret = hal_flash_read(HAL_PARTITION_PARAMETER_2, &offset, (uint8_t *)(data_read + i % 32), 1);
        if (ret)
        {
            printf("read 2 fail %d\n", ret);
            break;
        }
        if ((i + 1) % 32 == 0)
        {
            if (memcmp(data, data_read, 32))
            {
                printf("write 2 1 by 1 test fail, data missmatch\n");
                break;
            }
            memset(data_read, 0, 32);
        }
    }

    hal_flash_erase(HAL_PARTITION_PARAMETER_2, 0, 4096);
}
