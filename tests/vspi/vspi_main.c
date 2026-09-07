/* SPDX-License-Identifier: TBD */
/* Guest-observable static vSPI prerequisite tests; no EL2 private-state seam. */
typedef unsigned int u32;
typedef unsigned long u64;
#define UART 0x09000000UL
#define GICD 0x08000000UL
#define GICR 0x080A0000UL
#define GICR_STRIDE 0x20000UL
#define SGI_FRAME 0x10000UL
#define TIMER_INTID 27U
#define UART_INTID 33U
#define TEST_SGI 1U
#define RX_MASK 0x50U
#define RXFE (1U << 4)
#define READ(reg) ({ u64 v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })
#define WRITE(reg, v) __asm__ volatile("msr " #reg ", %0" :: "r"((u64)(v)) : "memory")
#define load(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define store(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

enum failure {
    EARLY_TIMER = 1, DEADLINE, CPU_ON, RX_DATA, GIC_STATE,
    UNEXPECTED_IRQ, UART_REPLAY, EXCEPTION
};
static u64 freq;
static u32 errors[2], timer_count[2], secondary_ready, mode, uart_irqs, rx_bytes;
extern char vspi_vectors[], vspi_secondary_entry[];
/* Explicit addressing keeps MMIO within the EL2 ISV-valid decoder subset. */
static void w32(u64 a, u32 v) { __asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(a) : "memory"); }
static u32 r32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static u32 cpu(void) { return (u32)READ(mpidr_el1) & 1U; }
static u64 now(void) { __asm__ volatile("isb" ::: "memory"); return READ(cntvct_el0); }
static void fail(u32 c, u32 e) { if (!load(&errors[c])) store(&errors[c], e); }
static void puts(const char *s) { while (*s) w32(UART, (u32)*s++); }
static void decimal(u32 n)
{
    char s[10]; u32 i = 0;
    do { s[i++] = '0' + n % 10; n /= 10; } while (n);
    while (i) w32(UART, (u32)s[--i]);
}
static void marker(const char *s)
{
    puts("VSPI VM"); decimal(VSPI_VM_ID); puts(" "); puts(s); puts("\n");
}
static void idle(void) __attribute__((noreturn));
static void idle(void) { for (;;) __asm__ volatile("wfi"); }
static void report_error(void)
{
    u32 e = load(&errors[0]);
    if (!e) e = load(&errors[1]);
    if (e) {
        puts("VSPI VM"); decimal(VSPI_VM_ID); puts(" FAIL "); decimal(e); puts("\n");
        idle();
    }
}
static void timer_setup(u32 c)
{
    WRITE(vbar_el1, vspi_vectors);
    WRITE(icc_sre_el1, 7); __asm__ volatile("isb" ::: "memory");
    WRITE(icc_pmr_el1, 0xff);
    WRITE(icc_ctlr_el1, 0); /* Combined EOI: TC-trapped DIR is not emulated. */
    WRITE(icc_igrpen1_el1, 0); __asm__ volatile("isb" ::: "memory");
    WRITE(cntv_tval_el0, freq / 100); WRITE(cntv_ctl_el0, 1);
    u64 start = now();
    while (now() - start < freq / 20) { }
    u64 rd = GICR + c * GICR_STRIDE;
    w32(rd + 0x14, 0);
    w32(rd + SGI_FRAME + 0x80, (1U << TIMER_INTID) | (1U << TEST_SGI));
    WRITE(icc_igrpen1_el1, 1); __asm__ volatile("isb" ::: "memory");
    w32(rd + SGI_FRAME + 0x100, (1U << TIMER_INTID) | (1U << TEST_SGI));
    __asm__ volatile("dsb sy; isb; msr daifclr, #2" ::: "memory");
    start = now();
    while (load(&timer_count[c]) < 5) {
        if (now() - start >= 5 * freq) { fail(c, EARLY_TIMER); break; }
        if (load(&errors[c])) break;
    }
}
void vspi_irq_handler(void)
{
    u32 c = cpu();
    u64 iar = READ(icc_iar1_el1);
    u32 id = (u32)iar & 0xffffffU;
    if (id == 1023) return;
    if (id == TIMER_INTID) {
        WRITE(cntv_tval_el0, freq / 100); WRITE(cntv_ctl_el0, 1);
        __asm__ volatile("isb" ::: "memory");
    } else if (id == UART_INTID && c == 0) {
        store(&uart_irqs, load(&uart_irqs) + 1);
        w32(UART + 0x44, RX_MASK);
        while (!(r32(UART + 0x18) & RXFE)) {
            u32 ch = r32(UART) & 0xff;
            if (!load(&mode)) {
                if (ch != 'G') fail(c, RX_DATA);
                store(&mode, ch);
            } else {
                fail(c, RX_DATA);
                store(&rx_bytes, load(&rx_bytes) + 1);
            }
        }
    } else { fail(c, UNEXPECTED_IRQ); }
    WRITE(icc_eoir1_el1, iar);
    __asm__ volatile("isb" ::: "memory");
    if (id == TIMER_INTID) store(&timer_count[c], load(&timer_count[c]) + 1);
}
void vspi_exception(void)
{
    fail(cpu(), EXCEPTION);
    idle();
}
void vspi_secondary(void)
{
    timer_setup(1);
    store(&secondary_ready, 1);
    u64 start = now();
    while (!load(&mode)) {
        if (now() - start >= 180 * freq) { fail(1, DEADLINE); idle(); }
    }
    idle();
}
void vspi_primary(void)
{
    freq = READ(cntfrq_el0);
    w32(GICD, 0x12); w32(GICD + 0x84, 2); w32(GICD + 0x104, 2);
    *(volatile unsigned char *)(GICD + 0x400 + UART_INTID) = 0xa0;
    w32(UART + 0x30, 0x301); w32(UART + 0x44, RX_MASK); w32(UART + 0x38, RX_MASK);
    timer_setup(0);
    register u64 x0 __asm__("x0") = 0xC4000003UL;
    register u64 x1 __asm__("x1") = 1;
    register u64 x2 __asm__("x2") = (u64)vspi_secondary_entry;
    register u64 x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    if (x0) fail(0, CPU_ON);
    u64 start = now();
    u64 boot_retry = start - freq;
    while (!load(&mode)) {
        if (VSPI_VM_ID == 0 && now() - boot_retry >= freq / 4) {
            marker("BOOT"); boot_retry = now();
        }
        if (now() - start >= 180 * freq) { fail(0, DEADLINE); report_error(); }
    }
    marker("ACTIVE");
    start = now();
    while (!load(&secondary_ready)) {
        if (now() - start >= 5 * freq) { fail(0, DEADLINE); break; }
        if (load(&errors[0]) || load(&errors[1])) break;
    }
    report_error();
    marker("GATE PASS");
    marker("DONE");
    idle();
}
