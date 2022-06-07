#ifndef _MY_KEY_H_
#define _MY_KEY_H_
#include <stdint.h>
#include <stdint.h>
#include <aos/kernel.h>
#include <aos/aos.h>
//#include "tg7100_priv.h"
#include "my_led.h"
#include "gpio.h"
#include <hal/soc/pwm.h>
#include "genie_sal.h"
#include "genie_mesh_api.h"
#include "pinmux.h"

#define GPIO_KEY GPIO_P03
#define GPIO_BUS GPIO_P02
void my_key_init(void);


#endif