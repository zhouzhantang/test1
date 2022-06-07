/*
 * Copyright (C) 2019-2020 Alibaba Group Holding Limited
 */

#include "genie_sal.h"
#include "genie_mesh_api.h"

#include "light.h"
#include "light_driver.h"

#include "my_key.h"
#include "drv_common.h"



static light_param_t light_param;
static sig_model_element_state_t light_elem_stat[ELEMENT_NUM];
static sig_model_powerup_t light_powerup[ELEMENT_NUM];



extern uart_0;
//extern uint16_t Cur_Weight;
extern uint8_t wait_uart_rx_over;
extern uint8_t uart_it_buf[512];
extern uint8_t uart_it_index;

#ifdef CONFIG_BT_MESH_CFG_CLI
static struct bt_mesh_cfg_cli cfg_cli = {};
#endif

static struct bt_mesh_model primary_element[] = {
    BT_MESH_MODEL_CFG_SRV(),
    BT_MESH_MODEL_HEALTH_SRV(),
#ifdef CONFIG_BT_MESH_CFG_CLI
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),
#endif
    MESH_MODEL_GEN_ONOFF_SRV(&light_elem_stat[0]),
    MESH_MODEL_LIGHTNESS_SRV(&light_elem_stat[0]),
    MESH_MODEL_CTL_SRV(&light_elem_stat[0]),
    MESH_MODEL_SCENE_SRV(&light_elem_stat[0]),
};

static struct bt_mesh_model primary_vendor_element[] = {
    MESH_MODEL_VENDOR_SRV(&light_elem_stat[0]),
};

struct bt_mesh_elem light_elements[] = {
    BT_MESH_ELEM(0, primary_element, primary_vendor_element, GENIE_ADDR_LIGHT),
};

#ifdef CONFIG_GENIE_OTA
bool genie_sal_ota_is_allow_reboot(void)
{
    // the device will reboot when it is off
    if (light_elem_stat[0].state.onoff[TYPE_PRESENT] == 0)
    {
        // save light para, always off
        light_powerup[0].last_onoff = 0;
        genie_storage_write_userdata(GFI_MESH_POWERUP, (uint8_t *)light_powerup, sizeof(light_powerup));
        LIGHT_DBG("Allow to reboot!");

        return true;
    }

    LIGHT_DBG("light is no so no reboot!");

    return false;
}
#endif

#ifdef CONFIG_MESH_MODEL_TRANS
static void _mesh_delay_timer_cb(void *p_timer, void *p_arg)
{
    sig_model_element_state_t *p_elem = (sig_model_element_state_t *)p_arg;

    sig_model_transition_timer_stop(p_elem);
    sig_model_event(SIG_MODEL_EVT_DELAY_END, p_arg);
}

static void _mesh_trans_timer_cycle(void *p_timer, void *p_arg)
{
    sig_model_element_state_t *p_elem = (sig_model_element_state_t *)p_arg;
    sig_model_state_t *p_state = &p_elem->state;

    sig_model_transition_timer_stop(p_elem);

    //do cycle
    sig_model_event(SIG_MODEL_EVT_TRANS_CYCLE, p_arg);
    //BT_DBG(">>>>>%d %d", (u32_t)cur_time, (u32_t)p_elem->state.trans_end_time);

    if (p_state->trans == 0)
    {
        sig_model_event(SIG_MODEL_EVT_TRANS_END, p_arg);
    }
    else
    {
        k_timer_start(&p_state->trans_timer, SIG_MODEL_TRANSITION_INTERVAL);
    }
}
#endif

