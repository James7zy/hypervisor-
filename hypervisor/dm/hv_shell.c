/* SPDX-License-Identifier: TBD */
/*
 * EL2 debug shell -- see hv_shell.h for the console-ownership contract.
 *
 * Runs entirely inside el2_irq_handler on pCPU0 (the only core the physical
 * PL011 SPI is routed to), so none of the state here needs locking. Output
 * goes through printk/console_putc, which serialize against guest TX under
 * the shared console lock (lib/print.c).
 */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <vm_config.h>   /* struct vm_config (pcpu_base), for vm_list */
#include <vuart.h>
#include <hv_shell.h>

#define HV_SHELL_PROMPT "hv> "

bool shell_active = false;

/*
 * Line buffer. 128 bytes is far more than any command this shell accepts
 * ("vm_console 1" is 12); the size exists to bound the buffer, not to
 * accommodate long input. On overflow we submit the line rather than
 * truncating silently forever -- the user sees their command run (or fail to
 * parse) instead of the shell appearing to freeze.
 */
#define HV_SHELL_LINE_MAX 128U

static char shell_line[HV_SHELL_LINE_MAX];
static u32  shell_line_len;

/* Forward decl: defined in Task 3, dispatches one completed line. */
static void hv_shell_dispatch(char *line);

/* Compare NUL-terminated a against b. Returns true when equal. This repo has
 * no libc -- lib/string.c provides memset and nothing else -- so the command
 * table needs its own comparison. */
static bool str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/*
 * Parse a decimal u32. Returns true on success and writes *out; returns false
 * for an empty string or any non-digit character, so "vm_console abc" and
 * "vm_console" are both rejected rather than silently parsed as 0.
 * No overflow handling beyond the digit cap: inputs here are VM ids (0..1).
 */
static bool parse_u32(const char *s, u32 *out)
{
    u32 val = 0U;
    u32 digits = 0U;

    if (s == NULL || *s == '\0') {
        return false;
    }
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return false;
        }
        val = (val * 10U) + (u32)(*s - '0');
        digits++;
        if (digits > 9U) {   /* far beyond any valid VM id; refuse silently-wrong input */
            return false;
        }
        s++;
    }
    *out = val;
    return true;
}

struct hv_shell_cmd {
    const char *name;
    const char *help;
    void (*fn)(const char *arg);   /* arg = text after the command name, "" if none */
};

static void cmd_help(const char *arg);
static void cmd_vm_list(const char *arg);
static void cmd_vm_console(const char *arg);

static const struct hv_shell_cmd hv_shell_cmds[] = {
    { "help",       "list commands",                                cmd_help },
    { "vm_list",    "list VMs, their pCPUs, state and console focus", cmd_vm_list },
    { "vm_console", "vm_console <n> - attach console input to VM n", cmd_vm_console },
};

#define HV_SHELL_NR_CMDS (sizeof(hv_shell_cmds) / sizeof(hv_shell_cmds[0]))

static void cmd_help(const char *arg)
{
    (void)arg;
    for (u32 i = 0U; i < (u32)HV_SHELL_NR_CMDS; i++) {
        printk("  %s\t%s\n", hv_shell_cmds[i].name, hv_shell_cmds[i].help);
    }
}

/*
 * One row per VM. The ID column is deliberately the same number vm_console
 * takes as its argument -- discovering that mapping is this command's whole
 * reason for existing.
 *
 * Deliberately NOT shown: config->vmid (the Stage-2 VMID). It is 1-based
 * while the id here is 0-based, so printing both side by side invites exactly
 * the "which number do I type?" confusion this command exists to remove.
 *
 * STATE says "halted", not "off", even though it reads vm->off: that flag
 * means "some vCPU of this VM called CPU_OFF/SYSTEM_OFF/SYSTEM_RESET, so all
 * of the VM's pCPUs are parked in wfi" (psci.c psci_power_down). "off" would
 * imply an orderly whole-VM shutdown, which is not what the flag guarantees.
 *
 * printk has no width specifiers (%4u etc. are unsupported -- see printk.h),
 * so the columns are aligned with literal spaces in the format strings. That
 * is sound only because every value here is a single digit: NR_VMS <= 2 and
 * NR_CPUS == 4 by static assert (vm.h), so ids and pCPU numbers never widen.
 */
