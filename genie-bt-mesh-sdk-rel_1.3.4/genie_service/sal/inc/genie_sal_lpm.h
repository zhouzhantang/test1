#ifndef __GENIE_SAL_LPM_H__
#define __GENIE_SAL_LPM_H__

#if defined(BOARD_TG7100B)
#include "pm.h"
#endif

typedef void (*genie_io_wakeup_cb)(void *arg);

enum io_pol
{
    FALLING = 0,
    RISING = 1,
    ACT_LOW = 2,
    ACT_HIGH = 3,
};

#define PWR_STANDBY_BOOT_FLAG (0x1688)
#define PWR_BOOT_REASON (0x4000f034)

void genie_sal_sleep_enable();
void genie_sal_sleep_disable();
int genie_sal_sleep_enter_standby();
int genie_sal_sleep_wakup_io_set(uint8_t port, uint8_t pol);
bool genie_sal_sleep_wakeup_io_get_status(uint8_t port);
int genie_sal_io_wakeup_cb_register(genie_io_wakeup_cb cb);
int genie_sal_io_wakeup_cb_unregister();

uint32_t genie_sal_lpm_get_bootup_reason(void);
int32_t genie_sal_lpm_set_bootup_reason(uint32_t reason);

#endif
