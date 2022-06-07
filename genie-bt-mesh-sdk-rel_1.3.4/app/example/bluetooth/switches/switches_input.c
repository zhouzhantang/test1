/*
 * Copyright (C) 2019-2020 Alibaba Group Holding Limited
 */

#include "genie_sal.h"
#include "genie_mesh_api.h"

#include "switches_queue.h"
#include "switches_input.h"

#define ZZT_LOG(fmt, ...) LOGW("zzt", ##__VA_ARGS__)

static switches_input_event_t input_event[INPUT_IO_NUM];

static int input_event_port_map_index(uint8_t port)
{
    int i = 0;

    for (i = 0; i < INPUT_IO_NUM; i++)
    {
        if (port == input_event[i].gpio_dev.port)
        {
            break;
        }
    }

    if (i < INPUT_IO_NUM)
    {
        return i;
    }

    return -1;
}

static int input_event_send(uint8_t element_id, switch_press_type_e press_type)
{
    queue_mesg_t mesg;

    memset(&mesg, 0, sizeof(queue_mesg_t));
    mesg.type = QUEUE_MESG_TYPE_INPUT;
    mesg.element_id = element_id;
    mesg.data = (uint32_t)press_type;

    return queue_send_data(&mesg);
}

static void input_switch_press_cb(uint8_t io_idx)
{
    input_event[io_idx].event++;

    //reset timer
    aos_timer_stop(&input_event[io_idx].long_press_timer);
    aos_timer_start(&input_event[io_idx].long_press_timer);
}

static void input_switch_release_cb(uint8_t io_idx, long long release_time)
{
    input_event[io_idx].do_once = 1;

    input_event[io_idx].switch_keep_time = 0; //reset keep time
}

static bool input_io_status_get(uint8_t port)
{
    return genie_lpm_get_wakeup_io_status(port);
}

static void input_event_timer_cb(void *timer, void *arg)
{
    static 
    int io_idx = 0;
    gpio_dev_t *gpio_dev = (gpio_dev_t *)arg;
    if (gpio_dev == NULL)
    {
        GENIE_LOG_WARN("gpio_dev is null");
        return;
    }

    io_idx = input_event_port_map_index(gpio_dev->port);
    if (io_idx < 0)
    {
        GENIE_LOG_WARN("io_idx is err");
        return;
    }

    input_event[io_idx].press_type = SWITCH_PRESS_NONE;
    switch (input_event[io_idx].event)
    {
    case 1:
    {
        if (input_io_status_get(input_event[io_idx].gpio_dev.port))
        {
            input_event[io_idx].press_type = SWITCH_PRESS_ONCE;
            input_event[io_idx].long_press_cnt = 0;
        }
        else
        {   
            input_event[io_idx].long_press_cnt++;
            if(input_event[io_idx].long_press_cnt >= (3000/500))
            {
                input_event[io_idx].press_type = SWITCH_PRESS_LONG;
            }
            else
            {
                //reset timer
                aos_timer_stop(&input_event[io_idx].long_press_timer);
                aos_timer_start(&input_event[io_idx].long_press_timer); 
                return;               
            }

            
        }
    }
    break;
    case 2:
    {
        input_event[io_idx].press_type = SWITCH_PRESS_DOUBLE;
    }
    break;
    case 3:
    {
        input_event[io_idx].press_type = SWITCH_PRESS_TRIPLE;
    }
    break;
    case 5:
    {
        input_event[io_idx].press_type = SWITCH_PRESS_START_OTA;
    }
    break;
    default:
    {
        GENIE_LOG_WARN("not support press type\n");
    }
    break;
    }

    input_event[io_idx].event = 0;

    if (input_event[io_idx].press_type != SWITCH_PRESS_NONE)
    {
        GENIE_LOG_INFO("input_event[%d].press_type:%d", io_idx, input_event[io_idx].press_type);
        input_event_send(io_idx, input_event[io_idx].press_type);
    }
}

