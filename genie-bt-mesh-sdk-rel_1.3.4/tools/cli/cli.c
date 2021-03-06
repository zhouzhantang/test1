/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <aos/aos.h>
#include <hal/hal.h>
#include <k_config.h>
#include <k_api.h>

#define RET_CHAR '\n'
#define END_CHAR '\r'
#define PROMPT "# "
#define EXIT_MSG "exit"

#if (AOS_CLI_MINI_SIZE > 0)
char *cli_mini_support_cmds[] = {"netmgr", "help", "sysver",
                                 "reboot", "time", "ota"};
#endif

static struct cli_st *cli = NULL;
static int volatile cliexit = 0;
char esc_tag[64] = {0};
static uint8_t esc_tag_len = 0;
//static aos_task_t cli_task;

static cpu_stack_t aos_cli_stack[CONFIG_AOS_CLI_STACK_SIZE / sizeof(cpu_stack_t)];
static ktask_t aos_cli_hdr;

#ifdef CONIFG_GENIE_MESH_BINARY_CMD
#define FRAME_BUFFER_SIZE (256)
static unsigned char frame_buff_index = 0;
static unsigned char frame_buff[FRAME_BUFFER_SIZE];
static aos_timer_t uart_frame_end_timer;
extern int genie_bin_cmds_handle(uint8_t *p_data, uint8_t data_len);
static void uart_frame_end_timer_cb(void *arg1, void *arg2)
{
    if (frame_buff_index > 0) //received some datas
    {
        genie_bin_cmds_handle(frame_buff, frame_buff_index);
        frame_buff_index = 0; //reset this index
    }
}
#endif

extern void hal_reboot(void);
extern void log_cli_init(void);

#ifdef CONFIG_AOS_CLI_BOARD
extern int board_cli_init(void);
#endif

#ifdef OSAL_RHINO
extern uint32_t krhino_version_get(void);
#endif

int cli_getchar(char *inbuf);
int cli_putstr(char *msg);

/* Find the command 'name' in the cli commands table.
 * If len is 0 then full match will be performed else upto len bytes.
 * Returns: a pointer to the corresponding cli_command struct or NULL.
 */
static const struct cli_command *lookup_command(char *name, int len)
{
    int i = 0;
    int n = 0;

    while (i < MAX_COMMANDS && n < cli->num_commands)
    {
        if (cli->commands[i]->name == NULL)
        {
            i++;
            continue;
        }
        /* See if partial or full match is expected */
        if (len != 0)
        {
            if (!strncmp(cli->commands[i]->name, name, len))
            {
                return cli->commands[i];
            }
        }
        else
        {
            if (!strcmp(cli->commands[i]->name, name))
            {
                return cli->commands[i];
            }
        }

        i++;
        n++;
    }

    return NULL;
}

/*proc one cli cmd and to run the according funtion
* Returns: 0 on success:
           1 fail
*/
int proc_onecmd(int argc, char *argv[])
{
    int i = 0;
    const char *p;
    const struct cli_command *command = NULL;

    if (argc < 1)
    {
        return 0;
    }

    if (!cli->echo_disabled)
    {
        csp_printf("\r\n");
        fflush(stdout);
    }

    /*
     * Some comamands can allow extensions like foo.a, foo.b and hence
     * compare commands before first dot.
     */
    i = ((p = strchr(argv[0], '.')) == NULL) ? 0 : (p - argv[0]);

    command = lookup_command(argv[0], i);
    if (command == NULL)
    {
        return 1;
    }

    cli->outbuf = aos_malloc(OUTBUF_SIZE);
    if (NULL == cli->outbuf)
    {
        aos_cli_printf("Error! cli alloc mem fail!\r\n");
        return 1;
    }
    memset(cli->outbuf, 0, OUTBUF_SIZE);

    command->function(cli->outbuf, OUTBUF_SIZE, argc, argv);
    aos_cli_printf("%s", cli->outbuf);

    aos_free(cli->outbuf);
    cli->outbuf = NULL;
    return 0;
}

/* Parse input line and locate arguments (if any), keeping count of the number
 * of arguments and their locations.  Look up and call the corresponding cli
 * function if one is found and pass it the argv array.
 *
 * Returns: 0 on success: the input line contained at least a function name and
 *          that function exists and was called.
 *          1 on lookup failure: there is no corresponding function for the
 *          input line.
 *          2 on invalid syntax: the arguments list couldn't be parsed
 */
