/*
 * Copyright (C) 2017-2019 C-SKY Microsystems Co., Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/******************************************************************************
 * @file     pinmux.h
 * @brief    Header file for the pinmux
 * @version  V1.0
 * @date     26. May 2019
 ******************************************************************************/
#ifndef _PINMUX_H_
#define _PINMUX_H_

#include <stdint.h>
#include "pin_name.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t drv_pinmux_config(pin_name_e pin, pin_func_e pin_func);
void drv_pinmux_reset(void);
void csi_pinmux_prepare_sleep_action();
void csi_pinmux_wakeup_sleep_action(void);

#ifdef __cplusplus
}
#endif

#endif /* _PINMUX_H_ */

