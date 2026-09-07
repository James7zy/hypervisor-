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
static u32 arrived[2], batch_start, stress_done[2];
#define BATCHES 128U
#define BATCH_BYTES 32U
#define ROUNDS_PER_BATCH 32U
#define TOTAL_BYTES (BATCHES * BATCH_BYTES)
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
        u64 start = now();
        while (!(r32(UART + 0x18) & RXFE)) {
            if (now() - start >= 5 * freq) { fail(c, DEADLINE); break; }
            u32 ch = r32(UART) & 0xff;
            if (!load(&mode)) {
                if (ch != 'G' && ch != 'S') fail(c, RX_DATA);
                store(&mode, ch);
            } else {
                u32 i = load(&rx_bytes);
                if (i >= TOTAL_BYTES || ch != 'a' + ((i + VSPI_VM_ID * 7) % 26))
                    fail(c, RX_DATA);
                store(&rx_bytes, i + 1);
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
/* All handshakes have counter deadlines, independent of working IRQ timers. */
static void check_wait(u32 c, u64 start)
{
    if (now() - start >= 5 * freq) fail(c, DEADLINE);
    if (load(&errors[0]) || load(&errors[1])) {
        if (c == 0) report_error();
        idle();
    }
}
static void barrier(u32 c, u32 *phase)
{
    store(&arrived[c], ++*phase);
    u64 start = now();
    while (load(&arrived[1 - c]) < *phase) check_wait(c, start);
    check_wait(c, start);
}
static void numbered(const char *s, u32 n)
{
    puts("VSPI VM"); decimal(VSPI_VM_ID); puts(" "); puts(s); puts(" ");
    decimal(n); puts("\n");
}
static u64 r64(u64 a)
{
    u64 v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v;
}
static void uart_concurrent_access(void)
{
    (void)r32(UART + 0x18); (void)r32(UART + 0x3c); (void)r32(UART + 0x40);
    w32(UART + 0x38, RX_MASK);
    w32(UART + 0x24, 1); w32(UART + 0x28, 0); w32(UART + 0x2c, 0x70);
    w32(UART + 0x30, 0x301); w32(UART + 0x34, 0);
}
static void gic_round(u32 c, u32 round, u32 *phase)
{
    const u64 rd0 = GICR + SGI_FRAME; /* Both CPUs intentionally use frame 0. */
    const u64 route64 = GICD + 0x6200;
    barrier(c, phase);
    w32(GICD + 0x108, 1U << c);
    w32(rd0 + 0x200, 1U << (5 + c));
    w32(route64 + c * 4, (c ? 0x22220000U : 0x11110000U) + round);
    barrier(c, phase);
    u64 expected = ((u64)(0x22220000U + round) << 32) | (0x11110000U + round);
    if ((r32(GICD + 0x108) & 3) != 3 || (r32(rd0 + 0x200) & 0x60) != 0x60 ||
        r64(route64) != expected) fail(c, GIC_STATE);
    barrier(c, phase);
    w32(GICD + 0x188, 1U << c);
    w32(rd0 + 0x280, 1U << (5 + c));
    barrier(c, phase);
    if ((r32(GICD + 0x108) & 3) || (r32(rd0 + 0x200) & 0x60)) fail(c, GIC_STATE);
    if (c == 1) uart_concurrent_access();
    barrier(c, phase);
}
static void stress(u32 c)
{
    u32 phase = 0;
    u32 ticks[2] = { load(&timer_count[0]), load(&timer_count[1]) };
    for (u32 b = 0; b < BATCHES; ++b) {
        if (c == 0) {
            numbered("RX READY", b);
            store(&batch_start, b + 1);
        } else {
            u64 start = now();
            while (load(&batch_start) < b + 1) check_wait(c, start);
        }
        for (u32 r = 0; r < ROUNDS_PER_BATCH; ++r)
            gic_round(c, b * ROUNDS_PER_BATCH + r, &phase);
        if (c == 0) {
            u64 start = now();
            while (load(&rx_bytes) < (b + 1) * BATCH_BYTES) check_wait(c, start);
            if (load(&rx_bytes) != (b + 1) * BATCH_BYTES) fail(c, RX_DATA);
            check_wait(c, start);
            numbered("RX PASS", b);
        } else {
            /* Keep device accesses concurrent with paced host RX even if the
             * GIC rounds finish before the first chunk reaches the guest. */
            u64 start = now();
            while (load(&rx_bytes) < (b + 1) * BATCH_BYTES) {
                uart_concurrent_access();
                check_wait(c, start);
            }
        }
    }
    /* End the rounds on BOTH participants, then exercise empty-FIFO masks. */
    barrier(c, &phase);
    for (u32 i = 0; i < 256; ++i) {
        if (c == 1) w32(UART + 0x38, (i & 1) ? RX_MASK : 0);
        else {
            u32 mask = r32(UART + 0x38);
            if (mask != 0 && mask != RX_MASK) fail(c, GIC_STATE);
            if ((r32(UART + 0x3c) | r32(UART + 0x40)) & RX_MASK) fail(c, RX_DATA);
            w32(UART + 0x44, RX_MASK);
        }
        barrier(c, &phase);
    }
    if (c == 1) w32(UART + 0x38, RX_MASK);
    barrier(c, &phase);
    if (!(r32(UART + 0x18) & RXFE) ||
        ((r32(UART + 0x3c) | r32(UART + 0x40)) & RX_MASK)) fail(c, RX_DATA);
    store(&stress_done[c], 1);
    if (c == 0) {
        u64 start = now();
        while (!load(&stress_done[1])) check_wait(c, start);
        if (load(&timer_count[0]) - ticks[0] < 5 || load(&timer_count[1]) - ticks[1] < 5)
            fail(c, EARLY_TIMER);
        if (load(&rx_bytes) != TOTAL_BYTES) fail(c, RX_DATA);
        report_error();
        marker("STRESS PASS");
    }
}
void vspi_secondary(void)
{
    timer_setup(1);
    store(&secondary_ready, 1);
    u64 start = now();
    while (!load(&mode)) {
        if (now() - start >= 180 * freq) { fail(1, DEADLINE); idle(); }
    }
    if (load(&mode) == 'S') stress(1);
    idle();
}
void vspi_primary(void)
{
    freq = READ(cntfrq_el0);
    w32(GICD, 0x12); w32(GICD + 0x84, 2); w32(GICD + 0x104, 2);
    *(volatile unsigned char *)(GICD + 0x400 + UART_INTID) = 0xa0;
    w32(UART + 0x30, 0x301); w32(UART + 0x44, RX_MASK); w32(UART + 0x38, RX_MASK);
    timer_setup(0);
    w32(GICD + 0x188, 3); w32(GICR + SGI_FRAME + 0x280, 0x60);
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
    if (load(&mode) == 'S') stress(0);
    marker("DONE");
    idle();
}