static int handle_input(char *inbuf)
{
    struct
    {
        unsigned inArg : 1;
        unsigned inQuote : 1;
        unsigned done : 1;
    } stat;
    static char *argvall[CLI_MAX_ONCECMD_NUM][CLI_MAX_ARG_NUM];
    int argcall[CLI_MAX_ONCECMD_NUM] = {0};
    /*
    static char *argv[CLI_MAX_ONCECMD_NUM][CLI_MAX_ARG_NUM];
    int argc = 0;*/
    int cmdnum = 0;
    int *pargc = &argcall[0];
    int i = 0;
    int ret = 0;

    memset((void *)&argvall, 0, sizeof(argvall));
    memset((void *)&argcall, 0, sizeof(argcall));
    memset(&stat, 0, sizeof(stat));

    do
    {
        switch (inbuf[i])
        {
        case '\0':
            if (stat.inQuote)
            {
                return 2;
            }
            stat.done = 1;
            break;

        case '"':
            if (i > 0 && inbuf[i - 1] == '\\' && stat.inArg)
            {
                memcpy(&inbuf[i - 1], &inbuf[i], strlen(&inbuf[i]) + 1);
                --i;
                break;
            }
            if (!stat.inQuote && stat.inArg)
            {
                break;
            }
            if (stat.inQuote && !stat.inArg)
            {
                return 2;
            }

            if (!stat.inQuote && !stat.inArg)
            {
                stat.inArg = 1;
                stat.inQuote = 1;
                (*pargc)++;
                argvall[cmdnum][(*pargc) - 1] = &inbuf[i + 1];
            }
            else if (stat.inQuote && stat.inArg)
            {
                stat.inArg = 0;
                stat.inQuote = 0;
                inbuf[i] = '\0';
            }
            break;

        case ' ':
            if (i > 0 && inbuf[i - 1] == '\\' && stat.inArg)
            {
                memcpy(&inbuf[i - 1], &inbuf[i], strlen(&inbuf[i]) + 1);
                --i;
                break;
            }
            if (!stat.inQuote && stat.inArg)
            {
                stat.inArg = 0;
                inbuf[i] = '\0';
            }
            break;

        case ';':
            if (i > 0 && inbuf[i - 1] == '\\' && stat.inArg)
            {
                memcpy(&inbuf[i - 1], &inbuf[i], strlen(&inbuf[i]) + 1);
                --i;
                break;
            }
            if (stat.inQuote)
            {
                return 2;
            }
            if (!stat.inQuote && stat.inArg)
            {
                stat.inArg = 0;
                inbuf[i] = '\0';

                if (*pargc)
                {
                    if (++cmdnum < CLI_MAX_ONCECMD_NUM)
                    {
                        pargc = &argcall[cmdnum];
                    }
                }
            }

            break;

        default:
            if (!stat.inArg)
            {
                stat.inArg = 1;
                (*pargc)++;
                argvall[cmdnum][(*pargc) - 1] = &inbuf[i];
            }
            break;
        }
    } while (!stat.done && ++i < INBUF_SIZE && cmdnum < CLI_MAX_ONCECMD_NUM &&
             (*pargc) < CLI_MAX_ARG_NUM);

    if (stat.inQuote)
    {
        return 2;
    }

    for (i = 0; i <= cmdnum && i < CLI_MAX_ONCECMD_NUM; i++)
    {
        int err;
        err = proc_onecmd(argcall[i], argvall[i]);
#ifdef CONFIG_GENIE_MESH_AT_CMD
        if (err == 1)
        {
            extern int genie_at_cmd_parser(char data[]);
            err = genie_at_cmd_parser(inbuf);
            if (err == 0)
            {
                ret = 3;
            }
        }
#endif
        ret |= err;
    }

    return ret;
}

/* Perform basic tab-completion on the input buffer by string-matching the
 * current input line against the cli functions table.  The current input line
 * is assumed to be NULL-terminated.
 */
