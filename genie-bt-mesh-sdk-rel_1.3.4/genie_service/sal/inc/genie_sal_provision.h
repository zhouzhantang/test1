/*
 * Copyright (C) 2018-2021 Alibaba Group Holding Limited
 */

#ifndef __GENIE_SAL_PROVISION_H__
#define __GENIE_SAL_PROVISION_H__

#define UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V1 0x10 //bit4-7
#define UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V2 0x20 //bit4-7
#define UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V3 0x30 //bit4-7
#define UNPROV_ADV_FEATURE2_GENIE_MESH_STACK_V4 0x40 //bit4-7

uint8_t genie_sal_provision_get_module_type(void);

#endif
