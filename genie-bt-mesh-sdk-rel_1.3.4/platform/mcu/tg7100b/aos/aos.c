/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <stdlib.h>
#include <k_api.h>
#include <aos/aos.h>
#include <kvmgr.h>

extern void board_base_init(void);
#define INIT_TASK_STACK_SIZE 2048
static cpu_stack_t app_stack[INIT_TASK_STACK_SIZE / 4] __attribute((section(".data")));

ktask_t *g_aos_init;
krhino_err_proc_t g_err_proc = soc_err_proc;
size_t soc_get_cur_sp()
{
    volatile size_t dummy = (size_t)&dummy;
    return dummy;
}

#if (RHINO_CONFIG_HW_COUNT > 0)
hr_timer_t soc_hr_hw_cnt_get(void)
{
    return 0;
}
#endif

#define HEAP_BUFF_SIZE (1024*10)
uint32_t g_heap_buff[HEAP_BUFF_SIZE / 4];
extern volatile uint32_t __heap_end;
extern volatile uint32_t __heap_start;
k_mm_region_t g_mm_region[1];
int           g_region_num  = sizeof(g_mm_region) / sizeof(k_mm_region_t);

void soc_err_proc(kstat_t err)
{
    printf("kernel panic,err %d!\n", err);
}

uint32_t aos_get_version_info(void)
{
    return 0;
}

static kinit_t kinit;
void board_cli_init(void)
{
    kinit.argc = 0;
    kinit.argv = NULL;
    kinit.cli_enable = 1;
}

int aos_clear_ftst_flag(void)
{
    int ret = -1;
    uint16_t flag = 0;

    ret = aos_kv_set("FTST", &flag, sizeof(uint16_t), 0);
    if (ret != 0)
    {
        printf("clear FTST flag error\n");
        return ret;
    }

    return 0;
}

int aos_enter_factory_test(void)
{
    int ret = -1;
    uint16_t flag_read = 0;
    int len = sizeof(uint16_t);

    ret = aos_kv_get("FTST", (void *)&flag_read, &len);
    //printf("flag_read: 0x%04X\n", flag_read);
    if ((ret == 0) && (flag_read == 0xAA55))
    {
        return 1;
    }

    return 0;
}

extern int application_start(int argc, char **argv);
static void application_task_entry(void *arg)
{
    board_base_init();
    board_cli_init();

#ifdef CONFIG_AOS_CLI
    aos_cli_init();
#endif

#ifdef AOS_KV
    aos_kv_init();
#endif

#ifdef OSAL_RHINO
    extern void dumpsys_cli_init(void);
    dumpsys_cli_init();
#endif

#ifdef AOS_LOOP
    aos_loop_init();
#endif

    //aos_components_init(&kinit);

#ifdef CONFIG_GENIE_FACTORY_TEST
    if (aos_enter_factory_test() == 1)
    {
        aos_clear_ftst_flag();
        ble_dut_start();
    }
    else
#endif
    {
#ifndef AOS_BINS
        application_start(kinit.argc, kinit.argv); /* jump to app/example entry */
#endif
    }
}

int main(void)
{
    g_mm_region[0].start = (uint8_t *)g_heap_buff;
    g_mm_region[0].len = HEAP_BUFF_SIZE;

    memset(g_mm_region[0].start, 0, g_mm_region[0].len);

    aos_init();

    //pm_init();

    ktask_t app_task_handle = {0};
    /* init task */
    krhino_task_create(&app_task_handle, "aos-init", NULL,
                       AOS_DEFAULT_APP_PRI, 0, app_stack,
                       INIT_TASK_STACK_SIZE / 4, application_task_entry, 1);
    aos_start();
    return 0;
}

