/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#ifndef __GENIE_LPM_H__
#define __GENIE_LPM_H__

#define DEFAULT_BOOTUP_DELAY_SLEEP_TIME (10000) //Unit:ms

#define MSG_ATTR_TYPE_ACTIVE_WAKEUP 0x01BA
#define MSG_ATTR_TYPE_ENTER_SLEEP 0x01BB

#define MSG_ACTIVE_TIME_MIN (2000)       //unit ms
#define MSG_ACTIVE_TIME_DEFAULT (5000)   //unit ms
#define MSG_ACK_AFTER_SLEEP_TIME (2000)  //unit ms
#define MSG_UNACK_AFTER_SLEEP_TIME (100) //unit ms
#define MSG_CHECK_AGAIN_TIME (500)       //unit ms

typedef enum _genie_lpm_status_e
{
    STATUS_WAKEUP,
    STATUS_SLEEP
} genie_lpm_status_e;

typedef enum _genie_lpm_wakeup_reason_e
{
    WAKEUP_BY_IO,
    WAKEUP_BY_TIMER,
    WAKEUP_IS_WAKEUP
} genie_lpm_wakeup_reason_e;

typedef void (*genie_lpm_cb_t)(genie_lpm_wakeup_reason_e reason, genie_lpm_status_e status, void *arg);

typedef struct _genie_lpm_wakeup_io_config_s
{
    uint8_t port;
    uint8_t io_pol;
} genie_lpm_wakeup_io_config_t;

#define GENIE_WAKEUP_PIN(_port, _pol) \
    {                                 \
        .port = (_port),              \
        .io_pol = (_pol),             \
    }

typedef struct _genie_lpm_wakeup_io_s
{
    uint8_t io_list_size;
    genie_lpm_wakeup_io_config_t *io_config;
} genie_lpm_wakeup_io_t;

typedef struct _genie_lpm_io_status_s
{
    uint8_t port;
    uint8_t status;
    uint8_t trigger_flag;
} genie_lpm_io_status_t;

typedef struct _genie_lpm_io_status_list_s
{
    uint8_t size;
    genie_lpm_io_status_t *io_status;
} genie_lpm_io_status_list_t;

typedef struct _genie_lpm_conf_s
{
    uint8_t lpm_wakeup_io;                      //0:then not wakeup by GPIO
    genie_lpm_wakeup_io_t lpm_wakeup_io_config; //wakeup IO list config
    uint8_t lpm_wakeup_msg;                     //1:support wakeup by active msg
    uint8_t is_auto_enable;                     //1:auto enter sleep mode when bootup
    uint32_t delay_sleep_time;                  //if auto enter sleep,delay some time then enter sleep mode
    uint16_t sleep_ms;                          //sleep time
    uint16_t wakeup_ms;                         //wakeup time
    genie_lpm_cb_t genie_lpm_cb;                //User callback
} genie_lpm_conf_t;

typedef struct _genie_lpm_ctx_s
{
    genie_lpm_conf_t p_config;
    uint8_t status;
    uint8_t has_disabled;
    aos_timer_t delay_sleep_timer;
    aos_timer_t glp_wakeup_timer;
    aos_timer_t io_wakeup_timer;
    aos_timer_t msg_wakeup_timer;
    aos_mutex_t mutex;
} genie_lpm_ctx_t;

void genie_lpm_init(genie_lpm_conf_t *lpm_conf);
int genie_lpm_start(void);

int genie_lpm_enable(bool force);
int genie_lpm_disable(void);
int genie_lpm_deep_sleep(void);

uint32_t genie_lpm_get_bootup_reason(void);
int32_t genie_lpm_set_bootup_reason(uint32_t reason);
bool genie_lpm_get_wakeup_io_status(uint8_t port);
int genie_lpm_handle_model_mesg(genie_transport_model_param_t *p_msg);

#endif
