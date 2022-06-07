#include "switches_output.h"
#include <gpio.h>
#define OUT_SWITCHES_NUM  1

#define OUT0_POSITIVE_PIN (15)
#define OUT0_NEGATIVE_PIN (1)



struct switches_output_str switches_output[OUT_SWITCHES_NUM];

void switches_output_gpio_init(void)
{
    uint8_t i = 0;
    uint8_t n = 0;

    uint8_t ret;
    GENIE_LOG_INFO("switches_output_gpio_init");
    switches_output[0].out_positive_pin = OUT0_POSITIVE_PIN;
    switches_output[0].out_negative_pin = OUT0_NEGATIVE_PIN;

    for(i = 0; i < OUT_SWITCHES_NUM; i++)
    {
        switches_output[i].gpio_dev[0].port = switches_output[i].out_positive_pin;
        drv_pinmux_config(switches_output[i].gpio_dev[0].port, 99);
        switches_output[i].gpio_dev[0].config = INPUT_PULL_DOWN;
        hal_gpio_init(&switches_output[i].gpio_dev[0]);

        switches_output[i].gpio_dev[1].port = switches_output[i].out_negative_pin;
        drv_pinmux_config(switches_output[i].gpio_dev[1].port, 99);
        switches_output[i].gpio_dev[1].config = INPUT_PULL_DOWN;
        hal_gpio_init(&switches_output[i].gpio_dev[1]); 
      
    }
}

void switch_output_gpio_set(uint8_t element_id, uint8_t onoff)
{
    if(onoff)
    {
        phy_gpio_pull_set(switches_output[element_id].out_positive_pin, STRONG_PULL_UP);
        phy_gpio_pull_set(switches_output[element_id].out_negative_pin, PULL_DOWN);
    }
    else
    {
        phy_gpio_pull_set(switches_output[element_id].out_positive_pin, PULL_DOWN);
        phy_gpio_pull_set(switches_output[element_id].out_negative_pin, STRONG_PULL_UP);
    }
    GENIE_LOG_INFO("switch num:%d, state:%d", element_id,onoff);
}