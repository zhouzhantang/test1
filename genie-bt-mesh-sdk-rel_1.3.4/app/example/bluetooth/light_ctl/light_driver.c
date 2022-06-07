/*
 * Copyright (C) 2015-2020 Alibaba Group Holding Limited
 */

#include "genie_sal.h"
#include "genie_mesh_api.h"
#include "pinmux.h"

#include "light.h"
#include "light_driver.h"

static gpio_dev_t gpio_c;
static gpio_dev_t gpio_w;
static pwm_dev_t light_led_c;
static pwm_dev_t light_led_w;

pwm_port_func_t pwm_port_channel[PWM_CHANNEL_COUNT] = {
    {COLD_PIN, PWM0},
    {WARM_PIN, PWM1},
};

uint16_t duty_list[] = {
    0,
    20,
    21,
    22,
    23,
    24,
    25,
    27,
    28,
    30,
    31,
    33,
    35,
    38,
    40,
    43,
    45,
    48,
    51,
    55,
    58,
    62,
    66,
    70,
    75,
    80,
    85,
    90,
    96,
    102,
    108,
    115,
    121,
    129,
    136,
    144,
    153,
    161,
    170,
    180,
    190,
    200,
    211,
    222,
    234,
    246,
    259,
    272,
    285,
    299,
    314,
    329,
    345,
    361,
    378,
    396,
    414,
    432,
    452,
    472,
    492,
    513,
    535,
    558,
    581,
    605,
    630,
    655,
    682,
    708,
    736,
    765,
    794,
    824,
    855,
    887,
    920,
    953,
    988,
    1023,
    1059,
    1097,
    1135,
    1174,
    1214,
    1255,
    1297,
    1340,
    1384,
    1430,
    1476,
    1523,
    1572,
    1621,
    1672,
    1724,
    1776,
    1831,
    1886,
    1942,
    1999,
};

void light_driver_init(void)
{
    gpio_c.port = pwm_port_channel[COLD_PORT_NUM].pin;
    gpio_c.config = OUTPUT_PUSH_PULL;
    hal_gpio_init(&gpio_c);

    light_led_c.port = COLD_PORT_NUM;
    light_led_c.config.duty_cycle = 0;
    drv_pinmux_config(COLD_PIN, pwm_port_channel[COLD_PORT_NUM].port);
    light_led_c.config.freq = LIGHT_PERIOD;
    hal_pwm_init(&light_led_c);
    hal_pwm_start(&light_led_c);

    gpio_w.port = pwm_port_channel[WARM_PORT_NUM].pin;
    gpio_w.config = OUTPUT_PUSH_PULL;
    hal_gpio_init(&gpio_w);

    light_led_w.port = WARM_PORT_NUM;
    light_led_w.config.duty_cycle = 0;
    drv_pinmux_config(WARM_PIN, pwm_port_channel[WARM_PORT_NUM].port);
    light_led_w.config.freq = LIGHT_PERIOD;
    hal_pwm_init(&light_led_w);
    hal_pwm_start(&light_led_w);
}

//temperature 800~20000
//ligntness 1~65535
//return duty 1-100
static void _get_led_duty(uint8_t *p_duty, uint16_t lightness, uint16_t temperature)
{
    uint8_t cold = 0;
    uint8_t warm = 0;

    if (temperature > COLOR_TEMPERATURE_MAX)
    {
        temperature = COLOR_TEMPERATURE_MAX;
    }
    if (temperature < COLOR_TEMPERATURE_MIN)
    {
        temperature = COLOR_TEMPERATURE_MIN;
    }

    //0-100
    cold = (temperature - COLOR_TEMPERATURE_MIN) * 100 / (COLOR_TEMPERATURE_MAX - COLOR_TEMPERATURE_MIN);
    warm = 100 - cold;

    p_duty[LED_COLD_CHANNEL] = (lightness * cold) / 65500;
    p_duty[LED_WARM_CHANNEL] = (lightness * warm) / 65500;
    if (p_duty[LED_COLD_CHANNEL] == 0 && p_duty[LED_WARM_CHANNEL] == 0)
    {
        if (temperature > (COLOR_TEMPERATURE_MAX - COLOR_TEMPERATURE_MIN) >> 1)
        {
            p_duty[LED_COLD_CHANNEL] = 1;
        }
        else
        {
            p_duty[LED_WARM_CHANNEL] = 1;
        }
    }

    //LIGHT_DBG("%d %d [%d %d] [%d %d]", lightness, temperature, warm, cold, p_duty[LED_COLD_CHANNEL], p_duty[LED_WARM_CHANNEL]);
}

