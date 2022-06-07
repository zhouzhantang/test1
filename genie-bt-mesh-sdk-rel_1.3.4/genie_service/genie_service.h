/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#ifndef _GENIE_SERVICE_H_
#define _GENIE_SERVICE_H_

#define GENIE_LOG_ERR(fmt, ...) SYS_LOG_ERR(fmt, ##__VA_ARGS__)
#define GENIE_LOG_WARN(fmt, ...) SYS_LOG_WRN(fmt, ##__VA_ARGS__)
#define GENIE_LOG_INFO(fmt, ...) SYS_LOG_INF(fmt, ##__VA_ARGS__)
#define GENIE_LOG_DBG(fmt, ...) SYS_LOG_DBG(fmt, ##__VA_ARGS__)

#define GENIE_GLP_SLEEP_TIME (1100) //unit:ms
#define GENIE_GLP_WAKEUP_TIME (60)  //unit:ms

#define GENIE_SERVICE_ERRCODE_USER_BASE (-0x100)
#define GENIE_SERVICE_ERRCODE_MESH_BASE (-0x200)
#define GENIE_SERVICE_ERRCODE_TRIPLE_BASE (-0x300)

typedef enum _genie_service_errcode_s
{
    GENIE_SERVICE_SUCCESS = 0,
    GENIE_SERVICE_ERRCODE_USER_INPUT = GENIE_SERVICE_ERRCODE_USER_BASE - 1,
    GENIE_SERVICE_ERRCODE_MESH_INIT = GENIE_SERVICE_ERRCODE_MESH_BASE - 1,
    GENIE_SERVICE_ERRCODE_NO_TRIPLE = GENIE_SERVICE_ERRCODE_MESH_BASE - 2,

    GENIE_SERVICE_ERRCODE_TRIPLE_READ_FAIL = GENIE_SERVICE_ERRCODE_TRIPLE_BASE - 1,
    GENIE_SERVICE_ERRCODE_TRIPLE_WRITE_FAIL = GENIE_SERVICE_ERRCODE_TRIPLE_BASE - 2,
} genie_service_errcode;

typedef void (*user_event_cb)(genie_event_e event, void *p_arg);

typedef struct _genie_service_ctx_s
{
    struct bt_mesh_elem *p_mesh_elem;
    unsigned char mesh_elem_counts;
    user_event_cb event_cb;
    uint32_t prov_timeout;
#ifdef CONFIG_PM_SLEEP
    genie_lpm_conf_t lpm_conf;
#endif
} genie_service_ctx_t;

/**
 * @brief The initialization api for genie sdk. The application always
 *        involks this api to initialize AIS/Mesh procedure for bussiness.
 */
int genie_service_init(genie_service_ctx_t *p_ctx);

genie_service_ctx_t *genie_service_get_context(void);

#endif