void light_elem_state_init(void)
{
    uint8_t index = 0;
    genie_storage_status_e ret;

    memset(light_elem_stat, 0, sizeof(light_elem_stat));

    // load light param
    ret = genie_storage_read_userdata(GFI_MESH_POWERUP, (uint8_t *)light_powerup, sizeof(light_powerup));
    for (index = 0; index < ELEMENT_NUM; index++)
    {
        light_elem_stat[index].element_id = index;

        if (ret == GENIE_STORAGE_SUCCESS) //Use saved data
        {
            memcpy(&light_elem_stat[index].powerup, &light_powerup[index], sizeof(sig_model_powerup_t));
        }
        else //Use default value
        {
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
            light_elem_stat[index].powerup.last_onoff = GEN_ONOFF_DEFAULT;
#endif
#ifdef CONFIG_MESH_MODEL_LIGHTNESS_SRV
            light_elem_stat[index].powerup.last_lightness = LIGHTNESS_DEFAULT;
#endif
#ifdef CONFIG_MESH_MODEL_CTL_SRV
            light_elem_stat[index].powerup.last_color_temperature = COLOR_TEMPERATURE_DEFAULT;
#endif
#ifdef CONFIG_MESH_MODEL_SCENE_SRV
            light_elem_stat[index].powerup.last_scene = SCENE_DEFAULT;
#endif
        }

#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
        light_elem_stat[index].state.onoff[TYPE_PRESENT] = 0;
        light_elem_stat[index].state.onoff[TYPE_TARGET] = light_elem_stat[index].powerup.last_onoff;
#endif
#ifdef CONFIG_MESH_MODEL_LIGHTNESS_SRV
        light_elem_stat[index].state.lightness[TYPE_PRESENT] = 0;
        light_elem_stat[index].state.lightness[TYPE_TARGET] = light_elem_stat[index].powerup.last_lightness;
#endif
#ifdef CONFIG_MESH_MODEL_CTL_SRV
        light_elem_stat[index].state.color_temperature[TYPE_PRESENT] = 0;
        light_elem_stat[index].state.color_temperature[TYPE_TARGET] = light_elem_stat[index].powerup.last_color_temperature;
#endif
#ifdef CONFIG_MESH_MODEL_SCENE_SRV
        light_elem_stat[index].state.scene[TYPE_PRESENT] = GENIE_SCENE_NORMAL;
        light_elem_stat[index].state.scene[TYPE_TARGET] = light_elem_stat[index].powerup.last_scene;
#endif

#ifdef CONFIG_MESH_MODEL_TRANS
        k_timer_init(&light_elem_stat[index].state.delay_timer, _mesh_delay_timer_cb, &light_elem_stat[index]);
        k_timer_init(&light_elem_stat[index].state.trans_timer, _mesh_trans_timer_cycle, &light_elem_stat[index]);

        light_elem_stat[index].state.trans = TRANSITION_DEFAULT_VALUE;
        light_elem_stat[index].state.delay = DELAY_DEFAULT_VAULE;
        if (light_elem_stat[index].state.trans)
        {
            light_elem_stat[index].state.trans_start_time = k_uptime_get() + light_elem_stat[index].state.delay * DELAY_TIME_UNIT;
            light_elem_stat[index].state.trans_end_time = light_elem_stat[index].state.trans_start_time + sig_model_transition_get_transition_time(light_elem_stat[index].state.trans);
        }

#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
        if (light_elem_stat[index].state.onoff[TYPE_TARGET] == 1) //light on
        {
            sig_model_event(SIG_MODEL_EVT_DELAY_START, &light_elem_stat[index]);
        }
#endif
#endif
    }
}

static void light_ctl_handle_order_msg(vendor_attr_data_t *attr_data)
{
    GENIE_LOG_INFO("type:%04x data:%04x\r\n", attr_data->type, attr_data->para);

    if (attr_data->type == ATTR_TYPE_GENERIC_ONOFF)
    {
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
        light_elem_stat[0].state.onoff[TYPE_TARGET] = attr_data->para;

        if (light_elem_stat[0].state.onoff[TYPE_PRESENT] != light_elem_stat[0].state.onoff[TYPE_TARGET])
        {
            sig_model_event(SIG_MODEL_EVT_DELAY_START, &light_elem_stat[0]);
        }
#endif
    }
}

static void light_param_reset(void)
{
    genie_storage_delete_userdata(GFI_MESH_POWERUP);
}

