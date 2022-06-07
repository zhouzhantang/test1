/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#include "genie_mesh_internal.h"

extern uint8_t g_mesh_log_mode;

static bool send_mesg_working = false;
static aos_timer_t mesg_send_timer;

#define MAC_LEN (6)
#define TIMER_SEND_MODE (255)
#define TIMEOUT_SEND_MODE (254)
#define USER_TIMER_SEND_MODE (253)

static void _get_triple(char *pwbuf, int blen, int argc, char **argv)
{
    uint8_t i;
    genie_triple_t genie_triple;
    genie_storage_status_e ret;

    memset(&genie_triple, 0, sizeof(genie_triple_t));

    ret = genie_triple_read(&genie_triple.pid, genie_triple.mac, genie_triple.key);
    if (ret != GENIE_STORAGE_SUCCESS)
    {
        GENIE_LOG_ERR("read triple fail(%d)", ret);
        return;
    }

    printk("%d ", (unsigned int)genie_triple.pid);

    for (i = 0; i < 16; i++)
    {
        printk("%02x", genie_triple.key[i]);
    }

    printk(" ");
    for (i = 0; i < 6; i++)
    {
        printk("%02x", genie_triple.mac[i]);
    }
    printk("\n");
}

static int genie_cli_set_triple(char *pwbuf, int blen, int argc, char **argv)
{
    uint32_t pid;
    uint8_t mac[MAC_LEN], read_mac[MAC_LEN];
    uint8_t key[16];
    uint8_t ret;

    if (argc != 4)
    {
        GENIE_LOG_ERR("para err");
        return -1;
    }

    //pro_id
    pid = atol(argv[1]);
    if (pid == 0)
    {
        GENIE_LOG_ERR("pid err");
        return -1;
    }
    //key
    if (strlen(argv[2]) != 32)
    {
        GENIE_LOG_ERR("key len err");
        return -1;
    }
    ret = stringtohex(argv[2], key, 16);
    if (ret == 0)
    {
        GENIE_LOG_ERR("key format err");
        return -1;
    }

    //addr
    if (strlen(argv[3]) != 12)
    {
        GENIE_LOG_ERR("mac len err");
        return -1;
    }
    ret = stringtohex(argv[3], mac, MAC_LEN);
    if (ret == 0)
    {
        GENIE_LOG_ERR("mac format err");
        return -1;
    }

    hal_flash_write_mac_params(mac, MAC_LEN);
    memset(read_mac, 0xFF, MAC_LEN);
    hal_flash_read_mac_params(read_mac, MAC_LEN);
    if (memcmp(mac, read_mac, MAC_LEN))
    {
        GENIE_LOG_ERR("write mac fail");
        return -1;
    }

    ret = genie_triple_write(&pid, mac, key);

    return ret;
}

static void _set_triple(char *pwbuf, int blen, int argc, char **argv)
{
    genie_cli_set_triple(pwbuf, blen, argc, argv);
    _get_triple(pwbuf, blen, argc, argv);
}

static void freq_offset_handle(char *pwbuf, int blen, int argc, char **argv)
{
    int freq_offset = 0;

    if (!strncmp(argv[1], "set", 3))
    {
        freq_offset = atoi((char *)(argv[2]));
        if (freq_offset == 0 && strncmp(argv[2], "0", 1))
        {
            GENIE_LOG_ERR("input int value[-200,200]");
            return;
        }

        if (freq_offset < -200 || freq_offset > 200)
        {
            GENIE_LOG_ERR("input range [-200,200]");
            return;
        }

        //Convert for match TG7100B
        if (freq_offset % 10 == freq_offset % 20)
        {
            freq_offset = freq_offset - freq_offset % 10;
        }
        else
        {
            freq_offset = freq_offset - freq_offset % 10;
            if (freq_offset < 0)
            {
                freq_offset -= 10;
            }
            else
            {
                freq_offset += 10;
            }
        }

        freq_offset >>= 2;

        if (hal_flash_write_freq_params(&freq_offset, sizeof(freq_offset)) != 0)
        {
            GENIE_LOG_ERR("write freq failed");
            return;
        }
    }
    else if (!strncmp(argv[1], "get", 3))
    {
        hal_flash_read_freq_params(&freq_offset, sizeof(freq_offset));
        if ((freq_offset & 0xFF) == 0xFF)
        {
            GENIE_LOG_INFO("default freq offset:%d", -80); //RF_PHY_FREQ_FOFF_N80KHZ
        }
        else
        {
            GENIE_LOG_INFO("freq offset:%d", freq_offset * 4);
        }
    }
}