static void input_event_key_check_timer_cb(void *timer, void *arg)
{
    long long now_time = aos_now_ms();
    int io_idx = 0;
    gpio_dev_t *gpio_dev = (gpio_dev_t *)arg;
    if (gpio_dev == NULL)
    {
        GENIE_LOG_WARN("gpio_dev is null");
        return;
    }

    io_idx = input_event_port_map_index(gpio_dev->port);
    if (io_idx < 0)
    {
        GENIE_LOG_WARN("io_idx is err");
        return;
    }

    input_event[io_idx].switch_keep_time += INPUT_EVENT_KEY_CHECK_TIMEROUT;

    if ((input_event[io_idx].last_key_status == KEY_PRESSED) && input_io_status_get(input_event[io_idx].gpio_dev.port))
    {
        input_event[io_idx].last_key_status = KEY_RELEASED;
        input_switch_release_cb(io_idx, now_time);
        aos_timer_stop(&input_event[io_idx].key_check_timer);
    }
    else if (input_event[io_idx].last_key_status == KEY_RELEASED && input_io_status_get(input_event[io_idx].gpio_dev.port))
    {
        input_switch_release_cb(io_idx, now_time);
        aos_timer_stop(&input_event[io_idx].key_check_timer); //released status
    }

    if (input_event[io_idx].switch_keep_time > INPUT_EVENT_SLEEP_INVALID_PRESS_TIME)
    {
        aos_timer_stop(&input_event[io_idx].key_check_timer); //maybe accur error
    }
}

static void input_event_timer_init(void)
{
    int i = 0;

    for (i = 0; i < INPUT_IO_NUM; i++)
    {
        aos_timer_new(&input_event[i].long_press_timer, input_event_timer_cb, &input_event[i].gpio_dev, INPUT_EVENT_LONG_PRESS_TIMEROUT, 0);
        aos_timer_stop(&input_event[i].long_press_timer);

        aos_timer_new(&input_event[i].key_check_timer, input_event_key_check_timer_cb, &input_event[i].gpio_dev, INPUT_EVENT_KEY_CHECK_TIMEROUT, 1);
        aos_timer_stop(&input_event[i].key_check_timer);
    }
}

int32_t input_event_start_check_timer(uint8_t port)
{
    int io_idx = 0;
    long long interrupt_time = aos_now_ms();

    io_idx = input_event_port_map_index(port);

    if ((interrupt_time - input_event[io_idx].last_interrupt_time < INPUT_EVENT_DEBOUNCE_TIME))//消除抖动
    {
        return -1; //for debounce
    }

    input_event[io_idx].last_interrupt_time = interrupt_time;

    if (io_idx < INPUT_IO_NUM)
    {
        if (input_event[io_idx].last_key_status == KEY_RELEASED)
        {
            input_event[io_idx].last_key_status = KEY_PRESSED;
            input_switch_press_cb(io_idx);
        }

        aos_timer_stop(&input_event[io_idx].key_check_timer);
        return aos_timer_start(&input_event[io_idx].key_check_timer);
    }

    GENIE_LOG_WARN("port:%d err", port);
    return -1;
}

__attribute__((section(".__sram.code"))) void io_irq_handler(void *arg)
{
    gpio_dev_t *io = (gpio_dev_t *)arg;

    input_event_start_check_timer(io->port);
}

static int32_t input_event_gpio_init(void)
{
    int i = 0;

    input_event[0].gpio_dev.port = INPUT_EVENT_PIN_1;
    //input_event[1].gpio_dev.port = INPUT_EVENT_PIN_2;

    for (i = 0; i < INPUT_IO_NUM; i++)
    {
        input_event[i].gpio_dev.config = INPUT_PULL_UP;
        hal_gpio_init(&input_event[i].gpio_dev);
        hal_gpio_enable_irq(&input_event[i].gpio_dev, IRQ_TRIGGER_FALLING_EDGE, io_irq_handler, &input_event[i].gpio_dev);
    }

    return 0;
}

int32_t input_event_init(void)
{
    memset(input_event, 0, sizeof(input_event));

    input_event_timer_init();
    input_event_gpio_init();

    return 0;
}
