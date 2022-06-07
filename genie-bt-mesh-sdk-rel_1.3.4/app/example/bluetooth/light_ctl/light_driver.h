#ifndef __LIGHT_DRIVER_H__
#define __LIGHT_DRIVER_H__

#include <stdint.h>
#include <stdint.h>
#include <aos/kernel.h>
#include <aos/aos.h>

#include "my_led.h"
#include "gpio.h"
#include <hal/soc/pwm.h>
#include "genie_sal.h"
#include "genie_mesh_api.h"
#include "pinmux.h"

#define WARM_PIN (24) //warm led
#define COLD_PIN (20) //cold led

#define PWM0 (10)
#define PWM1 (11)

#define COLD_PORT_NUM 0
#define WARM_PORT_NUM 1

#define PWM_CHANNEL_COUNT 2

#define LIGHT_PERIOD 500

enum
{
    LED_COLD_CHANNEL = 0,
    LED_WARM_CHANNEL,
    LED_CHANNEL_MAX
};

typedef struct
{
    uint8_t pin;
    uint8_t port;
} pwm_port_func_t;

void light_driver_init(void);
void light_driver_update(uint8_t onoff, uint16_t lightness, uint16_t temperature);

#endif