static void tab_complete(char *inbuf, unsigned int *bp)
{
    int i, n, m;
    const char *fm = NULL;

    aos_cli_printf("\r\n");

    /* show matching commands */
    for (i = 0, n = 0, m = 0; i < MAX_COMMANDS && n < cli->num_commands; i++)
    {
        if (cli->commands[i]->name != NULL)
        {
            if (!strncmp(inbuf, cli->commands[i]->name, *bp))
            {
                m++;
                if (m == 1)
                {
                    fm = cli->commands[i]->name;
                }
                else if (m == 2)
                    aos_cli_printf("%s %s ", fm, cli->commands[i]->name);
                else
                    aos_cli_printf("%s ", cli->commands[i]->name);
            }
            n++;
        }
    }

    /* there's only one match, so complete the line */
    if (m == 1 && fm)
    {
        n = strlen(fm) - *bp;
        if (*bp + n < INBUF_SIZE)
        {
            memcpy(inbuf + *bp, fm + *bp, n);
            *bp += n;
            inbuf[(*bp)++] = ' ';
            inbuf[*bp] = '\0';
        }
    }
    if (m >= 2)
    {
        aos_cli_printf("\r\n");
    }

    /* just redraw input line */
    aos_cli_printf("%s%s", PROMPT, inbuf);
}

#if (AOS_CLI_MINI_SIZE <= 0)

static void cli_history_input(void)
{
    char *inbuf = cli->inbuf;
    int charnum = strlen(cli->inbuf) + 1;

    int his_cur = cli->his_cur;
    int left_num = INBUF_SIZE - his_cur;
    char lastchar;
    int tmp_idx;

    cli->his_idx = his_cur;

    if (left_num >= charnum)
    {
        tmp_idx = his_cur + charnum - 1;
        lastchar = cli->history[tmp_idx];
        strncpy(&(cli->history[his_cur]), inbuf, charnum);
    }
    else
    {
        tmp_idx = (his_cur + charnum - 1) % INBUF_SIZE;
        lastchar = cli->history[tmp_idx];
        strncpy(&(cli->history[his_cur]), inbuf, left_num);
        strncpy(&(cli->history[0]), inbuf + left_num, charnum - left_num);
    }
    tmp_idx = (tmp_idx + 1) % INBUF_SIZE;
    cli->his_cur = tmp_idx;

    /*overwrite*/
    if ('\0' != lastchar)
    {

        while (cli->history[tmp_idx] != '\0')
        {
            cli->history[tmp_idx] = '\0';
            tmp_idx = (tmp_idx + 1) % INBUF_SIZE;
        }
    }
}

static void cli_up_history(char *inaddr)
{
    int index;
    int lastindex = 0;

    lastindex = cli->his_idx;
    index = (cli->his_idx - 1 + INBUF_SIZE) % INBUF_SIZE;

    while ((cli->history[index] == '\0') && (index != cli->his_idx))
    {
        index = (index - 1 + INBUF_SIZE) % INBUF_SIZE;
    }
    if (index != cli->his_idx)
    {
        while (cli->history[index] != '\0')
        {
            index = (index - 1 + INBUF_SIZE) % INBUF_SIZE;
        }
        index = (index + 1) % INBUF_SIZE;
    }
    cli->his_idx = index;

    while (cli->history[lastindex] != '\0')
    {

        *inaddr++ = cli->history[lastindex];
        lastindex = (lastindex + 1) % INBUF_SIZE;
    }
    *inaddr = '\0';

    return;
}

static void cli_down_history(char *inaddr)
{
    int index;
    int lastindex = 0;

    lastindex = cli->his_idx;
    index = cli->his_idx;

    while ((cli->history[index] != '\0'))
    {
        index = (index + 1) % INBUF_SIZE;
    }
    if (index != cli->his_idx)
    {
        while (cli->history[index] == '\0')
        {
            index = (index + 1) % INBUF_SIZE;
        }
    }
    cli->his_idx = index;

    while (cli->history[lastindex] != '\0')
    {
        *inaddr++ = cli->history[lastindex];
        lastindex = (lastindex + 1) % INBUF_SIZE;
    }

    *inaddr = '\0';

    return;
}
#endif

/* Get an input line.
 *
 * Returns: 1 if there is input, 0 if the line should be ignored.
 */
