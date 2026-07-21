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
static void hv_shell_dispatch(const char *line);

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

static void hv_shell_dispatch(const char *line)
{
    (void)line;   /* command table arrives in Task 3 */
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
        printk(HV_SHELL_PROMPT);
        return;
    }

    /* Backspace: accept both BS (0x08) and DEL (0x7F) -- terminals disagree
     * about which one the key sends. Erase visually with "\b \b": back up,
     * overwrite the glyph with a space, back up again. */
    if (ch == 0x08U || ch == 0x7FU) {
        if (shell_line_len > 0U) {
            shell_line_len--;
            console_putc('\b');
            console_putc(' ');
            console_putc('\b');
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
            printk(HV_SHELL_PROMPT);
        }
    }
}
