/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_GLP)
#include "genie_mesh_internal.h"

static genie_lpm_ctx_t genie_lpm_ctx;

#ifndef GENIE_WAKE_UP_IO_NUM_MAX
#define GENIE_WAKE_UP_IO_NUM_MAX 4
#endif

static genie_lpm_wakeup_io_config_t g_wakeup_io[GENIE_WAKE_UP_IO_NUM_MAX];

static uint8_t g_wakeup_io_num = 0;

static void genie_lpm_delay_sleep_timer_irq_handler(void *p_timer, void *args)
{
    genie_lpm_enable(false);
}

static void genie_lpm_glp_timer_irq_handler(void *p_timer, void *args)
{
    int ret = 0;

    //GENIE_LOG_INFO("[%u]cur status: %d\n", k_uptime_get_32(), genie_lpm_ctx.status);

    if (genie_lpm_ctx.has_disabled == 1)
    {
        GENIE_LOG_INFO("lpm already disable, stop timer\n");
        aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
        return;
    }

    if (STATUS_WAKEUP == genie_lpm_ctx.status)
    {
        //GENIE_LOG_INFO("[%u]sleep and suspend mesh stack\n", k_uptime_get_32());
        ret = genie_mesh_suspend(false);
        if (0 == ret || -EALREADY == ret)
        {
            genie_lpm_ctx.status = STATUS_SLEEP;
            if (genie_lpm_ctx.p_config.genie_lpm_cb)
            {
                genie_lpm_ctx.p_config.genie_lpm_cb(WAKEUP_BY_TIMER, genie_lpm_ctx.status, NULL);
            }
            aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
            aos_timer_change_once(&genie_lpm_ctx.glp_wakeup_timer, genie_lpm_ctx.p_config.sleep_ms);
            aos_timer_start(&genie_lpm_ctx.glp_wakeup_timer);
        }
        else
        {
            aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
            aos_timer_change_once(&genie_lpm_ctx.glp_wakeup_timer, genie_lpm_ctx.p_config.wakeup_ms);
            aos_timer_start(&genie_lpm_ctx.glp_wakeup_timer);
        }
    }
    else if (STATUS_SLEEP == genie_lpm_ctx.status)
    {
        //GENIE_LOG_INFO("[%u]wake up and resume mesh stack\n", k_uptime_get_32());
        ret = genie_mesh_resume();
        if (0 == ret || -EALREADY == ret)
        {
            genie_lpm_ctx.status = STATUS_WAKEUP;
            genie_lpm_ctx.p_config.genie_lpm_cb(WAKEUP_BY_TIMER, genie_lpm_ctx.status, NULL);
            aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
            aos_timer_change_once(&genie_lpm_ctx.glp_wakeup_timer, genie_lpm_ctx.p_config.wakeup_ms);
            aos_timer_start(&genie_lpm_ctx.glp_wakeup_timer);
        }
        else
        {
            aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
            aos_timer_change_once(&genie_lpm_ctx.glp_wakeup_timer, genie_lpm_ctx.p_config.sleep_ms);
            aos_timer_start(&genie_lpm_ctx.glp_wakeup_timer);
        }
    }
}

