#include "my_adc.h"
#include <hal/soc/adc.h>
#include "adc.h"
#include "gpio.h"
#include "my_key.h"
adc_dev_t adc1,adc2;

#define POWER           0x0104

extern uint8_t worke_mode;
extern uint32_t my_massagegunspeed;
extern uint32_t my_massagegunspeed_data;
extern float power;
extern uint8_t vbus_flag;
extern uint8_t massagecontrol;
extern uint8_t bound_flag;

extern uint8_t first_bat_value;

extern timer_10ms;



void my_adc_init(void)
{
    /*
    drv_pinmux_config(P15, ADCC);
    adc1.port = hal_adc_pin2channel(P15);
    adc1.config.sampling_cycle = 100;
    hal_adc_init(&adc1);

    drv_pinmux_config(P14, ADCC);
    adc2.port = hal_adc_pin2channel(P14);
    adc2.config.sampling_cycle = 100;
    hal_adc_init(&adc2);
    */
}

uint8_t get_power_value(uint16_t bat_value)
{
    if(bat_value >=  1465)
    {
        return 100;
    }
    else if(bat_value < 1465 && bat_value >= 1404)//电池8.35V - 8.00v
    {
        return 75;
    }
    else if(bat_value < 1404 && bat_value >= 1333)//电池8.00V - 7.60v
    {
        return 50;
    }
    else if(bat_value < 1333 && bat_value >= 1263)//电池7.60V - 7.20v
    {
        return 25;
    }
    else if(bat_value < 1263 && bat_value < 500)//电池7.20V
    {
        return 0;
    }


}

void mode_handler(void)
{
    static uint8 worke_mode_bk = 0;
    if(worke_mode_bk != worke_mode)
    {
        switch (worke_mode)
        {
        case 1:
            my_massagegunspeed = 54;
            my_massagegunspeed_data = 2200;
            break;
        case 2:
            my_massagegunspeed = 56;
            my_massagegunspeed_data = 2400;
            break;        
        case 3:
            my_massagegunspeed = 58;
            my_massagegunspeed_data = 2600;
            break;
        case 4:
            my_massagegunspeed = 60;
            my_massagegunspeed_data = 2800;
            break;
        default:
            break;
        }
        worke_mode_bk = worke_mode;
        //send_backag_cnt = 1;
        report_vendor_func_packag(); 
        led_light(worke_mode);
        chang_motor_pwm(my_massagegunspeed);
    }


}

void charge_handler(void)
{
    int n = 0;//电压 mv
    int bat_value = 0;

    static uint8_t power_bk = 200;
    static uint8_t send_power_bk = 200;
    static uint8_t char_power_bk = 0;
    static uint8_t cal_cnt = 0;   
    static int power_diff = 0;
    static uint8_t power_diff_cnt = 0;
    static uint8_t gatt_con_send_power = 0;
    static uint8_t cnt = 0;

    uint8_t i = 0;

    for(i = 0; i < 10; i++)
    {
        hal_adc_value_get(&adc1, &n, HAL_WAIT_FOREVER);
        if(n < 100)
        {
            return 0 ;
        }
        bat_value += n;
    }

    bat_value = bat_value/10;
    /*
    if(vbus_flag)
    {
        power = (bat_value - 1167.0)/272.0;

        power = power*(int)100.0;//算电量百分比 

    }
    else
    {
        power = (bat_value - 1018.0)/386.0;

        power = power*(int)100.0;//算电量百分比        
    }

    if(bat_value <= 1018)
    {
        power = 0;
    }
    */

    if(vbus_flag)
    {
        power = (bat_value - 605.0)/140.0;
        power = power*(int)100.0;//算电量百分比 
    }
    else
    {
        power = (bat_value - 555.0)/181.0;

        power = power*(int)100.0;//算电量百分比        
    }

    if(bat_value <= 555)
    {
        power = 0;
    }

    
    if(vbus_flag)//充电显示
    {   

        if(first_bat_value >= 100)
        {
            led_light_flash(5);
        }
        else if(first_bat_value > 75 && first_bat_value < 100)//电池8.35V - 8.00v
        {
            led_light_flash(4);
        }
        else if(first_bat_value > 50 && first_bat_value <= 75)//电池8.00V - 7.60v
        {
            led_light_flash(3);
        }
        else if(first_bat_value > 25 && first_bat_value <= 50)//电池7.60V - 7.20v
        {
            led_light_flash(2);  
        }
        else if(first_bat_value > 0 && first_bat_value <= 25)//电池7.20V
        {
           led_light_flash(1);
        } 
        else if(first_bat_value == 0) 
        {
            led_light_flash(0);
        }       
    }



    //LOGI("charge_handler","send_power_bk:%d,bat_value:%d", send_power_bk,bat_value);


}

void scan_vbus(void)
{
    if(phy_gpio_read(GPIO_BUS) == 1)
    {
        vbus_flag = 1;
        aos_timer_stop(&timer_10ms);
        massagecontrol = 0;
        worke_mode = 0;
        my_massagegunspeed_data = 0;
        chang_motor_pwm(0);
        close_machine();  
        stop_led();
    }
    else
    {
        vbus_flag = 0;
    }
    //LOGI("can_vbus","vbus_flag:%d",vbus_flag);
}

void bat_adc_handler(void)
{
    int n = 0;//电压 mv
    int bat_value = 0;
         
    uint8_t i = 0;
    int16_t a = 0;
    int16_t b = 0;

    for(i = 0; i < 10; i++)
    {
        hal_adc_value_get(&adc1, &n, HAL_WAIT_FOREVER);
        bat_value += n;
    }

    bat_value = bat_value/10;
/*
    if(vbus_flag)
    {
        power = (bat_value - 1167.0)/272.0;

        power = power*(int)100.0;//算电量百分比 

    }
    else
    {
        power = (bat_value - 1018.0)/386.0;

        power = power*(int)100.0;//算电量百分比        
    }

    if(bat_value <= 1018)
    {
        power = 0;
    }

*/
    if(vbus_flag)
    {
        power = (bat_value - 605.0)/140.0;

        power = power*(int)100.0;//算电量百分比 
    }
    else
    {
        power = (bat_value - 555.0)/181.0;

        power = power*(int)100.0;//算电量百分比        
    }

    if(bat_value <= 555)
    {
        power = 0;
    }
    


    //LOGI("charge_handler","power:%ld,bat_value:%d", (uint16_t)power,bat_value);
    if(worke_mode == 0)
    {
        if( power > 75 )//电池8.35V
        {
           led_light(4);
        }
        else if(power > 50 && power <= 75)//电池8.35V - 8.00v
        {
            led_light(3);
        }
        else if(power > 25 && power <= 50)//电池8.00V - 7.60v
        {
            led_light(2);
        }
        else if(power > 0 && power <= 25)//电池7.60V - 7.20v
        {
            led_light(1);  
        }
        else if(power == 0)//电池7.20V
        {
           led_light(0);
        }         
    }
   
        
        if(power > 0)
        {
            first_bat_value = (uint8_t)power;
        }
        else
        {
            first_bat_value = 0;
        }
        
        if(first_bat_value >= 100)
        {
            first_bat_value = 100;
        }
    
        if(power == 0)
        {
            report_vendor_func((uint8_t)power,1,POWER);
            aos_msleep(3000);
            close_my_machine();
        }    

    //LOGI("adc_handler","bat_value:%d, motor_value:%d", bat_value,motor_value);

}