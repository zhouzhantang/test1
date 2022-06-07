#ifndef _MY_LED_H_
#define _MY_LED_H_
#include <stdint.h>
#include "genie_sal.h"
#include "genie_mesh_api.h"

extern gpio_dev_t led1, led2_1, led2_2, led3;

void my_led_init(void);
void led_on(gpio_dev_t *gpio);
void led_off(gpio_dev_t *gpio);
#endif