#include <yoc_config.h>
#include <stdio.h>
#include <stdint.h>
#include <drv_usart.h>
#include <hal/soc/uart.h>
#include <hal/soc/wdg.h>
#include <drv_wdt.h>
#include <drv_gpio.h>
#include <drv_usart.h>
#include <pinmux.h>
#include <pwrmgr.h>
#include <yoc/lpm.h>

#ifdef CONFIG_GPIO_UART
void gpio_uart_init(void)
{
    drv_gpio_usart_init(LOG_PIN);
    drv_gpio_usart_config(9600, USART_MODE_SINGLE_WIRE, USART_PARITY_NONE, USART_STOP_BITS_1, USART_DATA_BITS_8);
}
#endif

#ifdef CONFIG_WDT
wdg_dev_t g_wdg_dev = {0};
int wdt_init(void)
{
    int ret = -1;
    wdg_config_t wdg_cfg;

    /* wdg port set */
    g_wdg_dev.port = 0;

    /* set reload time to 2000ms */
    g_wdg_dev.config.timeout = 2000; /* 2000ms */

    /* init wdg1 with the given settings */
    ret = hal_wdg_init(&g_wdg_dev);
    if (ret != 0) {
        printf("wdg init error !\n");
    }

    /* config to auto feed mode with wdg1 */
    ret = csi_wdt_config_feed(g_wdg_dev.port, WDT_FEED_AUTO);
    if (ret != 0) {
        printf("wdg auto feed config error !\n");
    }
}
#endif
    uart_dev_t uart_0;
void board_base_init(void)
{

    uart_0.port                = STDIO_UART;
#ifdef CONFIG_GPIO_UART
    uart_0.config.baud_rate    = 9600;
#else
    uart_0.config.baud_rate    = 9600;
#endif
    uart_0.config.data_width   = DATA_WIDTH_8BIT;
    uart_0.config.parity       = NO_PARITY;
    uart_0.config.stop_bits    = STOP_BITS_1;
    uart_0.config.flow_control = FLOW_CONTROL_DISABLED;
    uart_0.config.mode = MODE_TX_RX;
    hal_uart_init(&uart_0);

#ifdef CONFIG_GPIO_UART
    gpio_uart_init();
#endif

#if defined CONFIG_PM_SLEEP
    pm_init();
#endif

#ifdef CONFIG_WDT
    wdt_init();
#endif
}