static int get_input(char *inbuf, unsigned int *bp)
{
    char c;
    int esc = 0, key1 = -1, key2 = -1;
    if (inbuf == NULL)
    {
        aos_cli_printf("inbuf_null\r\n");
        return 0;
    }

    while (cli_getchar(&c) == 1)
    {
        if (c == RET_CHAR || c == END_CHAR)
        { /* end of input line */
            inbuf[*bp] = '\0';
            *bp = 0;
            return 1;
        }

        if (c == 0x1b)
        { /* escape sequence */
            esc = 1;
            key1 = -1;
            key2 = -1;
            continue;
        }

        if (esc)
        {
            if (key1 < 0)
            {
                key1 = c;
                if (key1 != 0x5b)
                {
                    /* not '[' */
                    inbuf[(*bp)] = 0x1b;
                    (*bp)++;
                    inbuf[*bp] = key1;
                    (*bp)++;
                    if (!cli->echo_disabled)
                    {
                        csp_printf("\x1b%c", key1);
                        fflush(stdout);
                    }
                    esc = 0; /* quit escape sequence */
                }
                continue;
            }

            if (key2 < 0)
            {
                key2 = c;
                if (key2 == 't')
                {
                    esc_tag[0] = 0x1b;
                    esc_tag[1] = key1;
                    esc_tag_len = 2;
                }
            }

            if (key2 != 0x41 && key2 != 0x42 && key2 != 't')
            {
                /*unsupported esc sequence*/
                inbuf[(*bp)] = 0x1b;
                (*bp)++;
                inbuf[*bp] = key1;
                (*bp)++;
                inbuf[*bp] = key2;
                (*bp)++;
                if (!cli->echo_disabled)
                {
                    csp_printf("\x1b%c%c", key1, key2);
                    fflush(stdout);
                }
                esc_tag[0] = '\x0';
                esc_tag_len = 0;
                esc = 0; /* quit escape sequence */
                continue;
            }

#if (AOS_CLI_MINI_SIZE > 0)
            if (key2 == 0x41 || key2 == 0x42)
            {
                csp_printf(
                    "\r\n" PROMPT
                    "Warning! mini cli mode do not support history cmds!");
            }

#else
            if (key2 == 0x41)
            { /* UP */
                cli_up_history(inbuf);
                csp_printf("\r\n" PROMPT "%s", inbuf);
                *bp = strlen(inbuf);
                esc_tag[0] = '\x0';
                esc_tag_len = 0;
                esc = 0; /* quit escape sequence */
                continue;
            }

            if (key2 == 0x42)
            { /* DOWN */
                cli_down_history(inbuf);
                csp_printf("\r\n" PROMPT "%s", inbuf);
                *bp = strlen(inbuf);
                esc_tag[0] = '\x0';
                esc_tag_len = 0;
                esc = 0; /* quit escape sequence */
                continue;
            }
#endif

            /* ESC_TAG */
            if (esc_tag_len >= sizeof(esc_tag))
            {
                esc_tag[0] = '\x0';
                esc_tag_len = 0;
                esc = 0; /* quit escape sequence */
                csp_printf("Error: esc_tag buffer overflow\r\n");
                fflush(stdout);
                continue;
            }
            esc_tag[esc_tag_len++] = c;
            if (c == 'm')
            {
                esc_tag[esc_tag_len++] = '\x0';
                if (!cli->echo_disabled)
                {
                    csp_printf("%s", esc_tag);
                    fflush(stdout);
                }
                esc = 0; /* quit escape sequence */
            }
            continue;
        }

        inbuf[*bp] = c;
        if ((c == 0x08) || /* backspace */
            (c == 0x7f))
        { /* DEL */
            if (*bp > 0)
            {
                (*bp)--;
                if (!cli->echo_disabled)
                {
                    csp_printf("%c %c", 0x08, 0x08);
                    fflush(stdout);
                }
            }
            continue;
        }

        if (c == '\t')
        {
            inbuf[*bp] = '\0';
            tab_complete(inbuf, bp);
            continue;
        }

        if (!cli->echo_disabled)
        {
            csp_printf("%c", c);
            fflush(stdout);
        }

        (*bp)++;
        if (*bp >= INBUF_SIZE)
        {
            aos_cli_printf("Error: input buffer overflow\r\n");
            aos_cli_printf(PROMPT);
            *bp = 0;
            return 0;
        }
    }

    return 0;
}