static void sn_number_handle(char *pwbuf, int blen, int argc, char **argv)
{
    int32_t ret = 0;
    uint8_t count = 0;
    uint8_t read_sn[24];

    if (!strncmp(argv[1], "set", 3))
    {
        if (argc != 3)
        {
            GENIE_LOG_ERR("param err");
            return;
        }
        count = strlen(argv[2]);
        if (count != CUSTOM_SN_LEN)
        {
            GENIE_LOG_ERR("sn len error");
            return;
        }
        ret = hal_flash_write_sn_params(argv[2], count);
        if (ret != 0)
        {
            GENIE_LOG_ERR("write sn failed(%d)", ret);
            return;
        }
        memset(read_sn, 0, sizeof(read_sn));
        hal_flash_read_sn_params(read_sn, count);
        if (memcmp(argv[2], read_sn, count))
        {
            GENIE_LOG_ERR("write sn failed");
            return;
        }
    }
    else if (!strncmp(argv[1], "get", 3))
    {
        memset(read_sn, 0, sizeof(read_sn));
        ret = hal_flash_read_sn_params(read_sn, CUSTOM_SN_LEN);
        if (ret != 0)
        {
            GENIE_LOG_ERR("read sn failed(%d)", ret);
            return;
        }
        if ((read_sn[0] & 0xFF) == 0xFF)
        {
            GENIE_LOG_INFO("SN:");
            return;
        }
        GENIE_LOG_INFO("SN:%s", read_sn);
    }
}

static void _reboot_handle(char *pwbuf, int blen, int argc, char **argv)
{
    aos_reboot();
}

static void genie_cli_print_sysinfo(void)
{
    uint8_t write_mac[MAC_LEN];
    char appver[16];

    genie_version_appver_string_get(appver, 16);

    printf("\r\nDEVICE:%s\r\n", CONFIG_BT_DEVICE_NAME);
    printf("APP VER:%s\r\n", appver);
    printf("GenieSDK:V%s\r\n", genie_version_sysver_get());
    printf("PROUDUCT:%s\r\n", SYSINFO_PRODUCT_MODEL);
    memset(write_mac, 0xFF, MAC_LEN);
    hal_flash_read_mac_params(write_mac, MAC_LEN);
    printf("MAC:%02X:%02X:%02X:%02X:%02X:%02X\r\n", write_mac[0], write_mac[1], write_mac[2], write_mac[3], write_mac[4], write_mac[5]);
}

static void _get_sw_info(char *pwbuf, int blen, int argc, char **argv)
{
    genie_cli_print_sysinfo();
}

extern uint32_t dump_mm_info_used(void);
static void _get_mm_info(char *pwbuf, int blen, int argc, char **argv)
{
#if RHINO_CONFIG_MM_DEBUG
    dump_mm_info_used();
#endif
}

static uint8_t retry_mode = 0;
static uint8_t *p_payload = NULL;
static genie_transport_payload_param_t *p_transport_payload_param = NULL;

static void mesg_send_timer_cb(void *time, void *args)
{
    genie_transport_payload_param_t *p_transport_payload_param = (genie_transport_payload_param_t *)args;
    if (p_transport_payload_param == NULL || send_mesg_working == false)
    {
        aos_timer_stop(&mesg_send_timer);
        if (p_payload)
        {
            hal_free(p_payload);
            p_payload = NULL;
        }
        if (p_transport_payload_param)
        {
            hal_free(p_transport_payload_param);
            p_transport_payload_param = NULL;
        }

        GENIE_LOG_INFO("stop send timer");
        return;
    }

    genie_transport_send_payload(p_transport_payload_param);
}

