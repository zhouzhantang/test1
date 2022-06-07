


/*

static ktask_t tg7100_hdr;
static cpu_stack_t tg7100_stack[3600 / sizeof(cpu_stack_t)];
static void loop_task(void *arg)
{
    LOGI(tag, "board loop task start");

    static long long last_1500ms = 0;
    static long long last_200ms = 0; 
    static long long last_1000ms = 0;
    static long long last_5000ms = 0;
    static long long  last_20000ms = 0;
    static uint8_t cnt = 0;
    uint64_t now = 0;

    while (true)
    {
        user_uart_recv();
         aos_msleep(70); 
    }
}
*/

void my_task_init(void)
{
    

}