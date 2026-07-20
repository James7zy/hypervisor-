/* SPDX-License-Identifier: TBD */
#ifndef HV_PRINTK_H
#define HV_PRINTK_H

/* Supports: %s %c %d %u %x %lx %%. No width, precision, floats, or %p. */
int printk(const char *fmt, ...);

/* Emit one raw character to the physical console, serialized with printk.
 * Used by the vuart TX path so guest output and EL2 printk output never
 * interleave mid-character. */
void console_putc(char c);

#endif /* HV_PRINTK_H */
