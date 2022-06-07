#ifndef __SWITCHES_OUTPUT_H__
#define __SWITCHES_OUTPUT_H__

#include <stdint.h>
#include "genie_sal.h"
#include "genie_mesh_api.h"

struct switches_output_str
{
    uint8_t out_positive_pin;/*+pin*/
    uint8_t out_negative_pin;/*-pin*/
    gpio_dev_t gpio_dev[2];
};




#endif