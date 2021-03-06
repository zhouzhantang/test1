/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#ifndef AOS_KV_H
#define AOS_KV_H

#include <aos/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Add a new KV pair.
 *
 * @param[in]  key    the key of the KV pair.
 * @param[in]  value  the value of the KV pair.
 * @param[in]  len    the length of the value.
 * @param[in]  sync   save the KV pair to flash right now (should always be 1).
 *
 * @return  0 on success, negative error on failure.
 */
int aos_kv_set(const char *key, const void *value, int len, int sync);

/**
 * Get the KV pair's value stored in buffer by its key.
 *
 * @note: the buffer_len should be larger than the real length of the value,
 *        otherwise buffer would be NULL.
 *
 * @param[in]      key         the key of the KV pair to get.
 * @param[out]     buffer      the memory to store the value.
 * @param[in-out]  buffer_len  in: the length of the input buffer.
 *                             out: the real length of the value.
 *
 * @return  0 on success, negative error on failure.
 */
int aos_kv_get(const char *key, void *buffer, int *buffer_len);

/**
 * Get the KV pair's value length stored.
 *
 * @param[in]      key         the key of the KV pair to get.
 *
 * @return  0 on no exist, > 0 on the real length of the value, negative error on failure.
 */
int aos_kv_get_vlen(const char *key);

/**
 * Delete the KV pair by its key.
 *
 * @param[in]  key  the key of the KV pair to delete.
 *
 * @return  0 on success, negative error on failure.
 */
int aos_kv_del(const char *key);

/**
 * Delete the all KV data.
 *
 * @param[in]  ex_prefix except the prefix KV no delete, other KV data delete.
 *
 * @return  0 on success, negative error on failure.
 */
int aos_kv_del_all(const char *ex_prefix);

/**
 * Delete the KV pair by its prefix.
 *
 * @param[in]  prefix  the prefix of the kv pair which is need to delete.
 *
 * @return  0 on success, negative error on failure.
 */
int aos_kv_del_by_prefix(const char *prefix);


#ifdef __cplusplus
}
#endif

#endif /* AOS_KV_H */