/* Print out a bad command string, including a hex
 * representation of non-printable characters.
 * Non-printable characters show as "\0xXX".
 */
static void print_bad_command(char *cmd_string)
{
    if (cmd_string != NULL)
    {
        aos_cli_printf("command '%s' not found\r\n", cmd_string);
    }
}

/* Main CLI processing thread
 *
 * Waits to receive a command buffer pointer from an input collector, and
 * then processes.  Note that it must cleanup the buffer when done with it.
 *
 * Input collectors handle their own lexical analysis and must pass complete
 * command lines to CLI.
 */
static void cli_main(void *data)
{
    while (!cliexit)
    {
        int ret;
        char *msg = NULL;

        if (get_input(cli->inbuf, &cli->bp))
        {
            msg = cli->inbuf;
#if 0
            if (strcmp(msg, EXIT_MSG) == 0) {
                break;
            }
#endif
#if (AOS_CLI_MINI_SIZE <= 0)
            if (strlen(cli->inbuf) > 0)
            {
                cli_history_input();
            }
#endif

            ret = handle_input(msg);
            if (ret == 1)
            {
                print_bad_command(msg);
            }
            else if (ret == 2)
            {
                aos_cli_printf("syntax error\r\n");
            }

            aos_cli_printf("\r\n");
            esc_tag[0] = '\x0';
            esc_tag_len = 0;
#ifdef CONFIG_GENIE_MESH_AT_CMD
            if (ret != 3)
            {
                aos_cli_printf(PROMPT);
            }
#else
            aos_cli_printf(PROMPT);
#endif
        }
    }

    aos_cli_printf("CLI exited\r\n");
    aos_free(cli);
    cli = NULL;

    aos_task_exit(0);
}

static void help_cmd(char *buf, int len, int argc, char **argv);
static void version_cmd(char *buf, int len, int argc, char **argv);
#if (AOS_CLI_MINI_SIZE <= 0)

static void echo_cmd(char *buf, int len, int argc, char **argv);
static void exit_cmd(char *buf, int len, int argc, char **argv);
static void devname_cmd(char *buf, int len, int argc, char **argv);
static void pmem_cmd(char *buf, int len, int argc, char **argv);
static void mmem_cmd(char *buf, int len, int argc, char **argv);

#endif
static void reboot_cmd(char *buf, int len, int argc, char **argv);
static void uptime_cmd(char *buf, int len, int argc, char **argv);
static void ota_cmd(char *buf, int len, int argc, char **argv);

#if defined(FAKE_REBOOT)
#include "mesh_base.h"
static void srb_cmd(char *buf, int len, int argc, char **argv)
{
    mesh_state_init();
}
#endif
static const struct cli_command built_ins[] = {
    /*cli self*/
    {"help", "print this", help_cmd},

#if (AOS_CLI_MINI_SIZE <= 0)

    {"p", "print memory", pmem_cmd},
    {"m", "modify memory", mmem_cmd},
    {"echo", "echo for command", echo_cmd},
    {"exit", "close CLI", exit_cmd},
    {"devname", "print device name", devname_cmd},
#endif

    /*rhino*/
    {"sysver", "system version", version_cmd},
    {"reboot", "reboot system", reboot_cmd},
#if defined(FAKE_REBOOT)
    {"srb", "sw reboot system", srb_cmd},
#endif

    /*aos_rhino*/
    {"time", "system time", uptime_cmd},
    {"ota", "system ota", ota_cmd},
};

/* Built-in "help" command: prints all registered commands and their help
 * text string, if any.
 */
static void help_cmd(char *buf, int len, int argc, char **argv)
{
    int i, n;
    uint32_t build_in_count = sizeof(built_ins) / sizeof(built_ins[0]);

    aos_cli_printf("====Build-in Commands====\r\n");
    aos_cli_printf("====Support %d cmds once, seperate by ; ====\r\n",
                   CLI_MAX_ONCECMD_NUM);

    for (i = 0, n = 0; i < MAX_COMMANDS && n < cli->num_commands; i++)
    {
        if (cli->commands[i]->name)
        {
            aos_cli_printf("%-10s: %s\r\n", cli->commands[i]->name,
                           cli->commands[i]->help ? cli->commands[i]->help
                                                  : "");
            n++;
            if (n == build_in_count)
            {
                aos_cli_printf("\r\n");
                aos_cli_printf("====User Commands====\r\n");
            }
        }
    }
}

