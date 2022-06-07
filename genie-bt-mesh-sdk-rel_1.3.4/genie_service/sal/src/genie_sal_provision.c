/*
 * Copyright (C) 2018-2021 Alibaba Group Holding Limited
 */

#include <stdint.h>

#include "genie_sal_provision.h"

uint8_t genie_sal_provision_get_module_type(void)
{
    uint8_t model_type = 0;

#if defined(BOARD_TG7100B)
    model_type = UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V1;
#elif defined(BOARD_PHY6220_EVB) || defined(BOARD_TG7120B_EVB)
    model_type = UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V2;
#elif defined(BOARD_TG7121B_EVB)
    model_type = UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V3;
#elif defined(BOARD_TG7100CEVB)
    model_type = UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V4;
#endif

    return model_type;
}
