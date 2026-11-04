/* SPDX-License-Identifier: GPL-2.0-only
 * Selected shima.c GFX3D->LLCC->EBI path, icc-rpmh.c + bcm-voter.c.
 * LF2020. No GMU HFI BCM shortcut is used for the host ICC interface.
 */
#include <sys/star_gpu_log.h>
#include <sys/star_gpu_icc.h>
#include <sys/star_gpu_clock_provider.h>
#include <sys/star_gpu_rpmh.h>
#include <sys/star_gpu_native_io.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
struct star_gpu_icc {struct star_gpu_icc *next;uint32_t average,peak;int last_error,pas;};
struct icc_bcm {const char *name;unsigned width,channels,keepalive,address,unit,aux_width,vcd;};
static struct icc_bcm bcms[]={
    {"SH0",16,2,1,0,0,0,0},{"MC0",4,2,1,0,0,0,0},{"ACV",4,2,0,0,0,0,0},
    {"CE0",8,1,0,0,0,0,0},{"SN8",16,1,0,0,0,0,0},{"SN0",16,1,1,0,0,0,0}
};
static struct star_gpu_icc *consumers;
static volatile uint8_t *gemnoc,*aggre2;
static unsigned bcm_count=3;
static int pas_qos_done;
static int opened,qos_done,voter_init=1,last_error;
LCK_GRP_DECLARE(star_gpu_icc_group,"Star ICC");
LCK_MTX_DECLARE(star_gpu_icc_lock,&star_gpu_icc_group);
static unsigned u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static uint64_t divide(uint64_t numerator,uint64_t base)
{return numerator&&numerator<base?1:numerator/base;}
static uint32_t encode(uint64_t x,uint64_t y,int commit)
{
    int valid=x||y;if(x>0x3fff)x=0x3fff;if(y>0x3fff)y=0x3fff;
    return ((unsigned)commit<<30)|((unsigned)valid<<29)|((uint32_t)x<<14)|(uint32_t)y;
}
static int commit_votes(void)
{
    uint64_t avg=0,peak=0,pas_avg=0,pas_peak=0;
    for(struct star_gpu_icc *c=consumers;c;c=c->next){avg+=c->average;if(c->peak>peak)peak=c->peak;
        if(c->pas){pas_avg+=c->average;if(c->peak>pas_peak)pas_peak=c->peak;}}
    unsigned order[6]={0,1,2,3,4,5};
    /* Stable sort by VCD exactly as the native voter, not a fixed SH0 order. */
    for(unsigned i=1;i<bcm_count;i++)for(unsigned j=i;j&&bcms[order[j-1]].vcd>bcms[order[j]].vcd;j--) {
        unsigned tmp=order[j];order[j]=order[j-1];order[j-1]=tmp;
    }
    struct star_gpu_rpmh_cmd active[6],wake[6],sleep[6];unsigned differs=0;
    for(unsigned i=0;i<bcm_count;i++) {
        struct icc_bcm *b=&bcms[order[i]];
        uint64_t node_avg=order[i]<3?avg:pas_avg,node_peak=order[i]<3?peak:pas_peak;
        uint64_t x=divide(divide(node_avg*b->aux_width,b->width*b->channels)*1000,b->unit);
        uint64_t y=divide(divide(node_peak*b->aux_width,b->width)*1000,b->unit),ax=x,ay=y;
        if(b->keepalive) {
            if(voter_init)ax=ay=16000;
            else if(!ax&&!ay)ax=ay=1;
        }
        int end=i+1==bcm_count||b->vcd!=bcms[order[i+1]].vcd;
        active[i]=(struct star_gpu_rpmh_cmd){b->address,encode(ax,ay,end),end};
        wake[i]=(struct star_gpu_rpmh_cmd){b->address,encode(ax,ay,end),0};
        sleep[i]=(struct star_gpu_rpmh_cmd){b->address,encode(x,y,end),0};
        if(ax!=x||ay!=y)differs|=1U<<i;
    }
    star_gpu_rpmh_invalidate_batch(&consumers);
    int e=star_gpu_rpmh_write_batch(&consumers,STAR_RPMH_ACTIVE,active,bcm_count);
    if(e&&e!=EBUSY)return e;
    /* Native ws_list contains only resources whose wake/sleep differ. */
    unsigned count=0;
    for(unsigned i=0;i<bcm_count;i++)if(differs&(1U<<i)){wake[count]=wake[i];sleep[count]=sleep[i];count++;}
    for(unsigned i=0;i<count;i++) {
        unsigned next=i+1;
        unsigned vcd=0,next_vcd=0;
        for(unsigned j=0;j<bcm_count;j++) {
            if(bcms[j].address==wake[i].address)vcd=bcms[j].vcd;
            if(next<count&&bcms[j].address==wake[next].address)next_vcd=bcms[j].vcd;
        }
        int end=next==count||vcd!=next_vcd;
        wake[i].data=(wake[i].data&~(1U<<30))|((unsigned)end<<30);
        sleep[i].data=(sleep[i].data&~(1U<<30))|((unsigned)end<<30);
    }
    if(count) {
        e=star_gpu_rpmh_write_batch(&consumers,STAR_RPMH_WAKE,wake,count);if(e)return e;
        e=star_gpu_rpmh_write_batch(&consumers,STAR_RPMH_SLEEP,sleep,count);
    }
    return e==EBUSY?0:e;
}
int star_gpu_icc_get(struct star_gpu_icc **out)
{
    lck_mtx_lock(&star_gpu_icc_lock);int e=0;
    if(!opened) {
        e=star_gpu_rpmh_open();if(e)goto done;
        for(unsigned i=0;i<bcm_count;i++) {
            struct star_gpu_db_entry entry;e=star_gpu_cmd_db_find(bcms[i].name,&entry);if(e)goto done;
            if(entry.bytes<8||!entry.address){e=EINVAL;goto done;}
            bcms[i].address=entry.address;
            bcms[i].unit=u16(entry.aux)|((uint32_t)u16(entry.aux+2)<<16);
            bcms[i].aux_width=u16(entry.aux+4);bcms[i].vcd=entry.aux[6];
            if(!bcms[i].unit){e=EINVAL;goto done;}
        }
        gemnoc=(volatile uint8_t *)ml_io_map(0x09100000,0xb4000);if(!gemnoc){e=ENOMEM;goto done;}
        opened=1;
    }
    struct star_gpu_icc *c=kalloc_data(sizeof(*c),Z_WAITOK|Z_ZERO);
    if(!c){e=ENOMEM;goto done;}c->next=consumers;consumers=c;*out=c;
done:lck_mtx_unlock(&star_gpu_icc_lock);return e;
}
int star_gpu_icc_get_pas(struct star_gpu_icc **out)
{
    struct star_gpu_icc *c=NULL;
    int e=star_gpu_clock_provider_open();if(e)return e;
    e=star_gpu_icc_get(&c);if(e)return e;
    lck_mtx_lock(&star_gpu_icc_lock);
    if(bcm_count==3) {
        for(unsigned i=3;i<6;i++) {
            struct star_gpu_db_entry entry;e=star_gpu_cmd_db_find(bcms[i].name,&entry);if(e)break;
            if(entry.bytes<8||!entry.address){e=EINVAL;break;}
            bcms[i].address=entry.address;
            bcms[i].unit=u16(entry.aux)|((uint32_t)u16(entry.aux+2)<<16);
            bcms[i].aux_width=u16(entry.aux+4);bcms[i].vcd=entry.aux[6];
            if(!bcms[i].unit){e=EINVAL;break;}
        }
        if(!e&&!aggre2)aggre2=(volatile uint8_t *)ml_io_map(0x01700000,0x2c080);
        if(!e&&!aggre2)e=ENOMEM;
        if(!e)bcm_count=6;
    }
    c->pas=1;
    lck_mtx_unlock(&star_gpu_icc_lock);
    if(e){star_gpu_icc_put(c);return e;}
    *out=c;return 0;
}
static int pas_qos(void)
{
    /* DT aggre2 clock order; native clk_bulk_prepare_enable prepares all
     * before enabling any. GEM-NoC has no QoS clocks. */
    const enum star_gpu_clock_id ids[]={STAR_CLK_AGGRE_UFS_AXI,STAR_CLK_IPA};
    unsigned prepared=0,enabled=0;int e=0;
    for(;prepared<2;prepared++){e=star_gpu_clock_prepare(ids[prepared]);if(e)goto done;}
    for(;enabled<2;enabled++){e=star_gpu_clock_enable(ids[enabled]);if(e)goto done;}
    star_gpu_update32(aggre2,0x1d008,0x70,2U<<4);
    star_gpu_update32(aggre2,0x1d008,8,8);
    star_gpu_update32(gemnoc,0xa4008,0x70,0);
    star_gpu_update32(gemnoc,0xa4008,8,8);
    pas_qos_done=1;
done:
    while(enabled)star_gpu_clock_disable(ids[--enabled]);
    while(prepared)star_gpu_clock_unprepare(ids[--prepared]);
    return e;
}
int star_gpu_icc_set(struct star_gpu_icc *c,uint32_t average,uint32_t peak)
{
    lck_mtx_lock(&star_gpu_icc_lock);c->average=average;c->peak=peak;
    last_error=commit_votes();c->last_error=last_error;
    if(last_error)star_gpu_log("STAR GPU ICC: native BCM commit error %d\n",last_error);
    /* qcom_icc_set ignores voter return; only QoS clock errors propagate.
     * Actual GEM-NoC DT has zero clocks. QoS uses native regmap_update_bits. */
    int e=0;
    if(c->pas&&!pas_qos_done&&(average||peak))e=pas_qos();
    if(!c->pas&&!qos_done&&(average||peak)) {
        unsigned offsets[2]={0x22008,0x62008};
        for(unsigned i=0;i<2;i++) {
            star_gpu_update32(gemnoc,offsets[i],0x70,0);
            star_gpu_update32(gemnoc,offsets[i],8,0);
        }
        qos_done=1;
    }
    lck_mtx_unlock(&star_gpu_icc_lock);return e;
}
void star_gpu_icc_put(struct star_gpu_icc *c)
{
    /* Native icc_put removes the request then re-aggregates affected nodes. */
    lck_mtx_lock(&star_gpu_icc_lock);struct star_gpu_icc **p=&consumers;
    while(*p&&*p!=c)p=&(*p)->next;
    if(*p)*p=c->next;last_error=commit_votes();
    lck_mtx_unlock(&star_gpu_icc_lock);kfree_data(c,sizeof(*c));
}
int star_gpu_icc_last_error(void)
{lck_mtx_lock(&star_gpu_icc_lock);int e=last_error;lck_mtx_unlock(&star_gpu_icc_lock);return e;}
int star_gpu_icc_consumer_error(struct star_gpu_icc *c)
{lck_mtx_lock(&star_gpu_icc_lock);int e=c->last_error;lck_mtx_unlock(&star_gpu_icc_lock);return e;}
void star_gpu_icc_sync_state(void)
{
    lck_mtx_lock(&star_gpu_icc_lock);voter_init=0;
    if(opened)last_error=commit_votes();
    lck_mtx_unlock(&star_gpu_icc_lock);
}

#include "star_gpu_apps_smmu.h"
