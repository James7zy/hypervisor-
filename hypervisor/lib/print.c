/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <uart.h>
#include <spinlock.h>

#include <stdarg.h>

/* Serialize printk across pCPUs so concurrent EL2 output is not interleaved
 * character-by-character (M3.5). The only shared sink is the PL011. */
static struct spinlock print_lock = SPINLOCK_INIT;

static int emit_char(char c)
{
    uart_putc(c);
    return 1;
}

static int emit_string(const char *s)
{
    int n = 0;
    if (!s)
        s = "(null)";
    while (*s)
        n += emit_char(*s++);
    return n;
}

static int emit_signed(s64 v)
{
    char buf[24];
    int len = 0, n = 0;
    u64 mag;

    if (v < 0) {
        n += emit_char('-');
        mag = (u64)(-(v + 1)) + 1;
    } else {
        mag = (u64)v;
    }
    do {
        buf[len++] = (char)('0' + (mag % 10));
        mag /= 10;
    } while (mag);
    while (len--)
        n += emit_char(buf[len]);
    return n;
}

static int emit_unsigned(u64 v)
{
    char buf[24];
    int len = 0, n = 0;
    do {
        buf[len++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (len--)
        n += emit_char(buf[len]);
    return n;
}

static int emit_hex(u64 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    int len = 0, n = 0;
    do {
        buf[len++] = digits[v & 0xF];
        v >>= 4;
    } while (v);
    while (len--)
        n += emit_char(buf[len]);
    return n;
}

int printk(const char *fmt, ...)
{
    va_list ap;
    int n = 0;

    spin_lock(&print_lock);
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            n += emit_char(*fmt++);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case '\0': goto done;
        case '%':  n += emit_char('%'); break;
        case 'c':  n += emit_char((char)va_arg(ap, int)); break;
        case 's':  n += emit_string(va_arg(ap, const char *)); break;
        case 'd':  n += emit_signed((s64)va_arg(ap, int)); break;
        case 'u':  n += emit_unsigned((u64)va_arg(ap, unsigned int)); break;
        case 'x':  n += emit_hex((u64)va_arg(ap, unsigned int)); break;
        case 'l':
            fmt++;
            if (*fmt == 'x') {
                n += emit_hex((u64)va_arg(ap, unsigned long));
            } else {
                n += emit_char('%'); n += emit_char('l');
                if (*fmt) n += emit_char(*fmt); else goto done;
            }
            break;
        default:
            n += emit_char('%'); n += emit_char(*fmt); break;
        }
        fmt++;
    }
done:
    va_end(ap);
    spin_unlock(&print_lock);
    return n;
}