static void _genie_lpm_io_wakeup_timer_handler(void *p_timer, void *args)
{
    bool status = false;
    uint8_t i = 0;
    uint8_t trigger_found = 0;
    genie_lpm_io_status_t _io_status[GENIE_WAKE_UP_IO_NUM_MAX];

    if (genie_lpm_ctx.p_config.lpm_wakeup_io == 0) // not support io wakeup
    {
        return;
    }

    memset(_io_status, 0, sizeof(_io_status));

    for (i = 0; i < g_wakeup_io_num; i++)
    {
        status = genie_sal_sleep_wakeup_io_get_status(g_wakeup_io[i].port);

        if (((g_wakeup_io[i].io_pol == FALLING || g_wakeup_io[i].io_pol == ACT_LOW) && status == false) || ((g_wakeup_io[i].io_pol == RISING || g_wakeup_io[i].io_pol == ACT_HIGH) && status == true))
        {
            _io_status[i].port = g_wakeup_io[i].port;
            _io_status[i].trigger_flag = 1;
            _io_status[i].status = status;
            trigger_found = 1;
        }
    }

    if (trigger_found)
    {
        GENIE_LOG_DBG("[%u]wakeup by i/o\n", k_uptime_get_32());
        GENIE_LOG_DBG("[%u]cur status: %d\n", k_uptime_get_32(), genie_lpm_ctx.status);
        if (STATUS_SLEEP == genie_lpm_ctx.status)
        {
            int ret = 0;
            GENIE_LOG_DBG("[%u]wake up and resume mesh stack\n", k_uptime_get_32());
            ret = genie_mesh_resume();
            if (0 == ret)
            {
                genie_lpm_ctx.status = STATUS_WAKEUP;
                genie_lpm_io_status_list_t list;
                list.size = g_wakeup_io_num;
                list.io_status = _io_status;
                if (genie_lpm_ctx.p_config.genie_lpm_cb)
                {
                    genie_lpm_ctx.p_config.genie_lpm_cb(WAKEUP_BY_IO, genie_lpm_ctx.status, &list);
                }
            }
        }
        else
        {
            if (genie_lpm_ctx.p_config.genie_lpm_cb)
            {
                genie_lpm_ctx.p_config.genie_lpm_cb(WAKEUP_IS_WAKEUP, genie_lpm_ctx.status, NULL);
            }
        }
    }
}

static __attribute__((section(".__sram.code"))) void genie_lpm_io_wakeup_handler(void *arg)
{
    aos_timer_stop(&genie_lpm_ctx.io_wakeup_timer);
    aos_timer_start(&genie_lpm_ctx.io_wakeup_timer);
}

static void _genie_lpm_io_wakeup_init(genie_lpm_wakeup_io_t config)
{
    if (config.io_list_size > GENIE_WAKE_UP_IO_NUM_MAX)
    {
        GENIE_LOG_ERR("Wakeup i/o num should no more than %d", GENIE_WAKE_UP_IO_NUM_MAX);
    }
    g_wakeup_io_num = GENIE_WAKE_UP_IO_NUM_MAX < config.io_list_size ? GENIE_WAKE_UP_IO_NUM_MAX : config.io_list_size;
    memcpy(g_wakeup_io, config.io_config, sizeof(genie_lpm_wakeup_io_config_t) * g_wakeup_io_num);

    for (int i = 0; i < g_wakeup_io_num; i++)
    {
        genie_sal_sleep_wakup_io_set(g_wakeup_io[i].port, g_wakeup_io[i].io_pol);
    }

    genie_sal_io_wakeup_cb_register(genie_lpm_io_wakeup_handler);
}

void _genie_lpm_io_wakeup_timer_init(void)
{
    aos_timer_new(&genie_lpm_ctx.io_wakeup_timer, _genie_lpm_io_wakeup_timer_handler, NULL, 1, 0);
    aos_timer_stop(&genie_lpm_ctx.io_wakeup_timer);
}

static void _genie_lpm_wakeup_timer_init(void)
{
    aos_timer_new(&genie_lpm_ctx.glp_wakeup_timer, genie_lpm_glp_timer_irq_handler, NULL, 10, 0);
    aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
}

bool genie_lpm_get_wakeup_io_status(uint8_t port)
{
    return genie_sal_sleep_wakeup_io_get_status(port) > 0 ? true : false;
}