static void light_save_state(sig_model_element_state_t *p_elem)
{
    uint8_t *p_read = NULL;

    if (p_elem->state.lightness[TYPE_PRESENT] != 0)
    {
        p_elem->powerup.last_lightness = p_elem->state.lightness[TYPE_PRESENT];
        light_powerup[p_elem->element_id].last_lightness = p_elem->state.lightness[TYPE_PRESENT];
    }

    p_elem->powerup.last_color_temperature = p_elem->state.color_temperature[TYPE_PRESENT];
    light_powerup[p_elem->element_id].last_color_temperature = p_elem->state.color_temperature[TYPE_PRESENT];
    // always on
    p_elem->powerup.last_onoff = 1;
    light_powerup[p_elem->element_id].last_onoff = 1;

    p_read = aos_malloc(sizeof(light_powerup));
    if (!p_read)
    {
        GENIE_LOG_WARN("no mem");
        return;
    }

    genie_storage_read_userdata(GFI_MESH_POWERUP, p_read, sizeof(light_powerup));

    if (memcmp(light_powerup, p_read, sizeof(light_powerup)))
    {
        LIGHT_DBG("save %d %d %d", light_powerup[0].last_onoff, light_powerup[0].last_lightness, light_powerup[0].last_color_temperature);
        genie_storage_write_userdata(GFI_MESH_POWERUP, (uint8_t *)light_powerup, sizeof(light_powerup));
    }

    aos_free(p_read);
#ifdef CONFIG_GENIE_OTA
    if (light_powerup[0].last_onoff == 0 && genie_ota_is_ready() == 1)
    {
        //Means have ota, wait for reboot while light off
        GENIE_LOG_INFO("reboot by ota");
        aos_reboot();
    }
#endif
}

static void light_update(sig_model_element_state_t *p_elem)
{
    static uint8_t last_onoff = 0;
    static uint16_t last_lightness = 0;
    static uint16_t last_temperature = 0;

    uint8_t onoff = p_elem->state.onoff[TYPE_PRESENT];
    uint16_t lightness = p_elem->state.lightness[TYPE_PRESENT];
    uint16_t temperature = p_elem->state.color_temperature[TYPE_PRESENT];

    if (last_onoff != onoff || last_lightness != lightness || last_temperature != temperature)
    {
        last_onoff = onoff;
        last_lightness = lightness;
        last_temperature = temperature;
        //LIGHT_DBG("%d,%d,%d", onoff, lightness, temperature);
        light_driver_update(onoff, lightness, temperature);
    }
}

void _cal_flash_next_step(uint32_t delta_time)
{
    uint16_t lightness_end;

    if (delta_time < 1000)
    {
        lightness_end = light_param.target_lightness;
        light_param.present_color_temperature = light_param.target_color_temperature;
    }
    else
    {
        lightness_end = light_param.lightness_start;
        delta_time %= 1000;
    }

    if (delta_time > LED_BLINK_ON_TIME)
    {
        delta_time -= LED_BLINK_ON_TIME;
        light_param.present_lightness = light_param.lightness_start * delta_time / LED_BLINK_OFF_TIME;
    }
    else
    {
        light_param.present_lightness = lightness_end * (LED_BLINK_ON_TIME - delta_time) / LED_BLINK_ON_TIME;
    }
    //LIGHT_DBG("delta %d, lightness %04x", delta_time, light_param.present_lightness);
}

static void _led_blink_timer_cb(void *p_timer, void *p_arg)
{
    uint32_t cur_time = k_uptime_get();

    if (cur_time >= light_param.color_temperature_end)
    {
        light_driver_update(1, light_param.target_lightness, light_param.target_color_temperature);
    }
    else
    {
        _cal_flash_next_step(light_param.color_temperature_end - cur_time);
        light_driver_update(1, light_param.present_lightness, light_param.present_color_temperature);
        k_timer_start(&light_param.led_blink_timer, SIG_MODEL_TRANSITION_INTERVAL);
    }
}