static int _set_pwm_duty(uint8_t channel, uint8_t duty)
{
    int err = -1;
    pwm_config_t pwm_cfg;
    pwm_dev_t *pwm_dev = NULL;

    if (duty > 100)
    {
        LIGHT_DBG(">>duty invaild\r\n");
        return -1;
    }

    pwm_cfg.freq = LIGHT_PERIOD;
    pwm_cfg.duty_cycle = duty_list[duty];

    if (channel == LED_COLD_CHANNEL)
    {
        pwm_dev = &light_led_c;
    }
    else if (channel == LED_WARM_CHANNEL)
    {
        pwm_dev = &light_led_w;
    }
    else
    {
        return -1;
    }

    drv_pinmux_config(pwm_port_channel[pwm_dev->port].pin, pwm_port_channel[pwm_dev->port].port);
    err = hal_pwm_para_chg(pwm_dev, pwm_cfg);
    if (err)
    {
        LIGHT_DBG("pwm err %d\n", err);
        return -1;
    }

    /* if duty is 0 or 100, pinmux pwm to gpio */
    if (pwm_cfg.duty_cycle == 0)
    {
        if (pwm_dev->port == COLD_PORT_NUM)
        {
            hal_gpio_output_low(&gpio_c);
            drv_pinmux_config(pwm_port_channel[pwm_dev->port].pin, PIN_FUNC_GPIO);
        }
        else
        {
            hal_gpio_output_low(&gpio_w);
            drv_pinmux_config(pwm_port_channel[pwm_dev->port].pin, PIN_FUNC_GPIO);
        }
    }

    if (pwm_cfg.duty_cycle == (1000000 / pwm_cfg.freq))
    {
        if (pwm_dev->port == COLD_PORT_NUM)
        {
            hal_gpio_output_high(&gpio_c);
            drv_pinmux_config(pwm_port_channel[pwm_dev->port].pin, PIN_FUNC_GPIO);
        }
        else
        {
            hal_gpio_output_high(&gpio_w);
            drv_pinmux_config(pwm_port_channel[pwm_dev->port].pin, PIN_FUNC_GPIO);
        }
    }

    return 0;
}

//lightness 1-65535
void light_driver_update(uint8_t onoff, uint16_t lightness, uint16_t temperature)
{
    uint8_t duty[LED_CHANNEL_MAX]; //0~100
    static uint8_t last_duty[LED_CHANNEL_MAX] = {0xFF, 0xFF};

    //LIGHT_DBG("%d %d %d", onoff, lightness, temperature);

    if (onoff == 0)
    {
        duty[LED_COLD_CHANNEL] = 0;
        duty[LED_WARM_CHANNEL] = 0;
    }
    else
    {
        _get_led_duty(duty, lightness, temperature);
    }

    if (last_duty[LED_COLD_CHANNEL] != duty[LED_COLD_CHANNEL])
    {
        last_duty[LED_COLD_CHANNEL] = duty[LED_COLD_CHANNEL];
        _set_pwm_duty(LED_COLD_CHANNEL, duty[LED_COLD_CHANNEL]);
    }
    if (last_duty[LED_WARM_CHANNEL] != duty[LED_WARM_CHANNEL])
    {
        last_duty[LED_WARM_CHANNEL] = duty[LED_WARM_CHANNEL];
        _set_pwm_duty(LED_WARM_CHANNEL, duty[LED_WARM_CHANNEL]);
    }
}