static void version_cmd(char *buf, int len, int argc, char **argv)
{
#ifdef OSAL_RHINO
    aos_cli_printf("kernel version :%d\r\n", krhino_version_get());
#else
    aos_cli_printf("kernel version :posix\r\n");
#endif
}

#if (AOS_CLI_MINI_SIZE <= 0)

static void echo_cmd(char *buf, int len, int argc, char **argv)
{
    if (argc == 1)
    {
        aos_cli_printf("Usage: echo on/off. Echo is currently %s\r\n",
                       cli->echo_disabled ? "Disabled" : "Enabled");
        return;
    }

    if (!strcmp(argv[1], "on"))
    {
        aos_cli_printf("Enable echo\r\n");
        cli->echo_disabled = 0;
    }
    else if (!strcmp(argv[1], "off"))
    {
        aos_cli_printf("Disable echo\r\n");
        cli->echo_disabled = 1;
    }
}

static void exit_cmd(char *buf, int len, int argc, char **argv)
{
    cliexit = 1;
    return;
}

static void devname_cmd(char *buf, int len, int argc, char **argv)
{
    aos_cli_printf("device name: %s\r\n", SYSINFO_DEVICE_NAME);
}

static void pmem_cmd(char *buf, int len, int argc, char **argv)
{
    int i;
    char *pos = NULL;
    char *addr = NULL;
    int nunits = 16;
    int width = 4;

    switch (argc)
    {
    case 4:
        width = strtol(argv[3], NULL, 0);
    case 3:
        nunits = strtol(argv[2], NULL, 0);
        nunits = nunits > 0x400 ? 0x400 : nunits;
    case 2:
        addr = (char *)strtol(argv[1], &pos, 0);
        break;
    default:
        break;
    }

    if (pos == NULL || pos == argv[1])
    {
        aos_cli_printf("p <addr> <nunits> <width>\r\n"
                       "addr  : address to display\r\n"
                       "nunits: number of units to display (default is 16)\r\n"
                       "width : width of unit, 1/2/4 (default is 4)\r\n");
        return;
    }

    switch (width)
    {
    case 1:
        for (i = 0; i < nunits; i++)
        {
            if (i % 16 == 0)
            {
                aos_cli_printf("0x%08x:", addr);
            }
            aos_cli_printf(" %02x", *(unsigned char *)addr);
            addr += 1;
            if (i % 16 == 15)
            {
                aos_cli_printf("\r\n");
            }
        }
        break;
    case 2:
        for (i = 0; i < nunits; i++)
        {
            if (i % 8 == 0)
            {
                aos_cli_printf("0x%08x:", addr);
            }
            aos_cli_printf(" %04x", *(unsigned short *)addr);
            addr += 2;
            if (i % 8 == 7)
            {
                aos_cli_printf("\r\n");
            }
        }
        break;
    default:
        for (i = 0; i < nunits; i++)
        {
            if (i % 4 == 0)
            {
                aos_cli_printf("0x%08x:", addr);
            }
            aos_cli_printf(" %08x", *(unsigned int *)addr);
            addr += 4;
            if (i % 4 == 3)
            {
                aos_cli_printf("\r\n");
            }
        }
        break;
    }
}