static void light_led_blink(uint8_t times, uint8_t reset)
{
    if (light_elem_stat[0].state.onoff[TYPE_PRESENT] == 1)
    {
        if (light_elem_stat[0].state.lightness[TYPE_PRESENT])
        {
            light_param.lightness_start = light_param.present_lightness = light_elem_stat[0].state.lightness[TYPE_PRESENT];
        }
        else
        {
            light_param.lightness_start = light_param.present_lightness = LIGHTNESS_DEFAULT;
        }

        if (light_elem_stat[0].state.color_temperature[TYPE_PRESENT])
        {
            light_param.present_color_temperature = light_elem_stat[0].state.color_temperature[TYPE_PRESENT];
        }
        else
        {
            light_param.present_color_temperature = COLOR_TEMPERATURE_DEFAULT;
        }

        if (reset)
        {
            light_param.target_lightness = LIGHTNESS_DEFAULT;
            light_param.target_color_temperature = COLOR_TEMPERATURE_DEFAULT;
        }
        else
        {
            light_param.target_lightness = light_param.present_lightness;
            light_param.target_color_temperature = light_param.present_color_temperature;
        }

        light_param.color_temperature_end = k_uptime_get() + times * LED_BLINK_PERIOD;
    }
    else
    {
        if (light_elem_stat[0].powerup.last_lightness && !reset)
        {
            light_param.lightness_start = light_param.target_lightness = light_elem_stat[0].powerup.last_lightness;
        }
        else
        {
            light_param.lightness_start = light_param.target_lightness = LIGHTNESS_DEFAULT;
        }

        light_param.present_lightness = 0;
        if (light_elem_stat[0].powerup.last_color_temperature)
        {
            light_param.present_color_temperature = light_elem_stat[0].powerup.last_color_temperature;
        }
        else
        {
            light_param.present_color_temperature = COLOR_TEMPERATURE_DEFAULT;
        }

        if (reset)
        {
            light_param.target_color_temperature = COLOR_TEMPERATURE_DEFAULT;
        }
        else
        {
            light_param.target_color_temperature = light_param.present_color_temperature;
        }

        light_param.color_temperature_end = k_uptime_get() + times * LED_BLINK_PERIOD - LED_BLINK_OFF_TIME;
    }
    //LIGHT_DBG("%d (%d-%d) tar %04x", times, k_uptime_get(), light_param.color_temperature_end, light_param.target_lightness);

    k_timer_start(&light_param.led_blink_timer, SIG_MODEL_TRANSITION_INTERVAL);
}
 void light_report_poweron_state(void)
{
    uint16_t index = 0;
    uint8_t payload[20];
    genie_transport_payload_param_t transport_payload_param;



    memset(&transport_payload_param, 0, sizeof(genie_transport_payload_param_t));
    transport_payload_param.opid = VENDOR_OP_ATTR_INDICATE;
    transport_payload_param.p_payload = payload;
    transport_payload_param.payload_len = index;
    transport_payload_param.retry_cnt = EVENT_REPORT_RETRY;

    genie_transport_send_payload(&transport_payload_param);
}

void report_vendor_func_packag(void)
{
    uint16_t index = 0;
    uint8_t payload[20];
    genie_transport_payload_param_t transport_payload_param;



    memset(&transport_payload_param, 0, sizeof(genie_transport_payload_param_t));
    transport_payload_param.opid = VENDOR_OP_ATTR_INDICATE;
    transport_payload_param.p_payload = payload;
    transport_payload_param.payload_len = index;
    transport_payload_param.retry_cnt = EVENT_REPORT_RETRY;

    genie_transport_send_payload(&transport_payload_param);

}

void report_vendor_func(uint32_t data,uint8_t len, uint16_t device_type)
{
    uint16_t index = 0;
    uint8_t payload[20];
    genie_transport_payload_param_t transport_payload_param;


    payload[index++] = device_type & 0xff;
    payload[index++] = (device_type >> 8) & 0xff;
    if(len == 1)
    {
        payload[index++] = data;   
    }
    else
    {
        payload[index++] = data & 0xff;
        payload[index++] = (data >> 8) & 0xff;
    }
    


    memset(&transport_payload_param, 0, sizeof(genie_transport_payload_param_t));
    transport_payload_param.opid = VENDOR_OP_ATTR_INDICATE;
    transport_payload_param.p_payload = payload;
    transport_payload_param.payload_len = index;
    transport_payload_param.retry_cnt = EVENT_REPORT_RETRY;

    genie_transport_send_payload(&transport_payload_param);
}


void report_vendor_func_tow(uint32_t data,uint8_t len, uint16_t device_type,uint32_t data2,uint8_t len2, uint16_t device_type2)
{
    uint16_t index = 0;
    uint8_t payload[20];
    genie_transport_payload_param_t transport_payload_param;


    payload[index++] = device_type & 0xff;
    payload[index++] = (device_type >> 8) & 0xff;
    if(len == 1)
    {
        payload[index++] = data;   
    }
    else
    {
        payload[index++] = data & 0xff;
        payload[index++] = (data >> 8) & 0xff;
    }

    payload[index++] = device_type2 & 0xff;
    payload[index++] = (device_type2 >> 8) & 0xff;
    if(len2 == 1)
    {
        payload[index++] = data2;   
    }
    else
    {
        payload[index++] = data2 & 0xff;
        payload[index++] = (data2 >> 8) & 0xff;
    }   
    


    memset(&transport_payload_param, 0, sizeof(genie_transport_payload_param_t));
    transport_payload_param.opid = VENDOR_OP_ATTR_INDICATE;
    transport_payload_param.p_payload = payload;
    transport_payload_param.payload_len = index;
    transport_payload_param.retry_cnt = EVENT_REPORT_RETRY;

    genie_transport_send_payload(&transport_payload_param);
}