static int send_mesg_cb(void *p_params, transport_result_e result_e)
{
    GENIE_LOG_INFO("%s(%p)", result_e == 0 ? "success" : "timeout", p_transport_payload_param);

    if (retry_mode == TIMEOUT_SEND_MODE) //This is timeout send mode
    {
        if (mesg_send_timer.hdl == NULL)
        {
            aos_timer_new(&mesg_send_timer, mesg_send_timer_cb, p_transport_payload_param, 10, 0);
        }
        aos_timer_stop(&mesg_send_timer);
        aos_timer_start(&mesg_send_timer);
    }

    return 0;
}

static void _send_msg(char *pwbuf, int blen, int argc, char **argv)
{
    uint8_t count;
    uint8_t ret = 0;
    uint8_t opid = 0;

    if (argc == 2 && !strncmp(argv[1], "stop", 4))
    {
        send_mesg_working = false;
        GENIE_LOG_INFO("send stop");
        return;
    }

    if (argc != 5)
    {
        GENIE_LOG_ERR("param err");
        return;
    }

    if (strlen(argv[3]) != 4)
    {
        GENIE_LOG_ERR("addr len error");
        return;
    }

    if (stringtohex(argv[1], &opid, 1) > 0)
    {
        retry_mode = atoi(argv[2]);
        count = strlen(argv[4]) >> 1;

        if (p_payload == NULL)
        {
            p_payload = hal_malloc(count);
            if (p_payload == NULL)
            {
                GENIE_LOG_ERR("malloc(%d) fail", count);
                return;
            }
        }
        else //remalloc
        {
            hal_free(p_payload);
            p_payload = NULL;
            p_payload = hal_malloc(count);
            if (p_payload == NULL)
            {
                GENIE_LOG_ERR("malloc(%d) fail", count);
                return;
            }
        }

        ret = stringtohex(argv[4], p_payload, count);
        if (ret == 0)
        {
            hal_free(p_payload);
            p_payload = NULL;
            return;
        }

        if (p_transport_payload_param == NULL)
        {
            p_transport_payload_param = hal_malloc(sizeof(genie_transport_payload_param_t));
            if (p_transport_payload_param == NULL)
            {
                hal_free(p_payload);
                p_payload = NULL;
                GENIE_LOG_ERR("malloc(%d) fail", sizeof(genie_transport_payload_param_t));
                return;
            }
        }

        memset(p_transport_payload_param, 0, sizeof(genie_transport_payload_param_t));
        p_transport_payload_param->opid = opid;
        p_transport_payload_param->p_payload = p_payload;
        p_transport_payload_param->payload_len = count;
        p_transport_payload_param->retry_cnt = retry_mode;
        p_transport_payload_param->result_cb = send_mesg_cb;

        ret = stringtohex(argv[3], (uint8_t *)&p_transport_payload_param->dst_addr, 2);
        if (ret == 0)
        {
            hal_free(p_payload);
            p_payload = NULL;
            hal_free(p_transport_payload_param);
            p_transport_payload_param = NULL;
            return;
        }

        //swap addr
        uint8_t upper = p_transport_payload_param->dst_addr >> 8;
        p_transport_payload_param->dst_addr = upper | (p_transport_payload_param->dst_addr << 8);

        if (retry_mode == TIMER_SEND_MODE) //This is auto send mode
        {
            send_mesg_working = true;

            p_transport_payload_param->retry_cnt = 0;
            if (mesg_send_timer.hdl == NULL)
            {
                aos_timer_new(&mesg_send_timer, mesg_send_timer_cb, p_transport_payload_param, 1000, 1);
            }
            aos_timer_stop(&mesg_send_timer);
            aos_timer_start(&mesg_send_timer);
            return;
        }
        else if (retry_mode == USER_TIMER_SEND_MODE && p_transport_payload_param->payload_len > 1) //This is auto send mode
        {
            send_mesg_working = true;

            p_transport_payload_param->retry_cnt = 0;
            if (mesg_send_timer.hdl == NULL)
            {
                int interval = p_transport_payload_param->p_payload[0];
                if (interval == 0)
                {
                    interval = 1;
                }

                interval = interval * 100; //Unit is 100ms
                //skip p_transport_payload_param->p_payload[0]
                p_transport_payload_param->p_payload = &p_transport_payload_param->p_payload[1];
                p_transport_payload_param->payload_len -= 1;
                aos_timer_new(&mesg_send_timer, mesg_send_timer_cb, p_transport_payload_param, interval, 1);
            }
            aos_timer_stop(&mesg_send_timer);
            aos_timer_start(&mesg_send_timer);
            return;
        }
        else if (retry_mode == TIMEOUT_SEND_MODE) //This is timeout send mode
        {
            send_mesg_working = true;
            p_transport_payload_param->retry_cnt = 0;
            genie_transport_send_payload(p_transport_payload_param);
            return;
        }

        genie_transport_send_payload(p_transport_payload_param);

        if (p_payload)
        {
            hal_free(p_payload);
            p_payload = NULL;
        }

        if (p_transport_payload_param)
        {
            hal_free(p_transport_payload_param);
            p_transport_payload_param = NULL;
        }
    }
}

