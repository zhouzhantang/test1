/*
 * Copyright (C) 2018-2021 Alibaba Group Holding Limited
 */

#include <stdint.h>
#include <string.h>
#include <types.h>
#include <hal/hal.h>
#include <genie_sal_lpm.h>
#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
#include <pm.h>
#include "gpio.h"
#endif

extern void drv_pm_sleep_enable();
extern void drv_pm_sleep_disable();
extern int drv_pm_enter_standby();

void genie_sal_sleep_enable()
{
#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
   drv_pm_sleep_enable();
#endif
}

void genie_sal_sleep_disable()
{
#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
   drv_pm_sleep_disable();
#endif
}

int genie_sal_sleep_enter_standby()
{
#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
   return drv_pm_enter_standby();
#else
   return 0;
#endif
}

int genie_sal_sleep_wakup_io_set(uint8_t port, uint8_t pol)
{
#if defined(BOARD_TG7100B)
   phy_gpio_wakeup_set(port, NEGEDGE);
   return 0;
#elif defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
   uint8_t type = (pol >= 2) ? pol - 2 : pol;
   phy_gpio_pull_set(port, !type);
   phy_gpio_wakeup_set(port, type);
   return 0;
#endif

   return -1;
}

#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
__attribute__((section(".__sram.code"))) bool genie_sal_sleep_wakeup_io_get_status(uint8_t port)
{
   return phy_gpio_read(port);
}
#else
bool genie_sal_sleep_wakeup_io_get_status(uint8_t port)
{
   return 0;
}
#endif

int genie_sal_io_wakeup_cb_register(genie_io_wakeup_cb cb)
{
   if (!cb)
   {
      return -1;
   }

#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
   drv_pm_io_wakeup_handler_register(cb);
#endif

   return 0;
}

int genie_sal_io_wakeup_cb_unregister()
{
#if defined(BOARD_TG7100B) || defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
   drv_pm_io_wakeup_handler_unregister();
#endif

   return 0;
}

uint32_t genie_sal_lpm_get_bootup_reason(void)
{
#if defined(BOARD_TG7100B)
   uint32_t *p_boot_reason = (uint32_t *)PWR_BOOT_REASON;

   return *p_boot_reason;
#endif

   return 0;
}

int32_t genie_sal_lpm_set_bootup_reason(uint32_t reason)
{
#if defined(BOARD_TG7100B)
   *(volatile uint32_t *)PWR_BOOT_REASON = reason;

   return 0;
#endif

   return -1;
}
