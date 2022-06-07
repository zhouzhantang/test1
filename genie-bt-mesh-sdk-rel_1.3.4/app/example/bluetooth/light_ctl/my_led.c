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

static char *tag = "led";

#define GPIO_LED 	 GPIO_P18
#define GPIO_LED1 	 GPIO_P34
#define GPIO_LED2    GPIO_P32
#define GPIO_LED3    GPIO_P33
#define GPIO_LED4 	 GPIO_P31

#define GPIO_MOTOR 	GPIO_P01
#define GPIO_EN 	GPIO_P20



pwm_dev_t light_led, motor_pwm;
gpio_dev_t led, led1, led2, led3, led4, motor_gpio,en_gpio;


void my_led_init(void)
{
	int ret = -1;

	/*LED1*/
	drv_pinmux_config(GPIO_LED1, PIN_FUNC_GPIO);
	led1.port = GPIO_LED1;
	led1.config = OUTPUT_PUSH_PULL;
	ret = hal_gpio_init(&led1);
	hal_gpio_output_low(&led1);

	/*LED2*/
	drv_pinmux_config(GPIO_LED2, PIN_FUNC_GPIO);
	led2.port = GPIO_LED2;
	led2.config = OUTPUT_PUSH_PULL;
	ret = hal_gpio_init(&led2);
	hal_gpio_output_low(&led2);

	/*led3*/
	drv_pinmux_config(GPIO_LED3, PIN_FUNC_GPIO);
	led3.port = GPIO_LED3;
	led3.config = OUTPUT_PUSH_PULL;
	ret = hal_gpio_init(&led3);
	hal_gpio_output_low(&led3);	

	/*led4*/
	drv_pinmux_config(GPIO_LED4, PIN_FUNC_GPIO);
	led4.port = GPIO_LED4;
	led4.config = OUTPUT_PUSH_PULL;
	ret = hal_gpio_init(&led4);
	hal_gpio_output_low(&led4);
 
}

void EN_init(void)
{
	drv_pinmux_config(GPIO_EN, PIN_FUNC_GPIO);
	en_gpio.port = GPIO_EN;
	en_gpio.config = OUTPUT_PUSH_PULL;
	hal_gpio_init(&en_gpio);
	hal_gpio_output_high(&en_gpio);

}

void close_machine(void)
{
	hal_gpio_output_low(&en_gpio);
}



void led_light(uint8 num)
{
	if(num == 1)
	{
		hal_gpio_output_high(&led1);
		hal_gpio_output_low(&led2);
		hal_gpio_output_low(&led3);
		hal_gpio_output_low(&led4);
	}
	else if(num == 2)
	{
		hal_gpio_output_high(&led2);
		hal_gpio_output_high(&led1);
		hal_gpio_output_low(&led3);
		hal_gpio_output_low(&led4);
	}
	else if(num == 3)
	{
		hal_gpio_output_high(&led3);
		hal_gpio_output_high(&led1);
		hal_gpio_output_high(&led2);
		hal_gpio_output_low(&led4);
	}
	else if(num == 4)
	{
		hal_gpio_output_high(&led4);
		hal_gpio_output_high(&led1);
		hal_gpio_output_high(&led2);
		hal_gpio_output_high(&led3);		
	}
	else if(num == 0)
	{
		hal_gpio_output_low(&led1);
		hal_gpio_output_low(&led2);
		hal_gpio_output_low(&led3);	
		hal_gpio_output_low(&led4);		
	}
}


void led_light_flash(uint8_t num)
{
    switch (num)
    {
    case 1:
		hal_gpio_output_toggle(&led1);
		hal_gpio_output_low(&led2);
		hal_gpio_output_low(&led3);	
		hal_gpio_output_low(&led4);	
            break;
    case 2:
		hal_gpio_output_high(&led1);
		hal_gpio_output_toggle(&led2);
		hal_gpio_output_low(&led3);	
		hal_gpio_output_low(&led4);	
            break;        
    case 3:
		hal_gpio_output_high(&led1);
		hal_gpio_output_high(&led2);
		hal_gpio_output_toggle(&led3);	
		hal_gpio_output_low(&led4);	
            break;
     case 4:
		hal_gpio_output_high(&led1);
		hal_gpio_output_high(&led2);
		hal_gpio_output_high(&led3);	
		hal_gpio_output_toggle(&led4);	
            break;
     case 5:
		hal_gpio_output_high(&led1);
		hal_gpio_output_high(&led2);
		hal_gpio_output_high(&led3);	
		hal_gpio_output_high(&led4);	
            break;

    default:
        break;
    }


}

