/*
 * Copyright (C) 2019-2020 Alibaba Group Holding Limited
 */

#include "genie_sal.h"
#include "genie_mesh_api.h"

#include "switches.h"
#include "switches_queue.h"
#include "switches_input.h"

/* element configuration start */
static sig_model_element_state_t switches_state[ELEMENT_NUM];
static sig_model_powerup_t switches_powerup[ELEMENT_NUM];

static struct bt_mesh_model primary_element[] = {
    BT_MESH_MODEL_CFG_SRV(),
    BT_MESH_MODEL_HEALTH_SRV(),

    MESH_MODEL_GEN_ONOFF_SRV(&switches_state[0]),
};

static struct bt_mesh_model primary_vendor_element[] = {
    MESH_MODEL_VENDOR_SRV(&switches_state[0]),
};

static struct bt_mesh_model secondary_element[] = {
    MESH_MODEL_GEN_ONOFF_SRV(&switches_state[1]),
};

static struct bt_mesh_model secondary_vendor_element[] = {
    MESH_MODEL_VENDOR_SRV(&switches_state[1]),
};

static struct bt_mesh_model third_element[] = {
    MESH_MODEL_GEN_ONOFF_SRV(&switches_state[2]),
};

static struct bt_mesh_model third_vendor_element[] = {
    MESH_MODEL_VENDOR_SRV(&switches_state[2]),
};

static struct bt_mesh_model fourth_element[] = {
    MESH_MODEL_GEN_ONOFF_SRV(&switches_state[3]),
};

static struct bt_mesh_model fourth_vendor_element[] = {
    MESH_MODEL_VENDOR_SRV(&switches_state[3]),
};

struct bt_mesh_elem switches_elements[] = {
    BT_MESH_ELEM(0, primary_element, primary_vendor_element, GENIE_ADDR_SWITCH),
    BT_MESH_ELEM(0, secondary_element, secondary_vendor_element, GENIE_ADDR_SWITCH),
    BT_MESH_ELEM(0, third_element, third_vendor_element, GENIE_ADDR_SWITCH),
    BT_MESH_ELEM(0, fourth_element, fourth_vendor_element, GENIE_ADDR_SWITCH),
};
/* element configuration end */

#ifdef CONFIG_GENIE_OTA
bool genie_sal_ota_is_allow_reboot(void)
{
    GENIE_LOG_INFO("Not allow to reboot!");
    return false;
}
#endif

static void switches_param_reset(void)
{
    genie_storage_delete_userdata(GFI_MESH_POWERUP);
}

static void switches_save_state(sig_model_element_state_t *p_elem)
{
    uint8_t *p_read = NULL;

#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
    p_elem->powerup.last_onoff = p_elem->state.onoff[TYPE_PRESENT];
    switches_powerup[p_elem->element_id].last_onoff = p_elem->state.onoff[TYPE_PRESENT];
#endif

    p_read = aos_malloc(sizeof(switches_powerup));
    if (!p_read)
    {
        GENIE_LOG_WARN("no mem");
        return;
    }

    genie_storage_read_userdata(GFI_MESH_POWERUP, p_read, sizeof(switches_powerup));
    GENIE_LOG_INFO("element_id:%d", p_elem->element_id,switches_powerup[p_elem->element_id].last_onoff);
    if (memcmp(switches_powerup, p_read, sizeof(switches_powerup)))
    {
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
        GENIE_LOG_INFO("save %d", switches_powerup[p_elem->element_id].last_onoff);
#endif
        genie_storage_write_userdata(GFI_MESH_POWERUP, (uint8_t *)switches_powerup, sizeof(switches_powerup));

        switch_output_gpio_set(p_elem->element_id, switches_powerup[p_elem->element_id].last_onoff);
    }
    
    aos_free(p_read);
}

void save_switches_param(void)
{
    uint8_t n = 0;
    for(n = 0; n < ELEMENT_NUM; n++)
    {
        switches_save_state(&switches_state[n]);        
    }

}

static void switches_update(uint8_t onoff)
{
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
    GENIE_LOG_INFO("Switch:%s", (onoff != 0) ? "on" : "off");
#endif
}

static void switches_report_poweron_state(int elem_index)
{
    uint16_t index = 0;
    uint8_t payload[20];
    genie_transport_model_param_t genie_transport_model_param;

#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
    payload[index++] = ATTR_TYPE_GENERIC_ONOFF & 0xff;
    payload[index++] = (ATTR_TYPE_GENERIC_ONOFF >> 8) & 0xff;
    payload[index++] = switches_state[elem_index].state.onoff[TYPE_PRESENT];
#endif

    memset(&genie_transport_model_param, 0, sizeof(genie_transport_model_param_t));
    genie_transport_model_param.opid = VENDOR_OP_ATTR_INDICATE;
    genie_transport_model_param.data = payload;
    genie_transport_model_param.len = index;
    genie_transport_model_param.p_elem = &switches_elements[elem_index];
    genie_transport_model_param.retry_period = EVENT_REPORT_INTERVAL;
    genie_transport_model_param.retry = EVENT_REPORT_RETRY;

    genie_transport_send_model(&genie_transport_model_param);
}

