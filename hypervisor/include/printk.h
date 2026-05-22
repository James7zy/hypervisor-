/* SPDX-License-Identifier: TBD */
#ifndef HV_PRINTK_H
#define HV_PRINTK_H

/* Supports: %s %c %d %u %x %lx %%. No width, precision, floats, or %p. */
int printk(const char *fmt, ...);

#endif /* HV_PRINTK_H */
