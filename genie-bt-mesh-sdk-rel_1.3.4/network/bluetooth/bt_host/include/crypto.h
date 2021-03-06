/** @file
 *  @brief Bluetooth subsystem crypto APIs.
 */

/*
 * Copyright (c) 2017 Nordic Semiconductor ASA
 * Copyright (c) 2015-2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __BT_CRYPTO_H
#define __BT_CRYPTO_H

/**
 * @brief Cryptography
 * @defgroup bt_crypto Cryptography
 * @ingroup bluetooth
 * @{
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bt_cmac_t {
  uint8_t iv[16];
  uint8_t K1[16];
  uint8_t K2[16];
  uint8_t leftover[16];
  unsigned int keyid;
  unsigned int leftover_offset;
  uint8_t aes_key[16];
  uint64_t countdown;
};
/** @brief Generate random data.
 *
 *  A random number generation helper which utilizes the Bluetooth
 *  controller's own RNG.
 *
 *  @param buf Buffer to insert the random data
 *  @param len Length of random data to generate
 *
 *  @return Zero on success or error code otherwise, positive in case
 *  of protocol error or negative (POSIX) in case of stack internal error
 */
int bt_rand(void *buf, size_t len);

/** @brief AES encrypt little-endian data.
 *
 *  An AES encrypt helper is used to request the Bluetooth controller's own
 *  hardware to encrypt the plaintext using the key and returns the encrypted
 *  data.
 *
 *  @param key 128 bit LS byte first key for the encryption of the plaintext
 *  @param plaintext 128 bit LS byte first plaintext data block to be encrypted
 *  @param enc_data 128 bit LS byte first encrypted data block
 *
 *  @return Zero on success or error code otherwise.
 */
int bt_encrypt_le(const u8_t key[16], const u8_t plaintext[16],
		  u8_t enc_data[16]);

/** @brief AES encrypt big-endian data.
 *
 *  An AES encrypt helper is used to request the Bluetooth controller's own
 *  hardware to encrypt the plaintext using the key and returns the encrypted
 *  data.
 *
 *  @param key 128 bit MS byte first key for the encryption of the plaintext
 *  @param plaintext 128 bit MS byte first plaintext data block to be encrypted
 *  @param enc_data 128 bit MS byte first encrypted data block
 *
 *  @return Zero on success or error code otherwise.
 */
int bt_encrypt_be(const u8_t key[16], const u8_t plaintext[16],
		  u8_t enc_data[16]);

int bt_decrypt_be(const u8_t key[16], const u8_t enc_data[16], u8_t dec_data[16]);

int prng_init(void);

#ifdef __cplusplus
}
#endif
/**
 * @}
 */

#endif /* __BT_CRYPTO_H */
