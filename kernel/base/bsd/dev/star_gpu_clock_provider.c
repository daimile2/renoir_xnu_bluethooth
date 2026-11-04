/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2013, 2016-2020, The Linux Foundation. All rights reserved.
 * Renoir GMU/SMMU clock consumers and native ancestor closure.
 * gcc-shima.c, gpucc-shima.c, clk.c, clk-rpmh.c, clk-alpha-pll.c,
 * clk-branch.c, clk-regmap.c, vdd-class.c, gdsc-regulator.c (pinned MiCode).
 * No GPU start, invented waits, fake lock/fence or automatic platform sync.
 */
#include <sys/star_gpu_log.h>
#include <sys/star_gpu_clock_provider.h>
#include <sys/star_gpu_clock.h>
#include <sys/star_gpu_power.h>
#include <sys/star_gpu_regulator.h>
#include <sys/star_gpu_rpmh.h>
#include <sys/star_gpu_native_io.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
enum kind { XO,PLL,DIV,BRANCH,RCG,RO_DIV,BCM };
enum halt { HALT,DELAY,VOTED,SKIP };
struct clock {
    enum kind kind;
    int parent,rail;
    unsigned gpu,reg,mask,hwcg,halt,aon,dont_hold,critical;
    unsigned prepares,enables,hold,selected_vdd,registered,boot_on,was_orphan;
    uint32_t cached;
    struct star_gpu_regulator *vreg;
};
#define C(k,p,r,g,o,m,h,hc,a,d,c) { .kind=k,.parent=p,.rail=r,.gpu=g,.reg=o,.mask=m,.halt=h,.hwcg=hc,.aon=a,.dont_hold=d,.critical=c }
static struct clock clocks[STAR_CLK_COUNT]={
    [STAR_CLK_XO]=C(XO,-1,-1,0,0,0,0,0,0,0,0),
    [STAR_CLK_XO_AO]=C(XO,-1,-1,0,0,0,0,0,1,0,0),
    [STAR_CLK_GPLL0]=C(PLL,STAR_CLK_XO,STAR_GPU_CX,0,0,1,0,0,0,0,0),
    [STAR_CLK_GPLL0_EVEN]=C(DIV,STAR_CLK_GPLL0,-1,0,0x0c,0,0,0,0,0,0),
    [STAR_CLK_GCC_MAIN]=C(BRANCH,STAR_CLK_GPLL0,-1,0,0x52000,1U<<15,DELAY,0,0,0,0),
    [STAR_CLK_GCC_DIV]=C(BRANCH,STAR_CLK_GPLL0_EVEN,-1,0,0x52000,1U<<16,DELAY,0,0,0,0),
    [STAR_CLK_CFG_AHB]=C(BRANCH,-1,-1,0,0x71004,1,VOTED,1U<<1,0,0,1),
    [STAR_CLK_MEMNOC]=C(BRANCH,-1,-1,0,0x7100c,1,VOTED,1U<<1,1,1,0),
    [STAR_CLK_SNOC]=C(BRANCH,-1,-1,0,0x71018,1,HALT,0,1,1,0),
    [STAR_CLK_DDR_AXI]=C(BRANCH,-1,-1,0,0x71154,1,SKIP,1U<<1,1,0,0),
    [STAR_CLK_PLL0]=C(PLL,STAR_CLK_XO,STAR_GPU_MX,1,0,0,0,0,0,0,0),
    [STAR_CLK_PLL1]=C(PLL,STAR_CLK_XO,STAR_GPU_MX,1,0x100,0,0,0,0,0,0),
    [STAR_CLK_GMU_SRC]=C(RCG,STAR_CLK_XO,STAR_GPU_CX,1,0x1120,0,0,0,0,0,0),
    [STAR_CLK_HUB_SRC]=C(RCG,STAR_CLK_XO,STAR_GPU_CX,1,0x117c,0,0,0,0,0,0),
    [STAR_CLK_AHB_DIV]=C(RO_DIV,STAR_CLK_HUB_SRC,-1,1,0x11c0,0,0,0,0,0,0),
    [STAR_CLK_INT_DIV]=C(RO_DIV,STAR_CLK_HUB_SRC,-1,1,0x11bc,0,0,0,0,0,0),
    [STAR_CLK_AHB]=C(BRANCH,STAR_CLK_AHB_DIV,-1,1,0x1078,1,DELAY,0,0,0,0),
    [STAR_CLK_GMU]=C(BRANCH,STAR_CLK_GMU_SRC,-1,1,0x1098,1,HALT,0,1,1,0),
    [STAR_CLK_CXO]=C(BRANCH,-1,-1,1,0x109c,1,HALT,0,1,1,0),
    [STAR_CLK_HUB]=C(BRANCH,STAR_CLK_HUB_SRC,-1,1,0x1178,1,HALT,0,1,0,0),
    [STAR_CLK_HUB_INT]=C(BRANCH,STAR_CLK_INT_DIV,-1,1,0x1204,1,HALT,0,1,1,0),
    [STAR_CLK_SMMU]=C(BRANCH,-1,-1,1,0x5000,1,VOTED,0,0,0,0),
    [STAR_CLK_CRC_AHB]=C(BRANCH,STAR_CLK_AHB_DIV,-1,1,0x107c,1,VOTED,0,0,0,0),
    [STAR_CLK_CX_SNOC]=C(BRANCH,-1,-1,1,0x108c,1,VOTED,0,0,0,0),
    [STAR_CLK_CXO_AON]=C(BRANCH,-1,-1,1,0x1004,1,VOTED,0,0,0,0),
    [STAR_CLK_GX_GMU]=C(BRANCH,STAR_CLK_GMU_SRC,-1,1,0x1064,1,HALT,0,0,0,0),
    [STAR_CLK_MND0]=C(BRANCH,-1,-1,1,0x802c,1,SKIP,0,0,0,0),
    [STAR_CLK_MND1]=C(BRANCH,-1,-1,1,0x8030,1,SKIP,0,0,0,0),
    [STAR_CLK_SLEEP]=C(BRANCH,-1,-1,1,0x1090,1,VOTED,0,0,0,0),
    [STAR_CLK_UFS_AXI_SRC]=C(RCG,STAR_CLK_XO,STAR_GPU_CX,0,0x77024,0,0,0,0,0,0),
    [STAR_CLK_AGGRE_UFS_AXI]=C(BRANCH,STAR_CLK_UFS_AXI_SRC,-1,0,0x770cc,1,VOTED,2,0,0,0),
    [STAR_CLK_IPA]=C(BCM,-1,-1,0,0,0,0,0,0,0,0),
};
#undef C
static volatile uint8_t *gcc,*gpucc;
static struct star_gpu_rcg rcgs[3];
static uint32_t xo_address,ipa_address,ipa_state,ipa_sent;
static uint64_t ipa_unit;
static unsigned xo_sent,xo_state[2],cx_users;
static int opened,gcc_registered,cx_registered,cx_enabled,cx_hw_mode;
static struct star_gpu_regulator *cx_parent;
struct vdd_class {
    struct star_gpu_regulator *regulator;
    unsigned votes[7],current;
};
static struct vdd_class vdd[3]={{.current=7},{.current=7},{.current=7}};
static const unsigned corners[7]={0,48,64,128,192,256,384};
LCK_GRP_DECLARE(star_gpu_cc_group,"Star GPU clocks");
LCK_MTX_DECLARE(star_gpu_cc_lock,&star_gpu_cc_group);
static int ipa_vote(int on)
{
    unsigned next=on?(ipa_state?ipa_state:1):0;
    if(next==ipa_sent)return 0;
    struct star_gpu_rpmh_cmd cmd={ipa_address,(1U<<30)|((unsigned)on<<29)|(next&0x3fff),0};
    int e=star_gpu_rpmh_write_state(STAR_RPMH_ACTIVE,&cmd,1,on);
    if(!e)ipa_sent=next;
    return e;
}
static int prepare(int id);
static int enable_clock(int id);
static void unprepare(int id);
static void disable_clock(int id);
static volatile uint8_t *base(const struct clock *c) {return c->gpu?gpucc:gcc;}
static uint32_t read_clock(const struct clock *c,unsigned offset)
{return star_gpu_read32(base(c),c->reg+offset);}
static int pll_is_enabled(const struct clock *c)
{return !!((read_clock(c,0x38)&1)&&(read_clock(c,0)&1));}
static int pll_wait(struct clock *c)
{
    (void)read_clock(c,0);
    for(unsigned i=100;i;i--) {
        if(read_clock(c,0)&(1U<<31))return 0;
        star_gpu_native_udelay(1);
    }
    star_gpu_log("STAR GPU PLL: native lock timeout\n");return ETIMEDOUT;
}
static int pll_enable(struct clock *c)
{
    volatile uint8_t *b=base(c);
    if(read_clock(c,0x0c)&(1U<<21)) {
        star_gpu_update32(b,c->gpu?0:0x52010,c->mask,c->mask);
        return pll_wait(c);
    }
    if(pll_is_enabled(c)) {star_gpu_log("STAR GPU PLL: native already enabled\n");return 0;}
    star_gpu_update32(b,c->reg,1U<<2,1U<<2);
    star_gpu_write32(b,c->reg+0x38,1);
    int error=pll_wait(c);if(error)return error;
    star_gpu_update32(b,c->reg+0x0c,7,7);
    star_gpu_update32(b,c->reg,1,1);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
static void pll_disable(struct clock *c)
{
    volatile uint8_t *b=base(c);
    if(read_clock(c,0x0c)&(1U<<21)) {
        star_gpu_update32(b,c->gpu?0:0x52010,c->mask,0);return;
    }
    star_gpu_update32(b,c->reg,1,0);star_gpu_update32(b,c->reg+0x0c,7,0);
    star_gpu_write32(b,c->reg+0x38,0);
}
static void pll_configure(unsigned id,unsigned l,unsigned alpha)
{
    struct clock *c=&clocks[id];if(pll_is_enabled(c))return;
    const unsigned pairs[][2]={{4,l},{8,0x44},{0x40,alpha},{0x18,0x20485699},
        {0x1c,0x2261},{0x20,0x2a9a699c},{0x0c,1},{0x10,0x805},{0x2c,0x01800000}};
    for(unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0]);i++)
        star_gpu_write32(gpucc,c->reg+pairs[i][0],pairs[i][1]);
    star_gpu_update32(gpucc,c->reg,1,0);star_gpu_write32(gpucc,c->reg+0x38,0);
    star_gpu_native_wmb();star_gpu_update32(gpucc,c->reg,4,4);
}
static int branch(struct clock *c,int on)
{
    star_gpu_update32(base(c),c->reg,c->mask,on?c->mask:0);
    if(c->halt==SKIP)return 0;
    if(c->hwcg&&(read_clock(c,0)&c->hwcg))return 0;
    if(c->halt==DELAY||(!on&&c->halt==VOTED)) {star_gpu_native_udelay(10);return 0;}
    for(unsigned i=200;i;i--) {
        uint32_t status=read_clock(c,0);
        if(on?(!(status&(1U<<31))||((status&0xf0000000U)==0x20000000U)):!!(status&(1U<<31)))return 0;
        star_gpu_native_udelay(1);
    }
    star_gpu_log("STAR GPU branch: native status stuck reg=%#x\n",c->reg);return EBUSY;
}
static unsigned rcg_index(int id) {return id==STAR_CLK_GMU_SRC?0:id==STAR_CLK_HUB_SRC?1:2;}
static uint32_t rate(int id)
{
    struct clock *c=&clocks[id];
    if(c->kind==BCM)return ipa_state*ipa_unit;
    if(c->kind==XO)return 38400000/2;
    if(c->kind==PLL)return (uint32_t)((uint64_t)rate(c->parent)*read_clock(c,4)+
        (((uint64_t)rate(c->parent)*(read_clock(c,0x40)&65535))>>16));
    if(c->kind==DIV) {
        unsigned div=(read_clock(c,0)>>8)&15;
        /* Native table { raw=1, divisor=2 }; unknown raw defaults to /1. */
        return rate(c->parent)/(div==1?2:1);
    }
    if(c->kind==RO_DIV)return rate(c->parent)/((read_clock(c,0)&15)+1);
    if(c->kind==RCG) {
        unsigned cfg=read_clock(c,4),source=(cfg>>8)&7;
        if(!source&&(!c->prepares||(read_clock(c,0)&(1U<<31))))
            return c->cached?c->cached:19200000;
        if(c->parent<0)return 0;
        unsigned hid=cfg&31;
        if(!source) {
            uint32_t current=rcgs[rcg_index(id)].current_hz;
            if(id==STAR_CLK_GMU_SRC)hid=current<=19200000?1:current<=200000000?2:1;
            else if(id==STAR_CLK_UFS_AXI_SRC)hid=current<=25000000?23:current<=75000000?7:current<=150000000?3:1;
            else hid=current<=150000000?3:current<=240000000?4:3;
        }
        uint32_t parent_rate=rate(c->parent);
        return hid?(uint32_t)((uint64_t)parent_rate*2/(hid+1)):parent_rate;
    }
    return c->parent>=0?rate(c->parent):0;
}
static unsigned voltage_level(int id,uint32_t hz)
{
    static const unsigned pll_hz[]={615000000,1066000000,1500000000,1750000000,1800000000};
    static const unsigned pll_levels[]={1,3,4,5,6};
    if(clocks[id].kind==PLL) {
        for(unsigned i=0;i<5;i++)if((hz+500)/1000<=(pll_hz[i]+500)/1000)return pll_levels[i];
        return 0;
    }
    if(id==STAR_CLK_UFS_AXI_SRC)return hz<=75000000?1:hz<=150000000?3:hz<=300000000?5:0;
    if(id==STAR_CLK_GMU_SRC)return hz<=200000000?1:hz<=500000000?3:0;
    if(id==STAR_CLK_HUB_SRC)return hz<=150000000?1:hz<=240000000?3:hz<=300000000?5:0;
    return 0;
}
static unsigned vdd_index(const struct clock *c)
{return c->gpu?(c->rail==STAR_GPU_MX?2:1):0;}
static int aggregate_vdd(struct vdd_class *v)
{
    unsigned level=6;while(level&&!v->votes[level])level--;
    unsigned old=v->current;
    if(level==old)return 0;
    int error=star_gpu_regulator_set_voltage(v->regulator,corners[level],65535);
    if(error)return error;
    if(old==0||old==7)error=star_gpu_regulator_enable(v->regulator);
    else if(!level)error=star_gpu_regulator_disable(v->regulator);
    if(error) {
        /* Native rollback uses the previous corner. cur_level=NUM is an
         * uninitialized sentinel; native uv[NUM] is outside its array.
         * No fictional voltage is supplied for that invalid old index. */
        if(old<7)(void)star_gpu_regulator_set_voltage(v->regulator,corners[old],65535);
        return error;
    }
    v->current=level;return 0;
}
static int count_vdd(struct clock *c,unsigned level,int add)
{
    struct vdd_class *v=&vdd[vdd_index(c)];
    if(level>=7)return EINVAL;
    if(!add&&!v->votes[level])return EINVAL;
    if(add)v->votes[level]++;else v->votes[level]--;
    int error=aggregate_vdd(v);
    if(error){if(add)v->votes[level]--;else v->votes[level]++;}
    return error;
}
static int vote(struct clock *c,unsigned new_level)
{
    int error=count_vdd(c,new_level,1);if(error)return error;
    (void)count_vdd(c,c->selected_vdd,0);
    c->selected_vdd=new_level;return 0;
}
static int prepare(int id)
{
    struct clock *c=&clocks[id];
    if(c->prepares){c->prepares++;return 0;}
    int error=0;
    if(c->parent>=0){error=prepare(c->parent);if(error)return error;}
    if(c->vreg) {
        unsigned required=voltage_level(id,rate(id));
        if(!required){error=EINVAL;goto parent;}
        if(!c->selected_vdd)c->selected_vdd=required;
        if(required>c->selected_vdd)star_gpu_log("STAR GPU clock: native voltage-level mismatch\n");
        error=count_vdd(c,c->selected_vdd,1);if(error)goto parent;
    }
    if(c->kind==PLL&&c->gpu&&!(read_clock(c,0)&(1U<<11))) {
        error=pll_enable(c);if(error)goto parent;
        pll_disable(c);
    }
    if(c->kind==BCM){error=ipa_vote(1);if(error)goto parent;}
    c->prepares=1;return 0;
parent:if(c->parent>=0)unprepare(c->parent);return error;
}
static void unprepare(int id)
{
    struct clock *c=&clocks[id];
    if(!c->prepares){star_gpu_log("STAR GPU clock: native unbalanced unprepare\n");return;}
    if(c->prepares==1&&c->critical)return;
    if(--c->prepares)return;
    if(c->kind==BCM)(void)ipa_vote(0);
    if(c->vreg)(void)count_vdd(c,c->selected_vdd,0);
    if(c->parent>=0)unprepare(c->parent);
}
static int xo_vote(struct clock *c,int on)
{
    unsigned peer=c->aon?0:1,own=c->aon?1:0;
    unsigned state=on?(c->aon?6U:7U):0;
    unsigned next=state|xo_state[peer];
    struct star_gpu_rpmh_cmd cmd={xo_address,0,0};
    for(unsigned state=0;state<3;state++)if((xo_sent^next)&(1U<<state)) {
        cmd.data=(next&(1U<<state))?3:0;
        int error=star_gpu_rpmh_write_state((enum star_gpu_rpmh_state)state,&cmd,1,next&&state==2);
        if(error)return error;
    }
    xo_state[own]=state;xo_sent=next;return 0;
}
static int enable_clock(int id)
{
    struct clock *c=&clocks[id];
    if(!c->prepares)return EIO;
    if(c->enables){c->enables++;return 0;}
    int error=0;if(c->parent>=0){error=enable_clock(c->parent);if(error)return error;}
    if(c->kind==XO)error=xo_vote(c,1);
    else if(c->kind==PLL)error=pll_enable(c);
    else if(c->kind==BRANCH)error=branch(c,1);
    else if(c->kind==RCG)error=star_gpu_rcg_enable(&rcgs[rcg_index(id)]);
    if(error){if(c->parent>=0)disable_clock(c->parent);return error;}
    c->enables=1;return 0;
}
static void disable_clock(int id)
{
    struct clock *c=&clocks[id];
    if(!c->enables){star_gpu_log("STAR GPU clock: native unbalanced disable\n");return;}
    if(c->enables==1&&c->critical)return;
    if(--c->enables)return;
    if(c->kind==XO)(void)xo_vote(c,0);
    else if(c->kind==PLL)pll_disable(c);
    else if(c->kind==BRANCH&&!c->aon)(void)branch(c,0);
    else if(c->kind==RCG)star_gpu_rcg_disable(&rcgs[rcg_index(id)]);
    if(c->parent>=0)disable_clock(c->parent);
}
static int boot_enabled(int id)
{
    struct clock *c=&clocks[id];
    if(c->kind==BRANCH)return !!(read_clock(c,0)&c->mask);
    if(c->kind==PLL)return pll_is_enabled(c);
    if(c->kind==RCG)return !(read_clock(c,0)&(1U<<31));
    return 0; /* RPMh clocks start with software state=0 in native probe. */
}
static int selected_parent(int id,unsigned source)
{
    if(id==STAR_CLK_UFS_AXI_SRC) {
        switch(source){case 0:return STAR_CLK_XO;case 1:return STAR_CLK_GPLL0;
        case 6:return STAR_CLK_GPLL0_EVEN;case 7:return -1;default:return STAR_CLK_XO;}
    }
    switch(source) {
    case 0:return STAR_CLK_XO;
    case 1:return id==STAR_CLK_GMU_SRC?STAR_CLK_PLL0:STAR_CLK_XO;
    case 3:return STAR_CLK_PLL1;
    case 5:return STAR_CLK_GCC_MAIN;
    case 6:return STAR_CLK_GCC_DIV;
    case 7:return -1; /* No board PLL-test provider is registered. */
    default:return STAR_CLK_XO; /* Native invalid-parent fallback index 0. */
    }
}
static int orphan(int id)
{
    struct clock *c=&clocks[id];
    if(c->kind==RCG&&c->parent<0)return 1;
    return c->parent>=0?(!clocks[c->parent].registered||orphan(c->parent)):0;
}
static void hold_state(int id)
{
    struct clock *c=&clocks[id];
    if(c->hold||c->dont_hold||orphan(id)||!c->boot_on)return;
    /* Native clk_core_hold_state is void; a failed hold does not turn the
     * entire provider probe into a new fatal gate. */
    if(prepare(id))return;
    if(enable_clock(id)){unprepare(id);return;}
    c->hold=1;
}
static void update_orphans(int hold)
{
    for(int i=0;i<STAR_CLK_COUNT;i++)if(clocks[i].registered) {
        int now=orphan(i),before=clocks[i].was_orphan;
        clocks[i].was_orphan=(unsigned)now;
        if(before&&!now&&hold)hold_state(i);
    }
}
static int register_clock(int id)
{
    struct clock *c=&clocks[id];
    c->registered=1;c->boot_on=boot_enabled(id);c->was_orphan=(unsigned)orphan(id);
    if(c->critical) {
        int error=prepare(id);
        if(error){c->registered=0;return error;}
        error=enable_clock(id);
        if(error) {
            /* A failed native critical registration deletes its core. This
             * clock has no parent/VDD; discard only that failed core count,
             * without inventing a compensating hardware disable. */
            c->prepares=0;c->registered=0;return error;
        }
    }
    hold_state(id);update_orphans(1);return 0;
}
static int set_rate(int id,uint32_t hz)
{
    struct clock *c=&clocks[id];
    if(c->kind==BCM) {
        ipa_state=hz/ipa_unit;
        if(c->prepares)(void)ipa_vote(1); /* native callback returns 0 */
        return 0;
    }
    if(c->kind==BRANCH&&c->parent>=0)return set_rate(c->parent,hz);
    if(c->kind==RO_DIV) {
        uint64_t target=(uint64_t)hz*((read_clock(c,0)&15)+1);
        if(target>0xffffffffU)return EINVAL;
        return set_rate(c->parent,(uint32_t)target);
    }
    if(c->kind!=RCG)return hz==rate(id)?0:EINVAL;
    static const uint32_t gmu_rates[]={19200000,200000000,500000000};
    static const uint32_t hub_rates[]={150000000,240000000,300000000};
    static const int gmu_parents[]={STAR_CLK_XO,STAR_CLK_GCC_DIV,STAR_CLK_PLL1};
    static const int hub_parents[]={STAR_CLK_GCC_DIV,STAR_CLK_GCC_MAIN,STAR_CLK_GCC_MAIN};
    static const uint32_t ufs_rates[]={25000000,75000000,150000000,300000000};
    static const int ufs_parents[]={STAR_CLK_GPLL0_EVEN,STAR_CLK_GPLL0_EVEN,STAR_CLK_GPLL0_EVEN,STAR_CLK_GPLL0_EVEN};
    unsigned count=id==STAR_CLK_UFS_AXI_SRC?4:3;
    unsigned index=0;const uint32_t *rates=id==STAR_CLK_GMU_SRC?gmu_rates:hub_rates;
    const int *parents=id==STAR_CLK_GMU_SRC?gmu_parents:hub_parents;
    if(id==STAR_CLK_UFS_AXI_SRC){rates=ufs_rates;parents=ufs_parents;}
    while(index<count&&hz>rates[index])index++;
    if(index==count)return EINVAL;
    hz=rates[index];
    if(hz==rate(id))return 0; /* Native clk_core_set_rate_nolock early exit. */
    int new_parent=parents[index],old_parent=c->parent,error=0;
    unsigned old_vdd=c->selected_vdd,new_vdd=voltage_level(id,hz);
    if(c->prepares&&new_vdd>old_vdd) {error=vote(c,new_vdd);if(error)return error;}
    int migrating=c->prepares&&new_parent!=old_parent;
    if(migrating) {
        error=prepare(new_parent);if(error)goto voltage_rollback;
        error=enable_clock(new_parent);
        if(error){unprepare(new_parent);goto voltage_rollback;}
        /* Native __clk_set_parent_before forces the core and new parent
         * on even when the core is prepared but not currently enabled. */
        error=enable_clock(id);
        if(error) {
            disable_clock(new_parent);unprepare(new_parent);
            goto voltage_rollback;
        }
    }
    c->parent=new_parent;
    error=star_gpu_rcg_set_rate(&rcgs[rcg_index(id)],hz,!!c->prepares);
    /* clk_change_rate always finishes parent migration, even if the RCG
     * callback reports an error. Do not invent an old-parent switch, reset
     * or second configuration attempt. Preserve the real callback error
     * for this XNU API instead of manufacturing completion. */
    if(!error)c->cached=hz;
    if(migrating) {
        disable_clock(id);
        if(old_parent>=0){disable_clock(old_parent);unprepare(old_parent);}
    }
    update_orphans(0);
    unsigned actual_vdd=voltage_level(id,rate(id));
    if(c->prepares&&actual_vdd&&actual_vdd<c->selected_vdd) {
        int post_error=vote(c,actual_vdd);
        if(!error)error=post_error;
    }
    if(!c->prepares)c->selected_vdd=new_vdd;
    return error;
voltage_rollback:
    if(c->prepares&&new_vdd>old_vdd)(void)vote(c,old_vdd);
    return error;
}
int star_gpu_clock_provider_open(void)
{
    lck_mtx_lock(&star_gpu_cc_lock);
    if(opened){lck_mtx_unlock(&star_gpu_cc_lock);return 0;}
    int error=star_gpu_regulators_open();if(error)goto out;
    gcc=(volatile uint8_t *)ml_io_map(0x00100000,0x90000);
    gpucc=(volatile uint8_t *)ml_io_map(0x03d90000,0x9000);
    if(!gcc||!gpucc){error=ENOMEM;goto out;}
    struct star_gpu_db_entry xo;
    error=star_gpu_cmd_db_find("xo.lvl",&xo);if(error)goto out;
    if(!xo.address){error=ENODEV;goto out;}
    xo_address=xo.address; /* ARC clock enable offset is 0. */
    struct star_gpu_db_entry ipa;
    error=star_gpu_cmd_db_find("IP0",&ipa);if(error)goto out;
    if(!ipa.address||ipa.bytes<4){error=ENODEV;goto out;}
    ipa_address=ipa.address;
    ipa_unit=(uint32_t)ipa.aux[0]|((uint32_t)ipa.aux[1]<<8)|((uint32_t)ipa.aux[2]<<16)|((uint32_t)ipa.aux[3]<<24);
    ipa_unit*=1000; /* Native clk-rpmh aux unit is kHz, rate API is Hz. */
    if(!ipa_unit){error=EINVAL;goto out;}
    if(!gcc_registered) {
        if(!clocks[STAR_CLK_GPLL0].vreg) {
            error=star_gpu_regulator_get(STAR_GPU_CX,0,&vdd[0].regulator);
            if(error)goto out;
            clocks[STAR_CLK_GPLL0].vreg=vdd[0].regulator;
        }
        clocks[STAR_CLK_UFS_AXI_SRC].vreg=vdd[0].regulator;
        unsigned cfg=read_clock(&clocks[STAR_CLK_UFS_AXI_SRC],4),hid=cfg&31;
        int parent=selected_parent(STAR_CLK_UFS_AXI_SRC,(cfg>>8)&7);
        clocks[STAR_CLK_UFS_AXI_SRC].parent=parent;
        uint32_t hz=parent<0?0:hid?(uint32_t)((uint64_t)rate(parent)*2/(hid+1)):rate(parent);
        clocks[STAR_CLK_UFS_AXI_SRC].cached=hz;
        rcgs[2]=(struct star_gpu_rcg){gcc,STAR_GPU_RCG_UFS_AXI,hz};
        const int order[]={STAR_CLK_XO,STAR_CLK_XO_AO,STAR_CLK_GPLL0,
            STAR_CLK_GPLL0_EVEN,STAR_CLK_DDR_AXI,STAR_CLK_CFG_AHB,STAR_CLK_GCC_MAIN,
            STAR_CLK_GCC_DIV,STAR_CLK_MEMNOC,STAR_CLK_SNOC,
            STAR_CLK_UFS_AXI_SRC,STAR_CLK_AGGRE_UFS_AXI,STAR_CLK_IPA};
        for(unsigned i=0;i<sizeof(order)/sizeof(order[0]);i++) {
            if(clocks[order[i]].registered)continue;
            error=register_clock(order[i]);if(error)goto out;
        }
        gcc_registered=1; /* A later GPUCC failure does not unregister GCC. */
    }
    for(unsigned i=STAR_CLK_PLL0;i<STAR_CLK_COUNT;i++)if(clocks[i].rail>=0) {
        unsigned which=vdd_index(&clocks[i]);
        if(!vdd[which].regulator) {
            error=star_gpu_regulator_get((enum star_gpu_rail)clocks[i].rail,0,&vdd[which].regulator);
            if(error)goto unwind;
        }
        clocks[i].vreg=vdd[which].regulator;
    }
    error=star_gpu_regulator_get(STAR_GPU_CX,0,&cx_parent);if(error)goto unwind;
    if(!cx_registered) {
        /* Native gdsc_probe: SW-trigger/HW-state-machine sequencing.
         * The selected DT has no clk-dis-wait override or reset toggle. */
        uint32_t control=star_gpu_read32(gpucc,0x106c);
        star_gpu_write32(gpucc,0x106c,control&~6U);
        cx_enabled=!(star_gpu_read32(gpucc,0x106c)&1);
        cx_hw_mode=!!(star_gpu_read32(gpucc,0x106c)&2);
        cx_registered=1;
    }
    pll_configure(STAR_CLK_PLL0,0x18,0x6000);
    pll_configure(STAR_CLK_PLL1,0x1a,0xaaa);
    star_gpu_update32(gpucc,0x1170,1,1); /* Native GPUCC probe, cb_clk. */
    for(unsigned i=0;i<2;i++) {
        int id=i?STAR_CLK_HUB_SRC:STAR_CLK_GMU_SRC;
        unsigned cfg=read_clock(&clocks[id],4);
        int parent=selected_parent(id,(cfg>>8)&7);
        clocks[id].parent=parent;
        unsigned hid=cfg&31;
        clocks[id].cached=!((cfg>>8)&7)?19200000:parent<0?0:
            hid?(uint32_t)((uint64_t)rate(parent)*2/(hid+1)):rate(parent);
        rcgs[i]=(struct star_gpu_rcg){gpucc,i?STAR_GPU_RCG_HUB:STAR_GPU_RCG_GMU,clocks[id].cached};
    }
    const int gpu_order[]={STAR_CLK_PLL0,STAR_CLK_PLL1,STAR_CLK_AHB,
        STAR_CLK_CRC_AHB,STAR_CLK_GMU,STAR_CLK_CX_SNOC,STAR_CLK_CXO_AON,
        STAR_CLK_CXO,STAR_CLK_GMU_SRC,STAR_CLK_GX_GMU,STAR_CLK_SMMU,
        STAR_CLK_AHB_DIV,STAR_CLK_HUB,STAR_CLK_HUB_SRC,STAR_CLK_HUB_INT,
        STAR_CLK_INT_DIV,STAR_CLK_MND0,STAR_CLK_MND1,STAR_CLK_SLEEP};
    for(unsigned i=0;i<sizeof(gpu_order)/sizeof(gpu_order[0]);i++) {
        error=register_clock(gpu_order[i]);if(error)goto unwind;
    }
    opened=1;goto out;
    /* Native critical CFG_AHB registration has no disable-on-unused path.
     * Its permanent reference is retained; no global GCC shutdown here. */
unwind:
    for(unsigned group=1;group<3;group++)if(vdd[group].regulator) {
        (void)star_gpu_regulator_put(vdd[group].regulator);vdd[group].regulator=NULL;
    }
    for(unsigned i=STAR_CLK_PLL0;i<STAR_CLK_UFS_AXI_SRC;i++)clocks[i].vreg=NULL;
    if(cx_parent){(void)star_gpu_regulator_put(cx_parent);cx_parent=NULL;}
out:
    lck_mtx_unlock(&star_gpu_cc_lock);return error;
}
int star_gpu_clock_prepare_enable(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return EINVAL;
    lck_mtx_lock(&star_gpu_cc_lock);
    int error=opened?prepare(id):ENXIO;
    if(!error){error=enable_clock(id);if(error)unprepare(id);}
    lck_mtx_unlock(&star_gpu_cc_lock);return error;
}
void star_gpu_clock_disable_unprepare(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return;
    lck_mtx_lock(&star_gpu_cc_lock);
    if(opened){disable_clock(id);unprepare(id);}
    lck_mtx_unlock(&star_gpu_cc_lock);
}
int star_gpu_clock_set_rate(enum star_gpu_clock_id id,uint32_t hz)
{
    if((unsigned)id>=STAR_CLK_COUNT)return EINVAL;
    lck_mtx_lock(&star_gpu_cc_lock);
    int error=opened?set_rate(id,hz):ENXIO;
    lck_mtx_unlock(&star_gpu_cc_lock);return error;
}
uint32_t star_gpu_clock_get_rate(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return 0;
    lck_mtx_lock(&star_gpu_cc_lock);uint32_t hz=opened?rate(id):0;
    lck_mtx_unlock(&star_gpu_cc_lock);return hz;
}
int star_gpu_clock_resolve(enum star_gpu_clock_controller controller,unsigned binding,
    enum star_gpu_clock_id *result)
{
    static const int gpucc_ids[20]={
        STAR_CLK_PLL0,STAR_CLK_PLL1,STAR_CLK_AHB,-1,STAR_CLK_CRC_AHB,
        STAR_CLK_GMU,STAR_CLK_CX_SNOC,STAR_CLK_CXO_AON,STAR_CLK_CXO,
        STAR_CLK_GMU_SRC,STAR_CLK_GX_GMU,STAR_CLK_SMMU,STAR_CLK_AHB_DIV,
        STAR_CLK_HUB,STAR_CLK_HUB_SRC,STAR_CLK_HUB_INT,STAR_CLK_INT_DIV,
        STAR_CLK_MND0,STAR_CLK_MND1,STAR_CLK_SLEEP
    };
    static const struct {unsigned binding;enum star_gpu_clock_id id;} gcc_ids[]={
        {0,STAR_CLK_GPLL0},{1,STAR_CLK_GPLL0_EVEN},{18,STAR_CLK_DDR_AXI},
        {30,STAR_CLK_CFG_AHB},{31,STAR_CLK_GCC_MAIN},{32,STAR_CLK_GCC_DIV},
        {34,STAR_CLK_MEMNOC},{35,STAR_CLK_SNOC},{8,STAR_CLK_AGGRE_UFS_AXI},{124,STAR_CLK_UFS_AXI_SRC}
    };
    if(controller==STAR_CC_GPUCC&&binding<20&&gpucc_ids[binding]>=0) {
        *result=(enum star_gpu_clock_id)gpucc_ids[binding];return 0;
    }
    if(controller==STAR_CC_GCC)for(unsigned i=0;i<sizeof(gcc_ids)/sizeof(gcc_ids[0]);i++)
        if(gcc_ids[i].binding==binding){*result=gcc_ids[i].id;return 0;}
    if(controller==STAR_CC_RPMH&&binding==22){*result=STAR_CLK_IPA;return 0;}
    if(controller==STAR_CC_RPMH&&binding<2) {
        *result=binding?STAR_CLK_XO_AO:STAR_CLK_XO;return 0;
    }
    return ENOENT;
}
void star_gpu_clock_sync_state(int gcc_bound,int gpucc_bound)
{
    lck_mtx_lock(&star_gpu_cc_lock);
    if(opened)for(int i=STAR_CLK_COUNT-1;i>=0;i--) {
        struct clock *c=&clocks[i];
        if(c->hold&&(c->gpu?gpucc_bound:gcc_bound)) {
            c->hold=0;disable_clock(i);unprepare(i);
        }
    }
    lck_mtx_unlock(&star_gpu_cc_lock);
}
int star_gpu_cx_get(void)
{
    lck_mtx_lock(&star_gpu_cc_lock);
    int error=opened?0:ENXIO;if(error)goto out;
    if(cx_users){cx_users++;goto out;}
    error=star_gpu_regulator_enable(cx_parent);if(error)goto out;
    if(!cx_enabled) {
        error=star_gpu_cx_gdsc_transition_on((volatile uint32_t *)(gpucc+0x106c),
                                          (volatile uint32_t *)(gpucc+0x1540));
        if(error){(void)star_gpu_regulator_disable(cx_parent);goto out;}
        cx_enabled=1;
    }
    cx_users=1;
out:lck_mtx_unlock(&star_gpu_cc_lock);return error;
}
int star_gpu_cx_put(void)
{
    lck_mtx_lock(&star_gpu_cc_lock);
    int error=0;
    if(!opened||!cx_users){error=EIO;goto out;}
    if(cx_users>1){cx_users--;goto out;}
    if(!star_gpu_regulator_is_enabled(cx_parent)){error=EIO;goto out;}
    star_gpu_native_udelay(1);
    star_gpu_write32(gpucc,0x106c,star_gpu_read32(gpucc,0x106c)|1);
    (void)star_gpu_read32(gpucc,0x106c);
    star_gpu_native_udelay(1);
    star_gpu_native_udelay(100); /* Native DT no-status-check-on-disable. */
    cx_enabled=0;cx_users=0;error=star_gpu_regulator_disable(cx_parent);
out:lck_mtx_unlock(&star_gpu_cc_lock);return error;
}
int star_gpu_cx_is_enabled(void)
{
    lck_mtx_lock(&star_gpu_cc_lock);
    int enabled=opened?cx_enabled:-ENXIO;
    lck_mtx_unlock(&star_gpu_cc_lock);return enabled;
}
int star_gpu_cx_get_hw_mode(void)
{
    lck_mtx_lock(&star_gpu_cc_lock);
    int mode=opened?cx_hw_mode:-ENXIO;
    lck_mtx_unlock(&star_gpu_cc_lock);return mode;
}
int star_gpu_cx_set_hw_mode(int hardware_mode)
{
    if(hardware_mode!=0&&hardware_mode!=1)return EINVAL;
    lck_mtx_lock(&star_gpu_cc_lock);
    /* Native regulator_set_mode returns early for the current mode.
     * The live CX node lacks qcom,support-hw-trigger: changing mode is
     * rejected by regulator_mode_constrain, before gdsc_set_mode MMIO. */
    int error=!opened?ENXIO:hardware_mode==cx_hw_mode?0:EPERM;
    lck_mtx_unlock(&star_gpu_cc_lock);return error;
}

int star_gpu_clock_prepare(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return EINVAL;
    lck_mtx_lock(&star_gpu_cc_lock);int e=opened?prepare(id):ENXIO;
    lck_mtx_unlock(&star_gpu_cc_lock);return e;
}
void star_gpu_clock_unprepare(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return;
    lck_mtx_lock(&star_gpu_cc_lock);if(opened)unprepare(id);
    lck_mtx_unlock(&star_gpu_cc_lock);
}
int star_gpu_clock_enable(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return EINVAL;
    lck_mtx_lock(&star_gpu_cc_lock);int e=opened?enable_clock(id):ENXIO;
    lck_mtx_unlock(&star_gpu_cc_lock);return e;
}
void star_gpu_clock_disable(enum star_gpu_clock_id id)
{
    if((unsigned)id>=STAR_CLK_COUNT)return;
    lck_mtx_lock(&star_gpu_cc_lock);if(opened)disable_clock(id);
    lck_mtx_unlock(&star_gpu_cc_lock);
}
