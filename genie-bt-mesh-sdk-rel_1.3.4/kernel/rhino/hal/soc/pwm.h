/*
 * Copyright (C) 2019-2020 Alibaba Group Holding Limited
 */

#ifndef HAL_PWM_H
#define HAL_PWM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
#if defined(BOARD_TC825X)
    float    duty_cycle;        /* the pwm duty_cycle */
#else
    uint32_t    duty_cycle;  /* the pwm duty_cycle */
#endif
    uint32_t freq;       /* the pwm freq */
} pwm_config_t;

typedef struct {
    uint8_t       port;   /* pwm port */
    pwm_config_t  config; /* spi config */
    void         *priv;   /* priv data */
} pwm_dev_t;

/**
 * Initialises a PWM pin
 *
 * @param[in]  pwm  the PWM device
 *
 * @return  0 : on success, EIO : if an error occurred with any step
 */
int32_t hal_pwm_init(pwm_dev_t *pwm);

/**
 * Starts Pulse-Width Modulation signal output on a PWM pin
 *
 * @param[in]  pwm  the PWM device
 *
 * @return  0 : on success, EIO : if an error occurred with any step
 */
int32_t hal_pwm_start(pwm_dev_t *pwm);

/**
 * Stops output on a PWM pin
 *
 * @param[in]  pwm  the PWM device
 *
 * @return  0 : on success, EIO : if an error occurred with any step
 */
int32_t hal_pwm_stop(pwm_dev_t *pwm);

/**
 * change the para of pwm
 *
 * @param[in]  pwm   the PWM device
 * @param[in]  para  the para of pwm
 *
 * @return  0 : on success, EIO : if an error occurred with any step
 */
int32_t hal_pwm_para_chg(pwm_dev_t *pwm, pwm_config_t para);

/**
 * De-initialises an PWM interface, Turns off an PWM hardware interface
 *
 * @param[in]  pwm  the interface which should be de-initialised
 *
 * @return  0 : on success, EIO : if an error occurred with any step
 */
int32_t hal_pwm_finalize(pwm_dev_t *pwm);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PWM_H */