static void light_ctl_event_handler(genie_event_e event, void *p_arg)
{
    switch (event)
    {
    case GENIE_EVT_SW_RESET:
    {
        light_param_reset();
        //light_led_blink(3, 1);
        aos_msleep(3000);
    } 
    break;
    case GENIE_EVT_MESH_READY:
    {
        //User can report data to cloud at here
        GENIE_LOG_INFO("User report data");

        light_report_poweron_state();
    }
    break;
    case GENIE_EVT_SDK_MESH_PROV_SUCCESS:
    {

        //light_led_blink(3, 0);
    }
    break;
#ifdef CONFIG_MESH_MODEL_TRANS
    case GENIE_EVT_USER_TRANS_CYCLE:
#endif
    case GENIE_EVT_USER_ACTION_DONE:
    {
        sig_model_element_state_t *p_elem = (sig_model_element_state_t *)p_arg;
        //light_update(p_elem);
        if (event == GENIE_EVT_USER_ACTION_DONE)
        {
            //light_save_state(p_elem);
        }
    }
    break;
    case GENIE_EVT_SIG_MODEL_MSG:
    {
        sig_model_msg *p_msg = (sig_model_msg *)p_arg;

        if (p_msg)
        {
            GENIE_LOG_INFO("SIG mesg ElemID(%d)", p_msg->element_id);
        }
    }
    break;
    case GENIE_EVT_VENDOR_MODEL_MSG:
    {
        genie_transport_model_param_t *p_msg = (genie_transport_model_param_t *)p_arg;

        if (p_msg && p_msg->p_model && p_msg->p_model->user_data)
        {
            sig_model_element_state_t *p_elem_state = (sig_model_element_state_t *)p_msg->p_model->user_data;
            GENIE_LOG_INFO("ElemID(%d) TID(%d)", p_elem_state->element_id, p_msg->tid);
        }

        //OGI("GENIE_EVT_VENDOR_MODEL_MSG","%x %x %x",p_msg->data[0],p_msg->data[1],p_msg->data[2]);

        uint16_t Vendor_Command = (p_msg->data[1] << 8) + p_msg->data[0];
        uint8_t *data = &p_msg->data[2];
        uint16_t op = 0;
        int32_t massagegunspeed;
        int32_t n;
        switch(Vendor_Command)
        {   
            
            default:
                break;
        }
    }
    break;
#ifdef CONIFG_GENIE_MESH_USER_CMD
    case GENIE_EVT_DOWN_MSG:
    {
        genie_down_msg_t *p_msg = (genie_down_msg_t *)p_arg;
        //User handle this msg,such as send to MCU
        if (p_msg)
        {
        }
    }
    break;
#endif
#ifdef MESH_MODEL_VENDOR_TIMER
    case GENIE_EVT_TIMEOUT:
    {
        vendor_attr_data_t *pdata = (vendor_attr_data_t *)p_arg;
        //User handle vendor timeout event at here
        if (pdata)
        {
            light_ctl_handle_order_msg(pdata);
        }
    }
    break;
#endif
    default:
        break;
    }
}

#ifdef CONFIG_PM_SLEEP
static void light_ctl_lpm_cb(genie_lpm_wakeup_reason_e reason, genie_lpm_status_e status, void *args)
{
    if (status == STATUS_WAKEUP)
    {
        GENIE_LOG_INFO("wakeup by %s", (reason == WAKEUP_BY_IO) ? "io" : "timer");
    }
    else
    {
        GENIE_LOG_INFO("sleep");
    }
}
#endif
wdg_dev_t g_dog;
static void light_init(void)
{
    //light_driver_init();
    LOGI("light_init","------");
    EN_init();
    vbus_init();
    my_led_init();
    my_adc_init();
    my_key_init();
    led_pwm_init();
    motor_pwm_init();
    
    g_dog.port = 0;
    g_dog.config.timeout = 5000;
    hal_wdg_init(&g_dog);
    int ret = csi_wdt_config_feed(g_dog.port, 1);
    if (ret != 0)
    {
        printf("wdg auto feed config error !\n");
    }
    csi_wdt_power_control(g_dog.priv, DRV_POWER_FULL);

    light_elem_state_init();
    k_timer_init(&light_param.led_blink_timer, _led_blink_timer_cb, NULL);
}

