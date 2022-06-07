#ifndef __SWITCHES_INPUT_H__
#define __SWITCHES_INPUT_H__

#define INPUT_IO_NUM (1)

#define INPUT_EVENT_PIN_1 (14)
#define INPUT_EVENT_PIN_2 (14)
#define INPUT_EVENT_PIN_3 (16)
#define INPUT_EVENT_PIN_4 (17)

#define INPUT_PIN_POL_PIN_1 (FALLING)
#define INPUT_PIN_POL_PIN_2 (FALLING)
#define INPUT_PIN_POL_PIN_3 (FALLING)
#define INPUT_PIN_POL_PIN_4 (FALLING)

#define INPUT_EVENT_LONG_PRESS_TIMEROUT (500)          //unit ms
#define INPUT_EVENT_KEY_CHECK_TIMEROUT (30)            //unit ms

#define INPUT_EVENT_SLEEP_INVALID_PRESS_TIME (40 * 1000)

#define INPUT_EVENT_DEBOUNCE_TIME (50) //For debounce

typedef enum _switch_press_type
{
    SWITCH_PRESS_NONE = 0,
    SWITCH_PRESS_ONCE = 5,
    SWITCH_PRESS_DOUBLE = 6,
    SWITCH_PRESS_LONG = 7,
    SWITCH_PRESS_TRIPLE,
    SWITCH_PRESS_PROVISION,
    SWITCH_PRESS_RESET,
    SWITCH_PRESS_START_OTA,
    SWITCH_PRESS_STANDBY,
    SWITCH_PRESS_PRESSED,
    SWITCH_PRESS_RELEASED
} switch_press_type_e;

typedef enum _key_status_s
{
    KEY_RELEASED,
    KEY_PRESSED
} key_status_e;

typedef struct _switches_input_event_s
{
    bool do_once;
    uint8_t event;
    uint32_t switch_keep_time;
    long long last_interrupt_time;
    key_status_e last_key_status;
    key_status_e cur_key_status;
    switch_press_type_e press_type;
    gpio_dev_t gpio_dev;
    aos_timer_t key_check_timer;
    aos_timer_t long_press_timer;
    uint8_t long_press_cnt;
} switches_input_event_t;

int32_t input_event_init(void);
int32_t input_event_start_check_timer(uint8_t port);

#endif
