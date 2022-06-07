#include <stdint.h>
#include <aos/kernel.h>
#include <aos/aos.h>
//#include "tg7100_priv.h"
#include "my_key.h"
#include "my_led.h"

#include "genie_sal.h"
#include "genie_mesh_api.h"
#include "pinmux.h"

gpio_dev_t key1,vbus;

extern uint8_t key_pressed;
extern uint8_t worke_mode;

void my_key_init(void)
{

    drv_pinmux_config(GPIO_KEY, PIN_FUNC_GPIO);
    key1.port = GPIO_KEY;
    key1.config = INPUT_HIGH_IMPEDANCE;
    hal_gpio_init(&key1);
    
}

void vbus_init(void)
{
    drv_pinmux_config(GPIO_BUS, PIN_FUNC_GPIO);
    vbus.port = GPIO_BUS;
    vbus.config = INPUT_PULL_DOWN;
    hal_gpio_init(&vbus);
	
}



void key_handler(void)
{
    static uint8 key_cnt = 0;

    if(phy_gpio_read(GPIO_KEY) == 1)
    {
        key_cnt++;
        if( key_cnt >= 15 && worke_mode != 0)//长按
        {
            close_my_machine();
            LOGI("key_handler","long_press");        
        }
        else if(key_cnt == 20)
        {
            LOGI("key_handler","key_cnt:20");
            led_light(1);
        }
        else if(key_cnt == 30)
        {
            LOGI("key_handler","key_cnt:30");
            led_light(2);
        }
        else if(key_cnt == 40)
        {
            LOGI("key_handler","key_cnt:40");
            led_light(3);
        }
        else if(key_cnt == 50)
        {
            LOGI("key_handler","key_cnt:50");
            led_light(4);   
             
        }
    }
    else
    {
        if(key_cnt > 0 && key_cnt < 15)//短按
        {
           LOGI("key_handler","short_press");
           mode_change(); 
           mode_handler();
           key_cnt = 0;
        }
        
        else if(key_cnt > 0 && key_cnt <= 50 && worke_mode == 0)
        {
            close_my_machine(); 
        }
        
        else if(key_cnt >= 50)
        {
            LOGI("key_handler","UnBinding");
            key_cnt = 0;
            UnBinding();
        }
        else
        {
            key_cnt = 0;
        }
    }

}