/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#include "genie_mesh_internal.h"
uint8_t g_bootload_flash_info[64]={0x11,0x22,0x33,0x44,0x55,0x66,0xab,0xcd,0xef,'t','h','i','s',' ','f','l','a','s','h',' ','r','e','s','e','r','v','e','d'};

static genie_service_ctx_t genie_service_ctx;

static int genie_service_check_ctx(genie_service_ctx_t *p_ctx)
{
    if (!p_ctx || !p_ctx->event_cb || !p_ctx->p_mesh_elem || p_ctx->mesh_elem_counts < 1)
    {
        GENIE_LOG_ERR("ctx:%p %p %p %d", p_ctx, p_ctx->event_cb, p_ctx->p_mesh_elem, p_ctx->mesh_elem_counts);
        return -1;
    }

    memset(&genie_service_ctx, 0, sizeof(genie_service_ctx_t));
    memcpy(&genie_service_ctx, p_ctx, sizeof(genie_service_ctx_t));

    return 0;
}

genie_service_ctx_t *genie_service_get_context(void)
{
    return &genie_service_ctx;
}

int genie_service_init(genie_service_ctx_t *p_ctx)
{
    int ret = 0;

    if (genie_service_check_ctx(p_ctx) < 0)
    {
        GENIE_LOG_ERR("genie service context error");
        return GENIE_SERVICE_ERRCODE_USER_INPUT;
    }

    genie_storage_init();

    genie_cli_init();

#if defined(CONIFG_GENIE_MESH_BINARY_CMD) || defined(CONFIG_GENIE_MESH_AT_CMD)
    genie_sal_uart_init();
#endif

    genie_reset_init();

    genie_transport_init();

#ifdef CONFIG_GENIE_MESH_GLP
    p_ctx->lpm_conf.sleep_ms = GENIE_GLP_SLEEP_TIME;
    p_ctx->lpm_conf.wakeup_ms = GENIE_GLP_WAKEUP_TIME;
#endif

#ifdef CONFIG_PM_SLEEP
    genie_lpm_init(&p_ctx->lpm_conf);
#endif

    if (genie_triple_init() < 0)
    {
        uint8_t temp_data[6];
        uint8_t mac[6];
        uint8_t key[16];
        uint32_t pid;
        /*
        uint8_t mac1[6] = {0x3c,0x5d,0x29,0x38,0x93,0x88};//{0x3c,0x5d,0x29,0x36,0x7c,0x3d};// 
        uint8_t key1[16] = {0x00,0xf1,0xe8,0x41,0x2e,0xaf,0xc5,0xfc,0x0a,0x1a,0x75,0x36,0x39,0x58,0x35,0xdc};//{0xf4,0x7b,0xec,0xba,0xd1,0xbb,0xc9,0x2d,0x6e,0x6c,0xd3,0x01,0x0e,0x86,0x17,0x98};
        uint32_t pid1 = 11115013; 
        */
        
        uint8_t mac1[6] = {0x18,0x14,0x6c,0x8e,0x84,0xf7};//{0x3c,0x5d,0x29,0x36,0x7c,0x3d};// 
        uint8_t key1[16] = {0xbb,0x48,0xec,0x45,0x9b,0xf0,0x7f,0xa2,0x68,0x49,0x0b,0x7b,0xf5,0x48,0x09,0x8d};//{0xf4,0x7b,0xec,0xba,0xd1,0xbb,0xc9,0x2d,0x6e,0x6c,0xd3,0x01,0x0e,0x86,0x17,0x98};
        uint32_t pid1 = 5737560;  
         
        memcpy(mac,&g_bootload_flash_info[0],6);
        memcpy(key,&g_bootload_flash_info[6],16);
        memcpy(&pid,g_bootload_flash_info+22,4);

       
        if(mac[0]==0x11 && key[0]==0xab)
        {
            memcpy(mac,mac1,6);
            memcpy(key,key1,16);
            pid=pid1;       
        }
        genie_triple_write(&pid, mac, key);

        temp_data[0] =  mac[0];
        temp_data[1] =  mac[1];
        temp_data[2] =  mac[2];
        temp_data[3] =  mac[3];
        temp_data[4] =  mac[4];
        temp_data[5] =  mac[5];
        hal_flash_write_mac_params(temp_data, 6);    
        
#ifdef CONFIG_PM_SLEEP
        genie_sal_sleep_disable();
#endif
        aos_reboot();
        return GENIE_SERVICE_ERRCODE_NO_TRIPLE;
    }

    ret = genie_mesh_init(genie_service_ctx.p_mesh_elem, genie_service_ctx.mesh_elem_counts);
    if (ret != 0)
    {
        GENIE_LOG_ERR("genie service init fail(%d)", ret);
        
        return GENIE_SERVICE_ERRCODE_MESH_INIT;
    }
    else
    {
        return GENIE_SERVICE_SUCCESS;
    }
}