static uint8_t msg_lpm_disable = 0;
static int _genie_lpm_msg_active(uint32_t sleep_ms)
{
    int ret = 0;

    if (msg_lpm_disable == 0)
    {
        ret = genie_lpm_disable();
        if (ret != 0)
        {
            GENIE_LOG_ERR("[%u]lpm disable failed(%d), sleep_ms:%d", k_uptime_get_32(), ret, sleep_ms);
            return ret;
        }
        msg_lpm_disable = 1;
    }

    aos_timer_stop(&genie_lpm_ctx.msg_wakeup_timer);
    aos_timer_change_once(&genie_lpm_ctx.msg_wakeup_timer, sleep_ms);
    aos_timer_start(&genie_lpm_ctx.msg_wakeup_timer);
    return ret;
}

static int32_t _genie_lpm_msg_sleep(uint32_t after_ms)
{
    if (msg_lpm_disable == 1)
    {
        aos_timer_stop(&genie_lpm_ctx.msg_wakeup_timer);
        aos_timer_change_once(&genie_lpm_ctx.msg_wakeup_timer, after_ms);
        aos_timer_start(&genie_lpm_ctx.msg_wakeup_timer);
    }
    return 0;
}

static void _genie_lpm_msg_timer_cb(void *p_timer, void *args)
{
    int32_t ret = 0;

    GENIE_LOG_INFO("[%u]enter sleep mode again", k_uptime_get_32());
    aos_timer_stop(&genie_lpm_ctx.msg_wakeup_timer);

    ret = genie_lpm_enable(false);
    if (ret != 0)
    {
        GENIE_LOG_ERR("[%u]lpm enable failed(%d)", k_uptime_get_32(), ret);
        aos_timer_change_once(&genie_lpm_ctx.msg_wakeup_timer, MSG_CHECK_AGAIN_TIME);
        aos_timer_start(&genie_lpm_ctx.msg_wakeup_timer);
        return;
    }
    msg_lpm_disable = 0;
}

static void _genie_lpm_msg_init(void)
{
    aos_timer_new(&genie_lpm_ctx.msg_wakeup_timer, _genie_lpm_msg_timer_cb, NULL, MSG_ACTIVE_TIME_DEFAULT, 0);
    aos_timer_stop(&genie_lpm_ctx.msg_wakeup_timer);
}

static void _genie_lpm_msg_status_ack(struct bt_mesh_elem *p_elem, uint8_t tid, uint16_t attr_type, uint16_t attr_time)
{
    uint16_t index = 0;
    uint8_t payload[20];
    genie_transport_model_param_t genie_transport_model_param;

    payload[index++] = attr_type & 0xff;
    payload[index++] = (attr_type >> 8) & 0xff;
    payload[index++] = attr_time & 0xff;
    payload[index++] = (attr_time >> 8) & 0xff;

    memset(&genie_transport_model_param, 0, sizeof(genie_transport_model_param_t));
    genie_transport_model_param.opid = VENDOR_OP_ATTR_STATUS;
    genie_transport_model_param.tid = tid;
    genie_transport_model_param.data = payload;
    genie_transport_model_param.len = index;
    if (p_elem != NULL)
    {
        genie_transport_model_param.p_elem = p_elem;
    }
    else
    {
        genie_transport_model_param.p_elem = genie_mesh_get_primary_element();
    }
    genie_transport_model_param.retry_period = 600;
    genie_transport_model_param.retry = 2;

    genie_transport_send_model(&genie_transport_model_param);
}