static void genie_system_reset(char *pwbuf, int blen, int argc, char **argv)
{
    genie_event(GENIE_EVT_HW_RESET_START, NULL);
}

static void genie_log_onoff(char *pwbuf, int blen, int argc, char **argv)
{
    if (argc != 2)
    {
        GENIE_LOG_ERR("para err");
        return;
    }

    if (!strncmp(argv[1], "on", 2))
    {
        g_mesh_log_mode = 1;
    }
    else if (!strncmp(argv[1], "off", 3))
    {
        g_mesh_log_mode = 0;
    }
    else
    {
        GENIE_LOG_ERR("para err");
        return;
    }

    return;
}

/*[Genie begin] add by wenbing.cwb at 2021-04-29*/
#ifdef CONFIG_BT_MESH_CTRL_RELAY
static void genie_cr_log_onoff(char *pwbuf, int blen, int argc, char **argv)
{
    if (argc != 2)
    {
        GENIE_LOG_ERR("para err");
        return;
    }

    if (!strncmp(argv[1], "on", 2))
    {
        ctrl_relay_log_set(CTRL_RELAY_LOG_ON);
    }
    else if (!strncmp(argv[1], "off", 3))
    {
        ctrl_relay_log_set(CTRL_RELAY_LOG_OFF);
    }
    else
    {
        GENIE_LOG_ERR("para err");
    }
    return;
}
#endif
/*[Genie end] add by wenbing.cwb at 2021-04-29*/

static const struct cli_command genie_cmds[] = {
    {"get_tt", "get tri truple", _get_triple},
    {"set_tt", "set_tt pid key mac", _set_triple},
    {"freq", "freq set -80|freq get", freq_offset_handle},
    {"sn", "sn set <value>|sn get", sn_number_handle},
    {"reboot", "reboot", _reboot_handle},
    {"reset", "reset system", genie_system_reset},
    {"log", "log on|off", genie_log_onoff},
/*[Genie begin] add by wenbing.cwb at 2021-04-29*/
#ifdef CONFIG_BT_MESH_CTRL_RELAY
    {"cr_log", "cr_log on|off", genie_cr_log_onoff},
#endif
/*[Genie end] add by wenbing.cwb at 2021-04-29*/
    {"get_info", "get sw info", _get_sw_info},
    {"mm_info", "get mm info", _get_mm_info},
    {"mesg", "mesg d4 1 f000 010203", _send_msg},
};

void genie_cli_init(void)
{
#ifdef CONFIG_AOS_CLI
    aos_cli_register_commands(&genie_cmds[0], sizeof(genie_cmds) / sizeof(genie_cmds[0]));
#endif

    genie_cli_print_sysinfo();
}