#ifdef CONFIG_BT_MESH_SHELL
static void handle_bt_mesh_cmd(char *pwbuf, int blen, int argc, char **argv)
{
    struct mesh_shell_cmd *mesh_cmds = NULL, *p;
    char *cmd_str;

    if (strcmp(argv[0], "bt-mesh") != 0)
    {
        return;
    }

    if (argc <= 1)
    {
        cmd_str = "help";
    }
    else
    {
        cmd_str = argv[1];
    }

    mesh_cmds = bt_mesh_get_shell_cmd_list();
    if (mesh_cmds)
    {
        p = mesh_cmds;
        while (p->cmd_name != NULL)
        {
            if (strcmp(p->cmd_name, cmd_str) != 0)
            {
                p++;
                continue;
            }
            if (p->cb)
            {
                p->cb(argc - 1, &(argv[1]));
                return;
            }
        }
        printf("cmd error\n");
    }
}

static struct cli_command ncmd[] = {
    {.name = "bt-mesh",
     .help = "bt-mesh [cmd] [options]",
     .function = handle_bt_mesh_cmd},
};
#endif

void UnBinding(void)
{
   genie_event(GENIE_EVT_HW_RESET_START, NULL);
}




bool bluetooth_is_gatt_connected()
{
    struct bt_conn *conn = bt_conn_lookup_state_le(NULL, 3);
    // //存在GATT连接
    if (conn)
    {
        bt_conn_unref(conn);
        return true;
    }
    else
    {
        return false;
    }
}

void user_uart_recv(void)
{
    uint8_t len;
    
	if(uart_it_index > 3)
	{
		len = uart_it_index;
        hal_uart_send(&uart_0,uart_it_buf,len,100);
		//uart_data_func(uart_it_buf, len);
		memset(&uart_it_buf,0,uart_it_index);
		uart_it_index=0;
	}

}
/*
static ktask_t tg7100_hdr;
static cpu_stack_t tg7100_stack[3600 / sizeof(cpu_stack_t)];
static void loop_task(void *arg)
{
    LOGI(tag, "board loop task start");

    static long long last_1500ms = 0;
    static long long last_200ms = 0; 
    static long long last_1000ms = 0;
    static long long last_5000ms = 0;
    static long long  last_20000ms = 0;
    static uint8_t cnt = 0;
    uint64_t now = 0;

    while (true)
    {
        user_uart_recv();
         aos_msleep(70); 
    }
}
*/
int application_start(int argc, char **argv)
{
    genie_service_ctx_t context;

    GENIE_LOG_INFO("BTIME:%s\n", __DATE__ ","__TIME__);

    light_init();

    memset(&context, 0, sizeof(genie_service_ctx_t));
    context.prov_timeout = MESH_PBADV_TIME;

    context.event_cb = light_ctl_event_handler;
    context.p_mesh_elem = light_elements;
    context.mesh_elem_counts = sizeof(light_elements) / sizeof(struct bt_mesh_elem);

#ifdef CONFIG_PM_SLEEP
    context.lpm_conf.is_auto_enable = 1;
    context.lpm_conf.lpm_wakeup_io = 14;
    context.lpm_conf.lpm_wakeup_msg = 1;

    context.lpm_conf.genie_lpm_cb = light_ctl_lpm_cb;
#ifndef CONFIG_GENIE_MESH_GLP
    //User can config sleep time and wakeup time when not config GLP
    context.lpm_conf.sleep_ms = 1000;
    context.lpm_conf.wakeup_ms = 30;
#endif
#endif

    genie_service_init(&context);

#ifdef CONFIG_BT_MESH_SHELL
    aos_cli_register_commands((const struct cli_command *)&ncmd, sizeof(ncmd) / sizeof(ncmd[0]));
#endif

/*
    return krhino_task_create(&tg7100_hdr, "tg7100", NULL,
                             30, 100, tg7100_stack,
                              sizeof(tg7100_stack) / sizeof(cpu_stack_t),
                              (task_entry_t)loop_task, 1);
*/


    


    aos_loop_run();

    return 0;
}