int genie_lpm_handle_model_mesg(genie_transport_model_param_t *p_msg)
{
    uint8_t *p_data = NULL;
    uint8_t opid = 0;
    uint8_t tid = 0;
    uint8_t is_lpm_msg = 0;

    if (genie_lpm_ctx.p_config.lpm_wakeup_msg == 0)
    {
        return is_lpm_msg;
    }

    if (!p_msg || !(p_msg->data))
    {
        return -1;
    }

    opid = p_msg->opid;
    tid = p_msg->tid;
    p_data = p_msg->data;

    switch (opid)
    {
    case VENDOR_OP_ATTR_SET_UNACK:
    case VENDOR_OP_ATTR_SET_ACK:
    {
        uint16_t attr_time = 0;
        uint16_t attr_type = *p_data++;
        attr_type += (*p_data++ << 8);
        if (attr_type == MSG_ATTR_TYPE_ACTIVE_WAKEUP)
        {
            if ((p_data != NULL) && ((p_data + 1) != NULL))
            {
                attr_time = ((p_data[0]) | (p_data[1] << 8)) * 1000;
                p_data += 2;
            }
            if (attr_time == 0)
            {
                attr_time = MSG_ACTIVE_TIME_DEFAULT;
            }
            if (attr_time < MSG_ACTIVE_TIME_MIN)
            {
                attr_time = MSG_ACTIVE_TIME_MIN;
            }
            GENIE_LOG_INFO("attr_type: 0x%04x, attr_time: %d", attr_type, attr_time);
            _genie_lpm_msg_active(attr_time);
            if (opid == VENDOR_OP_ATTR_SET_ACK)
            {
                _genie_lpm_msg_status_ack(p_msg->p_elem, tid, attr_type, attr_time);
            }
            is_lpm_msg = 1;
        }
        else if (attr_type == MSG_ATTR_TYPE_ENTER_SLEEP)
        {
            if ((p_data != NULL) && ((p_data + 1) != NULL))
            {
                attr_time = ((p_data[0]) | (p_data[1] << 8)) * 1000;
                p_data += 2;
            }
            if ((opid == VENDOR_OP_ATTR_SET_ACK) && (attr_time < MSG_ACK_AFTER_SLEEP_TIME))
            {
                attr_time = MSG_ACK_AFTER_SLEEP_TIME;
            }
            else if ((opid == VENDOR_OP_ATTR_SET_UNACK) && (attr_time < MSG_UNACK_AFTER_SLEEP_TIME))
            {
                attr_time = MSG_UNACK_AFTER_SLEEP_TIME;
            }
            GENIE_LOG_INFO("attr_type: 0x%04x, attr_time: %d", attr_type, attr_time);
            if (opid == VENDOR_OP_ATTR_SET_ACK)
            {
                _genie_lpm_msg_status_ack(p_msg->p_elem, tid, attr_type, attr_time);
            }
            _genie_lpm_msg_sleep(attr_time);
            is_lpm_msg = 1;
        }
        break;
    }
    default:
        break;
    }

    return is_lpm_msg;
}

int genie_lpm_disable(void)
{
    int ret = -1;

    GENIE_LOG_INFO("lpm disable");

    bt_mesh_gatt_user_enable();
    ret = genie_mesh_resume();

    if (0 == ret || -EALREADY == ret)
    {
        genie_lpm_ctx.has_disabled = 1;
        genie_lpm_ctx.status = STATUS_WAKEUP;
        if ((genie_lpm_ctx.p_config.sleep_ms != 0) && (genie_lpm_ctx.p_config.wakeup_ms != 0))
        {
            aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
        }
        genie_sal_sleep_disable();
    }

    return (ret == -EALREADY) ? 0 : ret;
}

int genie_lpm_enable(bool force)
{
    int ret = -1;

    GENIE_LOG_INFO("lpm enable");
    bt_conn_disconnect_all();
    bt_mesh_gatt_user_disable();
    
    ret = genie_mesh_suspend(force);

    GENIE_LOG_INFO("------ret:%d",ret);
    if (0 == ret || -EALREADY == ret)
    {
        genie_lpm_ctx.has_disabled = 0;
        genie_sal_sleep_enable();
        genie_lpm_ctx.status = STATUS_SLEEP;

        if ((genie_lpm_ctx.p_config.sleep_ms != 0) && (genie_lpm_ctx.p_config.wakeup_ms != 0))
        {
            if (force)
            {
                aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
                GENIE_LOG_INFO("------force:%d",force);
            }
            else
            {
                aos_timer_stop(&genie_lpm_ctx.delay_sleep_timer);
                aos_timer_free(&genie_lpm_ctx.delay_sleep_timer);
                aos_timer_stop(&genie_lpm_ctx.glp_wakeup_timer);
                aos_timer_change_once(&genie_lpm_ctx.glp_wakeup_timer, genie_lpm_ctx.p_config.sleep_ms);
                aos_timer_start(&genie_lpm_ctx.glp_wakeup_timer);
                
                GENIE_LOG_INFO("------force:%d",force);
                
            }
        }
    }
    else
    {
        aos_timer_stop(&genie_lpm_ctx.delay_sleep_timer);
        aos_timer_change(&genie_lpm_ctx.delay_sleep_timer, 5000);
        aos_timer_start(&genie_lpm_ctx.delay_sleep_timer);
    }

    return (ret == -EALREADY) ? 0 : ret;
}

