/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SYS_STAR_GPU_CLOCK_PROVIDER_H_
#define _SYS_STAR_GPU_CLOCK_PROVIDER_H_
#include <sys/types.h>
/* Production GMU/SMMU ancestor closure plus all registered GPUCC clocks.
 * Controller binding numbers are resolved explicitly, not cast to this enum. */
enum star_gpu_clock_controller { STAR_CC_RPMH,STAR_CC_GCC,STAR_CC_GPUCC };
enum star_gpu_clock_id {
    STAR_CLK_XO, STAR_CLK_XO_AO, STAR_CLK_GPLL0, STAR_CLK_GPLL0_EVEN,
    STAR_CLK_GCC_MAIN, STAR_CLK_GCC_DIV, STAR_CLK_CFG_AHB,
    STAR_CLK_MEMNOC, STAR_CLK_SNOC, STAR_CLK_DDR_AXI, STAR_CLK_PLL0, STAR_CLK_PLL1,
    STAR_CLK_GMU_SRC, STAR_CLK_HUB_SRC, STAR_CLK_AHB_DIV, STAR_CLK_INT_DIV,
    STAR_CLK_AHB, STAR_CLK_GMU, STAR_CLK_CXO, STAR_CLK_HUB,
    STAR_CLK_HUB_INT, STAR_CLK_SMMU, STAR_CLK_CRC_AHB, STAR_CLK_CX_SNOC,
    STAR_CLK_CXO_AON, STAR_CLK_GX_GMU, STAR_CLK_MND0, STAR_CLK_MND1,
    STAR_CLK_SLEEP, STAR_CLK_UFS_AXI_SRC, STAR_CLK_AGGRE_UFS_AXI,
    STAR_CLK_IPA, STAR_CLK_COUNT
};
int star_gpu_clock_provider_open(void);
int star_gpu_clock_resolve(enum star_gpu_clock_controller,unsigned binding,
    enum star_gpu_clock_id *);
/* Bulk clients prepare every clock before enabling any, as native CCF. */
int star_gpu_clock_prepare(enum star_gpu_clock_id);
void star_gpu_clock_unprepare(enum star_gpu_clock_id);
int star_gpu_clock_enable(enum star_gpu_clock_id);
void star_gpu_clock_disable(enum star_gpu_clock_id);
int star_gpu_clock_prepare_enable(enum star_gpu_clock_id);
void star_gpu_clock_disable_unprepare(enum star_gpu_clock_id);
int star_gpu_clock_set_rate(enum star_gpu_clock_id,uint32_t hz);
uint32_t star_gpu_clock_get_rate(enum star_gpu_clock_id);
void star_gpu_clock_sync_state(int gcc_consumers_bound,int gpucc_consumers_bound);
int star_gpu_cx_get(void);
int star_gpu_cx_put(void);
int star_gpu_cx_is_enabled(void);
int star_gpu_cx_get_hw_mode(void);
int star_gpu_cx_set_hw_mode(int hardware_mode);
#endif