static void cmd_vm_list(const char *arg)
{
    (void)arg;

    printk("  ID  pCPUs  STATE   CONSOLE\n");
    for (u32 i = 0U; i < (u32)NR_VMS; i++) {
        const struct vm *m = &vm[i];
        u32 first = (u32)m->config->pcpu_base;

        bool focused = (i == console_focus);
        const char *state = (m->off != 0U) ? "halted" : "on";

        /* The state field is padded to the marker column ONLY when a marker
         * follows, so an unmarked row ends right after its state word rather
         * than trailing blanks into the serial log. "halted" is 6 chars and
         * "on" is 2, hence the 4-space difference. */
        printk("   %u    %u-%u  %s%s\n",
               (unsigned)m->id,
               (unsigned)first,
               (unsigned)(first + VCPUS_PER_VM - 1U),
               state,
               focused ? ((m->off != 0U) ? "  *" : "      *") : "");
    }
}

static void cmd_vm_console(const char *arg)
{
    u32 n;

    if (!parse_u32(arg, &n) || n >= (u32)NR_VMS) {
        /* Name the valid range rather than just rejecting: a bad id is the
         * moment the user most needs to know what the ids are. Derived from
         * NR_VMS so it stays correct under every build profile (the SVM
         * profiles set NR_VMS=1, where this correctly prints "0-0"). */
        printk("Error: invalid VM id (valid: 0-%u).\n", (unsigned)(NR_VMS - 1));
        return;
    }
    console_focus = n;
    printk("[hv] console: VM%u\n", (unsigned)n);
    shell_active = false;
}

void hv_shell_enter(void)
{
    shell_active = true;
    /* One printk, not two: printk locks per call, so splitting this would let
     * a guest on another pCPU interleave output between the newline and the
     * prompt. */
    printk("\n" HV_SHELL_PROMPT);
}

void hv_shell_exit(void)
{
    shell_active = false;
    printk("\n");
}

/*
 * Split the line into a command name and the remaining argument text, then
 * look the name up in the table. The split is destructive (the first space
 * becomes a NUL), which is fine: shell_line is scratch space reset after
 * every dispatch.
 */
static void hv_shell_dispatch(char *line)
{
    char *p = line;
    const char *arg = "";

    /* Skip leading spaces; an all-blank line is a no-op (just reprompt). */
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return;
    }

    /* Find the end of the command name. */
    char *name = p;
    while (*p != '\0' && *p != ' ') {
        p++;
    }
    if (*p == ' ') {
        *p = '\0';   /* terminate the name */
        p++;
        while (*p == ' ') {   /* skip spaces before the argument */
            p++;
        }
        arg = p;
    }

    for (u32 i = 0U; i < (u32)HV_SHELL_NR_CMDS; i++) {
        if (str_eq(name, hv_shell_cmds[i].name)) {
            hv_shell_cmds[i].fn(arg);
            return;
        }
    }
    printk("Error: Invalid command.\n");
}

void hv_shell_rx(u8 ch)
{
    /* Enter: terminate, dispatch, reset, reprompt. Accept both CR and LF --
     * a terminal in raw mode typically sends CR, but do not depend on it. */
    if (ch == '\r' || ch == '\n') {
        printk("\n");
        shell_line[shell_line_len] = '\0';
        hv_shell_dispatch(shell_line);
        shell_line_len = 0U;
        if (shell_active) {
            printk(HV_SHELL_PROMPT);
        }
        return;
    }

    /* Backspace: accept both BS (0x08) and DEL (0x7F) -- terminals disagree
     * about which one the key sends. Erase visually with "\b \b": back up,
     * overwrite the glyph with a space, back up again. */
    if (ch == 0x08U || ch == 0x7FU) {
        if (shell_line_len > 0U) {
            shell_line_len--;
            /* One printk, not three console_putc calls: printk locks per
             * call, so splitting "\b \b" would let a guest on another pCPU
             * interleave a byte into the middle of the erase sequence. */
            printk("\b \b");
        }
        return;
    }

    /* Printable ASCII only. Anything else (control chars, high bytes) is
     * ignored rather than echoed, so a stray escape sequence cannot corrupt
     * the visible line. */
    if (ch >= 0x20U && ch <= 0x7EU) {
        if (shell_line_len < (HV_SHELL_LINE_MAX - 1U)) {
            shell_line[shell_line_len] = (char)ch;
            shell_line_len++;
            console_putc((char)ch);
        } else {
            /* Full: submit what we have rather than silently dropping keys. */
            printk("\n");
            shell_line[shell_line_len] = '\0';
            hv_shell_dispatch(shell_line);
            shell_line_len = 0U;
            if (shell_active) {
                printk(HV_SHELL_PROMPT);
            }
        }
    }

    /* Everything else (ESC 0x1B, arrow-key escape sequences, other control
     * chars) falls through here and is dropped by design: this shell has no
     * cursor movement, history, or escape-sequence parser to feed it into,
     * so there is nothing to do with an unrecognized byte but ignore it. */
}