int genie_lpm_deep_sleep(void)
{
    return genie_sal_sleep_enter_standby();
}

uint32_t genie_lpm_get_bootup_reason(void)
{
    return genie_sal_lpm_get_bootup_reason();
}

int32_t genie_lpm_set_bootup_reason(uint32_t reason)
{
    return genie_sal_lpm_set_bootup_reason(reason);
}

void genie_lpm_init(genie_lpm_conf_t *lpm_conf)
{
    memcpy(&genie_lpm_ctx.p_config, lpm_conf, sizeof(genie_lpm_conf_t));

    if (genie_lpm_ctx.p_config.genie_lpm_cb == NULL)
    {
        GENIE_LOG_ERR("lpm param err");
        return;
    }

    if (genie_lpm_ctx.p_config.delay_sleep_time == 0)
    {
        genie_lpm_ctx.p_config.delay_sleep_time = DEFAULT_BOOTUP_DELAY_SLEEP_TIME;
    }

    genie_lpm_ctx.status = STATUS_WAKEUP;

    printf("io_wakeup:%d, msg_wakeup:%d, sleep:%dms, wakeup:%dms, after:%ldms\n", genie_lpm_ctx.p_config.lpm_wakeup_io, genie_lpm_ctx.p_config.lpm_wakeup_msg,
           genie_lpm_ctx.p_config.sleep_ms, genie_lpm_ctx.p_config.wakeup_ms, genie_lpm_ctx.p_config.delay_sleep_time);

    if (genie_lpm_ctx.p_config.lpm_wakeup_io != 0 && genie_lpm_ctx.p_config.lpm_wakeup_io_config.io_list_size != 0)
    {
        _genie_lpm_io_wakeup_init(lpm_conf->lpm_wakeup_io_config);
        _genie_lpm_io_wakeup_timer_init(); // Do things in the timer when wakeup by IO
    }

    if ((genie_lpm_ctx.p_config.sleep_ms != 0) && (genie_lpm_ctx.p_config.wakeup_ms != 0))
    {
        _genie_lpm_wakeup_timer_init();
    }

    if (genie_lpm_ctx.p_config.delay_sleep_time > 0)
    {
        aos_timer_new(&genie_lpm_ctx.delay_sleep_timer, genie_lpm_delay_sleep_timer_irq_handler, NULL, genie_lpm_ctx.p_config.delay_sleep_time, 0);
        aos_timer_stop(&genie_lpm_ctx.delay_sleep_timer);
    }

    if (genie_lpm_ctx.p_config.lpm_wakeup_msg != 0)
    {
        _genie_lpm_msg_init();
    }
}

int genie_lpm_start(void)
{
    GENIE_LOG_DBG("genie lpm mesh ready");
    if (genie_lpm_ctx.p_config.is_auto_enable == 1)
    {
        printf("delay sleep timer start\n");
        if (genie_lpm_ctx.p_config.delay_sleep_time > 0)
        {
            aos_timer_start(&genie_lpm_ctx.delay_sleep_timer);
        }
        else
        {
            genie_lpm_enable(false);
        }
    }

    return 0;
}
