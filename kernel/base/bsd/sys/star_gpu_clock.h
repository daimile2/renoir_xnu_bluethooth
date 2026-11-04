/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SYS_STAR_GPU_CLOCK_H_
#define _SYS_STAR_GPU_CLOCK_H_
#include <sys/types.h>
enum star_gpu_rcg_kind { STAR_GPU_RCG_GMU, STAR_GPU_RCG_HUB, STAR_GPU_RCG_UFS_AXI };
/* Native GPUCC RCG register operations only. Parent clocks, regulator votes,
 * prepare/enable reference counts and provider lock remain caller-owned.
 * current_hz must come from native recalc/current state, not a debug constant. */
struct star_gpu_rcg {
    volatile uint8_t *gpucc;
    enum star_gpu_rcg_kind kind;
    uint32_t current_hz;
};
int star_gpu_rcg_set_rate(struct star_gpu_rcg *, uint32_t requested_hz, int prepared);
int star_gpu_rcg_enable(struct star_gpu_rcg *);
void star_gpu_rcg_disable(struct star_gpu_rcg *);
#endif