void led_on(gpio_dev_t *gpio)
{
    hal_gpio_output_low(gpio);
}

void led_off(gpio_dev_t *gpio)
{
    hal_gpio_output_high(gpio);
}

void led_toggle(gpio_dev_t *gpio)
{
    hal_gpio_output_toggle(gpio);
}

void led_pwm_init(void)
{
	int ret = -1;


	led.port = GPIO_LED;
	led.config = OUTPUT_PUSH_PULL;
	hal_gpio_init(&led);

	drv_pinmux_config(GPIO_LED, PWM0);

    light_led.port = 0;
    light_led.config.duty_cycle = 0;   
    light_led.config.freq = 500;//500hz 2000us
    hal_pwm_init(&light_led);
    hal_pwm_start(&light_led);	
}

void led_hb(uint8_t mode)//1开 0关
{
	static uint16 cnt = 0;
	static uint8 dowm_up_flag = 1;//0 dowm 1 up
	pwm_config_t pwm_cfg;

	if(mode) 
	{
		if(cnt > 0 && cnt < 2000)
		{
			pwm_cfg.duty_cycle = cnt;   
			pwm_cfg.freq = 500;//500hz 2000us
			hal_pwm_para_chg(&light_led, pwm_cfg);

		}
		else if(cnt == 0)
		{
			dowm_up_flag = 1;
			hal_gpio_output_low(&led);
		}
		else if(cnt == 2000)
		{
			hal_gpio_output_high(&led);
			dowm_up_flag = 0;
		}

		if(dowm_up_flag)
		{
				cnt += 10;	
			
		}
		else
		{

				cnt -= 10;	
			
		}
	}
	else
	{
		pwm_cfg.duty_cycle = 0;   
		pwm_cfg.freq = 500;//500hz 2000us
		hal_pwm_para_chg(&light_led, pwm_cfg);		
		hal_pwm_stop(&light_led);
		hal_gpio_output_high(&led);
	}

	//LOGI(tag, "cnt:%d\n",cnt);

}

void stop_led(void)
{
	hal_pwm_finalize(&light_led);
	hal_pwm_stop(&light_led);

	drv_pinmux_config(GPIO_LED, PIN_FUNC_GPIO);	
	led.port = GPIO_LED;
	led.config = OUTPUT_PUSH_PULL;
	hal_gpio_init(&led);

	hal_gpio_output_low(&led);
}

extern timer_10ms;
void provisioned_led_flash(void)
{
    if(bt_mesh_is_provisioned())//已经配网
    {  
        led_hb(0); 
		aos_timer_stop(&timer_10ms);  
		aos_timer_free(&timer_10ms); 
    }
    else//未配网
    {
       led_hb(1);
    }

}

void motor_pwm_init(void)
{
	motor_gpio.port = GPIO_MOTOR;
	motor_gpio.config = INPUT_PULL_DOWN;
	hal_gpio_init(&motor_gpio);

	drv_pinmux_config(GPIO_MOTOR, PWM1);

    motor_pwm.port = 1;
    motor_pwm.config.duty_cycle = 0;   
    motor_pwm.config.freq = 12820;//12820hz 78us
    hal_pwm_init(&motor_pwm);
	hal_pwm_start(&motor_pwm);
}

extern uint8_t massagecontrol;
void chang_motor_pwm(uint16 value)
{
	pwm_config_t pwm_cfg;
	static uint8 close_PWM_flag = 1;
	if(value == 0)
	{
		hal_pwm_stop(&motor_pwm);
		hal_pwm_finalize(&motor_pwm);

		drv_pinmux_config(GPIO_MOTOR, PIN_FUNC_GPIO);	
		led.port = GPIO_MOTOR;
		led.config = OUTPUT_PUSH_PULL;
		hal_gpio_init(&motor_gpio);

		hal_gpio_output_low(&motor_gpio);		
		close_PWM_flag = 0;
	}
	else
	{
		if(massagecontrol)
		{
			if(close_PWM_flag == 0)
			{
				motor_pwm_init();
				close_PWM_flag = 1;
			}
			pwm_cfg.duty_cycle = value;   
			pwm_cfg.freq = 12820;//500hz 2000us
			hal_pwm_para_chg(&motor_pwm, pwm_cfg);
			hal_pwm_start(&motor_pwm);
		}

	}

}



