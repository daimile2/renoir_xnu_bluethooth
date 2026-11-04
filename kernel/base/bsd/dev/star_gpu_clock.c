/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2013, 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (C) 2012 ARM Limited (native IO ordering / cycle delay).
 * MiCode pin a9244def8a5178edee9ed4a30d60034de8286cbb:
 * gpucc-shima.c, clk-rcg2.c, regmap-mmio.c, arm64 io.h/delay.c.
 * Both selected RCGs have enable_safe_config=true, mnd_width=0, hid_width=5,
 * and no FORCE_ENABLE_RCG/HW_CLK_CTRL_MODE flags. This is not the complete
 * clock/regulator framework or GPUCC PLL probe.
 */
#include <sys/star_gpu_log.h>
#include <sys/star_gpu_clock.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <kern/clock.h>
struct native_rate { uint32_t hz, source, divisor; };
static const struct native_rate gmu_rates[] = {
    {19200000, 0, 1}, {200000000, 6, 2}, {500000000, 3, 1}
};
static const struct native_rate hub_rates[] = {
    {150000000, 6, 3}, {240000000, 5, 4}, {300000000, 5, 3}
};
/* gcc-shima: mnd_width=8 but all four rates use integer division (m=n=0). */
static const struct native_rate ufs_axi_rates[] = {
    {25000000,6,23}, {75000000,6,7}, {150000000,6,3}, {300000000,6,1}
};
static const struct native_rate cxo = {19200000, 0, 1};
static uint32_t reg_read(volatile uint32_t *p)
{
    uint32_t value = *p;
    uintptr_t dependency;
    __asm__ volatile("dmb oshld" ::: "memory");
    __asm__ volatile("eor %0, %1, %1\n\tcbnz %0, ."
        : "=r" (dependency) : "r" ((uintptr_t)value) : "memory");
    return value;
}
static void reg_write(volatile uint32_t *p, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *p = value;
}
static void update_bits(volatile uint32_t *p, uint32_t mask, uint32_t value)
{
    uint32_t before = reg_read(p);
    uint32_t after = (before & ~mask) | (value & mask);
    if (after != before) reg_write(p, after); /* Native regmap_update_bits. */
}
static void native_udelay_one(void)
{
    uint64_t cycles;
    clock_interval_to_absolutetime_interval(1, NSEC_PER_USEC, &cycles);
    uint64_t start = mach_absolute_time();
    while (mach_absolute_time() - start < cycles)
        __asm__ volatile("yield" ::: "memory"); /* Native __delay final loop. */
}
static volatile uint32_t *command(struct star_gpu_rcg *rcg)
{
    return (volatile uint32_t *)(rcg->gpucc +
        (rcg->kind == STAR_GPU_RCG_GMU ? 0x1120 :
         rcg->kind == STAR_GPU_RCG_HUB ? 0x117c : 0x77024));
}
static const struct native_rate *find_rate(struct star_gpu_rcg *rcg, uint32_t hz)
{
    const struct native_rate *table;
    unsigned count;
    if (rcg->kind == STAR_GPU_RCG_GMU) {
        table = gmu_rates; count = sizeof(gmu_rates)/sizeof(gmu_rates[0]);
    } else if (rcg->kind == STAR_GPU_RCG_HUB) {
        table = hub_rates; count = sizeof(hub_rates)/sizeof(hub_rates[0]);
    } else if (rcg->kind == STAR_GPU_RCG_UFS_AXI) {
        table = ufs_axi_rates; count = sizeof(ufs_axi_rates)/sizeof(ufs_axi_rates[0]);
    } else return NULL;
    for (unsigned i = 0; i < count; i++) if (hz <= table[i].hz) return &table[i];
    return NULL;
}
static void force_enable(struct star_gpu_rcg *rcg)
{
    volatile uint32_t *cmd = command(rcg);
    update_bits(cmd, 2, 2);
    for (unsigned count = 500; count > 0; count--) {
        if (!(reg_read(cmd) & (1U << 31))) return;
        native_udelay_one();
    }
    /* Native set_force_enable warns but returns its original regmap result
     * (0 for MMIO); enable/disable do not promote this to a new error gate. */
    star_gpu_log("STAR GPU RCG: native root-enable status timeout\n");
}
static int configure(struct star_gpu_rcg *rcg, const struct native_rate *rate)
{
    volatile uint32_t *cmd = command(rcg);
    const uint32_t mask = 0x1f | 0x700 | 0x3000 | (1U << 20);
    update_bits(cmd + 1, mask, rate->divisor | (rate->source << 8));
    update_bits(cmd, 1, 1);
    for (unsigned count = 500; count > 0; count--) {
        if (!(reg_read(cmd) & 1)) return 0;
        native_udelay_one();
    }
    star_gpu_log("STAR GPU RCG: native configuration update timeout\n");
    return EBUSY;
}
int star_gpu_rcg_set_rate(struct star_gpu_rcg *rcg, uint32_t requested_hz, int prepared)
{
    const struct native_rate *rate = find_rate(rcg, requested_hz);
    if (rate == NULL) return EINVAL;
    if (!prepared) {
        rcg->current_hz = requested_hz;
        return 0; /* Native enable_safe_config deferred update, no MMIO. */
    }
    int error = configure(rcg, rate);
    if (!error) rcg->current_hz = requested_hz;
    return error;
}
int star_gpu_rcg_enable(struct star_gpu_rcg *rcg)
{
    const struct native_rate *rate = find_rate(rcg, rcg->current_hz);
    if (rate == NULL) return EINVAL;
    if (rcg->current_hz == cxo.hz) rate = &cxo;
    force_enable(rcg);
    int error = configure(rcg, rate);
    update_bits(command(rcg), 2, 0);
    return error;
}
void star_gpu_rcg_disable(struct star_gpu_rcg *rcg)
{
    force_enable(rcg);
    int error = configure(rcg, &cxo);
    if (error) star_gpu_log("STAR GPU RCG: native CXO configuration failed\n");
    update_bits(command(rcg), 2, 0);
}
