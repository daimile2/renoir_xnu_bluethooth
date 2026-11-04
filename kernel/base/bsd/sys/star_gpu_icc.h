/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SYS_STAR_GPU_ICC_H_
#define _SYS_STAR_GPU_ICC_H_
#include <sys/types.h>
struct star_gpu_icc;
int star_gpu_icc_get(struct star_gpu_icc **);
/* Live pil_scm_pas: MASTER_CRYPTO_CORE0 -> SLAVE_EBI1. */
int star_gpu_icc_get_pas(struct star_gpu_icc **);
/* Actual MASTER_GFX3D -> LLCC -> EBI path, KB/s. */
int star_gpu_icc_set(struct star_gpu_icc *,uint32_t average,uint32_t peak);
void star_gpu_icc_put(struct star_gpu_icc *);
int star_gpu_icc_last_error(void);
int star_gpu_icc_consumer_error(struct star_gpu_icc *);
/* Only when the native set of all QNoC providers/consumers has bound. */
void star_gpu_icc_sync_state(void);
/* apps-SMMU parent: APPSS_PROC -> GEM_NOC_CNOC -> IMEM_CFG, active-only. */
int star_gpu_apps_smmu_power_on(void);
int star_gpu_apps_smmu_power_off(void);
#endif