#ifdef MESH_MODEL_VENDOR_TIMER
static void switches_handle_order_msg(vendor_attr_data_t *attr_data)
{
    GENIE_LOG_INFO("type:%04x data:%04x\r\n", attr_data->type, attr_data->para);

    if (attr_data->type == ATTR_TYPE_GENERIC_ONOFF)
    {
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
        switches_state[0].state.onoff[TYPE_TARGET] = attr_data->para;

        if (switches_state[0].state.onoff[TYPE_PRESENT] != switches_state[0].state.onoff[TYPE_TARGET])
        {
            switches_update(switches_state[0].state.onoff[TYPE_TARGET]);
            switches_state[0].state.onoff[TYPE_PRESENT] = switches_state[0].state.onoff[TYPE_TARGET];
        }
#endif
    }
}
#endif

static void switches_event_handler(genie_event_e event, void *p_arg)
{
    int index = 0;

    switch (event)
    {
    case GENIE_EVT_SW_RESET:
    {
        switches_param_reset();
    }
    break;
    case GENIE_EVT_MESH_READY:
    {
        //User can report data to cloud at here
        GENIE_LOG_INFO("User report data");
        for (index = 0; index < ELEMENT_NUM; index++)
        {
            switches_report_poweron_state(index);
        }
    }
    break;
    case GENIE_EVT_USER_ACTION_DONE:
    {
        sig_model_element_state_t *p_elem = (sig_model_element_state_t *)p_arg;

        if (p_elem)
        {
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
            switches_update(p_elem->state.onoff[TYPE_TARGET]);
#endif
            switches_save_state(p_elem);
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
    }
    break;

#ifdef CONIFG_GENIE_MESH_USER_CMD
    case GENIE_EVT_DOWN_MSG:
    {
        genie_down_msg_t *p_msg = (genie_down_msg_t *)p_arg;
        //User handle this msg,such as send to MCU
        if (p_msg)
        {
            GENIE_LOG_INFO("GENIE_EVT_DOWN_MSG");
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
            switches_handle_order_msg(pdata);
            
        }
    }
    break;
#endif
    default:
    {
    }
    break;
    }
}

static int switches_report_cb(void *p_params, transport_result_e result_e)
{
    genie_transport_model_param_t *p_transport_model_param = NULL;

    p_transport_model_param = (genie_transport_model_param_t *)p_params;

    if (result_e == SEND_RESULT_TIMEOUT)
    {
        GENIE_LOG_WARN("switches elem id(%d) send timeout", bt_mesh_elem_find_id(p_transport_model_param->p_elem));
    }

    return 0;
}

static void report_event_to_cloud(uint8_t element_id, uint8_t onoff)
{
    uint8_t payload[3];
    genie_transport_payload_param_t transport_payload_param;

    payload[0] = ATTR_TYPE_GENERIC_ONOFF & 0xFF;
    payload[1] = (ATTR_TYPE_GENERIC_ONOFF >> 8) & 0xFF;
    payload[2] = onoff;

    memset(&transport_payload_param, 0, sizeof(genie_transport_payload_param_t));
    transport_payload_param.element_id = element_id;
    transport_payload_param.opid = VENDOR_OP_ATTR_INDICATE;
    transport_payload_param.p_payload = payload;
    transport_payload_param.payload_len = sizeof(payload);
    transport_payload_param.retry_cnt = EVENT_REPORT_RETRY;
    transport_payload_param.result_cb = switches_report_cb;

    genie_transport_send_payload(&transport_payload_param);
}

static void handle_input_event(uint8_t element_id, switch_press_type_e press_type)
{
    switch (press_type)
    {
    case SWITCH_PRESS_PROVISION:
    {
    }
    break;
    case SWITCH_PRESS_ONCE:
    {
        uint8_t onoff = switches_state[element_id].state.onoff[TYPE_PRESENT];
        if (onoff == 0)
        {
            onoff = 1;
        }
        else if (onoff == 1)
        {
            onoff = 0;
        }
        switches_state[element_id].state.onoff[TYPE_TARGET] = onoff;
        switches_state[element_id].state.onoff[TYPE_PRESENT] = switches_state[element_id].state.onoff[TYPE_TARGET];

        //GENIE_LOG_INFO("element_id:%d, onoff:%d",element_id,onoff);

        //switch_output_gpio_set(element_id, onoff); //switch on off here

        report_event_to_cloud(element_id, onoff);
        switches_save_state(&switches_state[element_id]);
    }
    break;
    case SWITCH_PRESS_DOUBLE:
    {
    }
    break;
    case SWITCH_PRESS_LONG:
    {
        UnBinding();
#ifdef CONFIG_PM_SLEEP
    /*
        static bool sleep_toggle_flag = false;
        if (sleep_toggle_flag == true)
        {
            genie_lpm_disable();
            sleep_toggle_flag = false;
            GENIE_LOG_DBG("genie_lpm_disable");
        }
        else if (sleep_toggle_flag == false)
        {
            genie_lpm_enable(false);
            sleep_toggle_flag = true;
            GENIE_LOG_DBG("genie_lpm_enable");
        }
    */
#endif
    }
    break;
    case SWITCH_PRESS_TRIPLE:
    {
    }
    break;
    case SWITCH_PRESS_RESET:
    {
    }
    break;
    case SWITCH_PRESS_STANDBY:
    {
    }
    break;
    case SWITCH_PRESS_PRESSED:
    {
    }
    break;
    case SWITCH_PRESS_RELEASED:
    {
    }
    break;
    default:
        break;
    }
}
void UnBinding(void)
{
   genie_event(GENIE_EVT_HW_RESET_START, NULL);
}

static uint8_t switches_statee_init(uint8_t state_count, sig_model_element_state_t *p_elem)
{
    uint8_t index = 0;

    while (index < state_count)
    {
        p_elem[index].element_id = index;
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
        p_elem[index].state.onoff[TYPE_TARGET] = GEN_ONOFF_DEFAULT;
        p_elem[index].state.onoff[TYPE_PRESENT] = GEN_ONOFF_DEFAULT;
#endif
        index++;
    }

    return 0;
}

static int powerup_init(void)
{
    uint8_t index = 0;
    genie_storage_status_e ret;

    // init element state
    memset(switches_state, 0, sizeof(switches_state));
    switches_statee_init(ELEMENT_NUM, switches_state);//默认值初始

    ret = genie_storage_read_userdata(GFI_MESH_POWERUP, (uint8_t *)switches_powerup, sizeof(switches_powerup));
    //GENIE_LOG_INFO("ret:%d", ret);
    if (ret == GENIE_STORAGE_SUCCESS)
    {

        while (index < ELEMENT_NUM)
        {
            memcpy(&switches_state[index].powerup, &switches_powerup[index], sizeof(sig_model_powerup_t));
#ifdef CONFIG_MESH_MODEL_GEN_ONOFF_SRV
            switches_state[index].state.onoff[TYPE_TARGET] = switches_powerup[index].last_onoff;
            switches_state[index].state.onoff[TYPE_PRESENT] = switches_powerup[index].last_onoff;
#endif
            switch_output_gpio_set(index, switches_powerup[index].last_onoff);
            index++;
        }
    }
    else
    {
        while (index < ELEMENT_NUM)
        {
            switch_output_gpio_set(index, switches_state[index].state.onoff[TYPE_TARGET]); 
            index++;
        }   
    }

    return ret;
}
static void switches_init(void)
{
    switches_output_gpio_init();
    powerup_init();
    input_event_init();
    queue_init();
}

#ifdef CONFIG_PM_SLEEP
static void switches_lpm_cb(genie_lpm_wakeup_reason_e reason, genie_lpm_status_e status, void *args)
{
    if (status == STATUS_WAKEUP)
    {
        if (reason == WAKEUP_BY_IO)
        {
            genie_lpm_io_status_list_t *list = (genie_lpm_io_status_list_t *)args;
            for (int i = 0; i < list->size; i++)
            {
                if (list->io_status[i].trigger_flag == true)
                {
                    input_event_start_check_timer(list->io_status[i].port);
                }
            }
        }
    }
    else
    {
        GENIE_LOG_INFO("sleep");
    }
}
#endif


int application_start(int argc, char **argv)
{
    queue_mesg_t queue_mesg;
    switch_press_type_e press_type;
    genie_service_ctx_t context;

    GENIE_LOG_INFO("BTIME:%s\n", __DATE__ ","__TIME__);

    switches_init();

    memset(&queue_mesg, 0, sizeof(queue_mesg_t));
    memset(&context, 0, sizeof(genie_service_ctx_t));
    context.prov_timeout = MESH_PBADV_TIME;
    context.event_cb = switches_event_handler;
    context.p_mesh_elem = switches_elements;
    context.mesh_elem_counts = sizeof(switches_elements) / sizeof(struct bt_mesh_elem);

#ifdef CONFIG_PM_SLEEP
    genie_lpm_wakeup_io_config_t io[] = {GENIE_WAKEUP_PIN(INPUT_EVENT_PIN_1, INPUT_PIN_POL_PIN_1),
                                         GENIE_WAKEUP_PIN(INPUT_EVENT_PIN_2, INPUT_PIN_POL_PIN_2)};

    genie_lpm_wakeup_io_t io_config = {sizeof(io) / sizeof(io[0]), io};
    context.lpm_conf.is_auto_enable = 1;
    context.lpm_conf.lpm_wakeup_io = 1;
    context.lpm_conf.lpm_wakeup_io_config = io_config;

    context.lpm_conf.genie_lpm_cb = switches_lpm_cb;
#endif

    if (GENIE_SERVICE_SUCCESS != genie_service_init(&context))
    {
        //Maybe there is no triple info
        GENIE_LOG_INFO("genie_service_init fail");
    }


    while (1)
    {
        if (queue_recv_data(&queue_mesg) < 0)
        {
            continue;
        }

        if (QUEUE_MESG_TYPE_INPUT == queue_mesg.type)
        {
            press_type = (switch_press_type_e)queue_mesg.data;

            handle_input_event(queue_mesg.element_id, press_type);
        }
    }

    return 0;
}
