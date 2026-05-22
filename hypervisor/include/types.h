/* SPDX-License-Identifier: TBD */
#ifndef HV_TYPES_H
#define HV_TYPES_H

typedef unsigned char        u8;
typedef unsigned short       u16;
typedef unsigned int         u32;
typedef unsigned long        u64;

typedef signed char          s8;
typedef signed short         s16;
typedef signed int           s32;
typedef signed long          s64;

typedef unsigned long        uintptr_t;
typedef unsigned long        size_t;
typedef signed long          ssize_t;

typedef _Bool                bool;
#define true                 ((bool)1)
#define false                ((bool)0)

#define NULL                 ((void *)0)

#endif /* HV_TYPES_H */