static void mmem_cmd(char *buf, int len, int argc, char **argv)
{
    void *addr = NULL;
    int width = 4;
    unsigned int value = 0;
    unsigned int old_value;
    unsigned int new_value;

    switch (argc)
    {
    case 4:
        width = strtol(argv[3], NULL, 0);
    case 3:
        value = strtol(argv[2], NULL, 0);
    case 2:
        addr = (void *)strtol(argv[1], NULL, 0);
        break;
    default:
        addr = NULL;
        break;
    }

    if (addr == NULL)
    {
        aos_cli_printf("m <addr> <value> <width>\r\n"
                       "addr  : address to modify\r\n"
                       "value : new value (default is 0)\r\n"
                       "width : width of unit, 1/2/4 (default is 4)\r\n");
        return;
    }

    switch (width)
    {
    case 1:
        old_value = (unsigned int)(*(unsigned char volatile *)addr);
        *(unsigned char volatile *)addr = (unsigned char)value;
        new_value = (unsigned int)(*(unsigned char volatile *)addr);
        break;
    case 2:
        old_value = (unsigned int)(*(unsigned short volatile *)addr);
        *(unsigned short volatile *)addr = (unsigned short)value;
        new_value = (unsigned int)(*(unsigned short volatile *)addr);
        break;
    case 4:
    default:
        old_value = *(unsigned int volatile *)addr;
        *(unsigned int volatile *)addr = (unsigned int)value;
        new_value = *(unsigned int volatile *)addr;
        break;
    }
    aos_cli_printf("value on 0x%x change from 0x%x to 0x%x.\r\n", addr,
                   old_value, new_value);
}

#endif

static void reboot_cmd(char *buf, int len, int argc, char **argv)
{
    aos_cli_printf("reboot\r\n");

    hal_reboot();
}

static void uptime_cmd(char *buf, int len, int argc, char **argv)
{
    aos_cli_printf("UP time %ld ms\r\n", (long)aos_now_ms());
}

void tftp_ota_thread(void *arg)
{
    aos_task_exit(0);
}

static void ota_cmd(char *buf, int len, int argc, char **argv)
{
    aos_task_new("LOCAL OTA", tftp_ota_thread, 0, 4096);
}

/* ------------------------------------------------------------------------- */

int aos_cli_register_command(const struct cli_command *cmd)
{
    int i;

    if (!cli)
    {
        return EPERM;
    }

    if (!cmd->name || !cmd->function)
    {
        return EINVAL;
    }

    if (cli->num_commands >= MAX_COMMANDS)
    {
        return ENOMEM;
    }

    /* Check if the command has already been registered.
     * Return 0, if it has been registered.
     */
    for (i = 0; i < cli->num_commands; i++)
    {
        if (cli->commands[i] == cmd)
        {
            return 0;
        }
    }

#if (AOS_CLI_MINI_SIZE > 0)
    for (i = 0; i < sizeof(cli_mini_support_cmds) / sizeof(char *); i++)
    {
        if (strcmp(cmd->name, cli_mini_support_cmds[i]) == 0)
        {
            break;
        }
    }
    if (i == sizeof(cli_mini_support_cmds) / sizeof(char *))
    {
        aos_cli_printf("Warning! mini cli mode do not support cmd:%s\r\n",
                       cmd->name);
        return 0;
    }
#endif

    cli->commands[cli->num_commands++] = cmd;

    return 0;
}

int aos_cli_unregister_command(const struct cli_command *cmd)
{
    int i;
    int remaining_cmds;

    if (!cmd->name || !cmd->function)
    {
        return EINVAL;
    }

    for (i = 0; i < cli->num_commands; i++)
    {
        if (cli->commands[i] == cmd)
        {
            cli->num_commands--;
            remaining_cmds = cli->num_commands - i;
            if (remaining_cmds > 0)
            {
                memmove(&cli->commands[i], &cli->commands[i + 1],
                        (remaining_cmds * sizeof(struct cli_command *)));
            }
            cli->commands[cli->num_commands] = NULL;
            return 0;
        }
    }

    return -ENOMEM;
}

int aos_cli_register_commands(const struct cli_command *cmds, int num_cmds)
{
    int i;
    int err;

    if (!cli)
    {
        return EPERM;
    }
    for (i = 0; i < num_cmds; i++)
    {
        if ((err = aos_cli_register_command(cmds++)) != 0)
        {
            return err;
        }
    }

    return 0;
}

int aos_cli_unregister_commands(const struct cli_command *cmds, int num_cmds)
{
    int i;
    int err;

    for (i = 0; i < num_cmds; i++)
    {
        if ((err = aos_cli_unregister_command(cmds++)) != 0)
        {
            return err;
        }
    }

    return 0;
}

int aos_cli_stop(void)
{
    cliexit = 1;

    return 0;
}

void *aos_cli_task_get(void)
{
    return &aos_cli_hdr;
}

