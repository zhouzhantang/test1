/*
 * Copyright (C) 2018-2021 Alibaba Group Holding Limited
 */

#include <stdint.h>
#include <string.h>
#include <hal/hal.h>

#define GENIE_MCU_UART_PORT (0)

int genie_sal_uart_init(void)
{
	return 0;
}

int32_t genie_sal_uart_send_one_byte(uint8_t byte)
{
     uart_dev_t uart_send;
     uint8_t send_data = 0;

     memset(&uart_send, 0, sizeof(uart_send));
     uart_send.port = GENIE_MCU_UART_PORT;

     send_data = byte;

     return hal_uart_send(&uart_send, &send_data, 1, 0);
}