int aos_cli_task_create(void)
{
    /*
    return aos_task_new_ext(&cli_task, "cli", cli_main, 0,
                            CONFIG_AOS_CLI_STACK_SIZE,
                            RHINO_CONFIG_USER_PRI_MAX);
*/
    return krhino_task_create(&aos_cli_hdr, "cli", NULL,
                              RHINO_CONFIG_USER_PRI_MAX, 0, aos_cli_stack,
                              sizeof(aos_cli_stack) / sizeof(cpu_stack_t),
                              (task_entry_t)cli_main, 1);
}

int aos_cli_init(void)
{
    int ret;

    cli = (struct cli_st *)aos_malloc(sizeof(struct cli_st));
    if (cli == NULL)
    {
        return ENOMEM;
    }

    memset((void *)cli, 0, sizeof(struct cli_st));

    /* add our built-in commands */
    if ((ret = aos_cli_register_commands(
             &built_ins[0], sizeof(built_ins) / sizeof(built_ins[0]))) != 0)
    {
        goto init_general_err;
    }

    ret = aos_cli_task_create();
    if (ret != 0)
    {
        aos_cli_printf("Error: Failed to create cli thread: %d\r\n", ret);
        goto init_general_err;
    }

    cli->initialized = 1;
    cli->echo_disabled = 0;

#ifdef CONFIG_AOS_CLI_BOARD
    board_cli_init();
#endif

    log_cli_init();

#ifdef CONIFG_GENIE_MESH_BINARY_CMD
    aos_timer_new(&uart_frame_end_timer, uart_frame_end_timer_cb, NULL, 50, 0);
#endif

    return 0;

init_general_err:
    if (cli)
    {
        aos_free(cli);
        cli = NULL;
    }

    return ret;
}

const char *aos_cli_get_tag(void)
{
    return esc_tag;
}

#if defined BUILD_BIN || defined BUILD_KERNEL
int aos_cli_printf(const char *msg, ...)
{
    va_list ap;

#if defined CONIFG_GENIE_MESH_BINARY_CMD || defined CONFIG_GENIE_MESH_AT_CMD || defined CONIFG_GENIE_MESH_USER_CMD
    va_start(ap, msg);
    vprintf(msg, ap);
    va_end(ap);

    fflush(stdout);
#else
    char *pos, message[256];
    int sz;
    int len;

    memset(message, 0, 256);

    sz = 0;
    if (esc_tag_len)
    {
        strcpy(message, esc_tag);
        sz = strlen(esc_tag);
    }
    pos = message + sz;

    va_start(ap, msg);
    len = vsnprintf(pos, 256 - sz, msg, ap);
    va_end(ap);

    if (len <= 0)
    {
        return 0;
    }
    cli_putstr(message);
#endif

    return 0;
}
#endif

int cli_putstr(char *msg)
{
    uart_dev_t uart_stdio;

    memset(&uart_stdio, 0, sizeof(uart_stdio));
    uart_stdio.port = 0;

    if (msg[0] != 0)
    {

#ifdef WITH_LWIP_TELNETD
        extern void TelnetWriteString(char *string);
        TelnetWriteString(msg);
#endif

        hal_uart_send(&uart_stdio, (void *)msg, strlen(msg), HAL_WAIT_FOREVER);
    }

    return 0;
}

int cli_getchar(char *inbuf)
{
    int ret = -1;
    uint32_t recv_size = 0;
    uart_dev_t uart_stdio;

    memset(&uart_stdio, 0, sizeof(uart_stdio));
    uart_stdio.port = 0;

    ret = hal_uart_recv_II(&uart_stdio, inbuf, 1, &recv_size, HAL_WAIT_FOREVER);
    if ((ret == 0) && (recv_size == 1))
    {
#ifdef CONIFG_GENIE_MESH_BINARY_CMD
        if (frame_buff_index > FRAME_BUFFER_SIZE)
        {
            frame_buff_index = 0;
        }

        frame_buff[frame_buff_index++] = (unsigned char)*inbuf;
        aos_timer_stop(&uart_frame_end_timer);
        aos_timer_start(&uart_frame_end_timer);
#endif
        return 1;
    }
    else
    {
        return 0;
    }
}
