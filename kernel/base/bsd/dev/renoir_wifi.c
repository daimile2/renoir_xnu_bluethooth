/* Renoir QCA6490 owned PCIe/MHI control service. Numbers from live FDT and
 * gcc-shima.c / pci-msm.c, LineageOS commit7ede20c8692e0e0fc044754429e33cfd1d75c40d.
 * HackiOS WCN6750 is an integrated WPSS transport and cannot supply this host.
 * radio-route-stage selects the owned root setup. An independent worker owns
 * the BAR/DMA/firmware lifetime; WLAN packet/network support is still pending. */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/renoir_adsp.h>
#include <vm/vm_kern.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_log_checkpoint(void);
extern vm_offset_t pe_arm_get_soc_base_phys(void);

/* One owner retains every BAR/SID/clock save frame until service exits.
 * Readers consume cached atomics only; they never touch powered-down MMIO. */
static struct {
    bool queued,stop;
    uint32_t state,failed,seconds,events,replies,fsr,mhi,cleanup,cleanup_error;
} wfr;
void renoir_wifi_probe_init(void);
unsigned renoir_wifi_request_stop(void);
void renoir_wifi_service_status(uint32_t values[10]);
void renoir_wifi_service_shutdown(void);
extern void thread_terminate_self(void);

static uint32_t rd(vm_offset_t base,unsigned offset)
{
    uint32_t value=*(volatile uint32_t *)(base+offset);
    __asm__ volatile("dmb oshld" ::: "memory");return value;
}

static bool
profile_resource(DTEntry entry,const char *key,uint32_t expected)
{
    const void *data;unsigned length=0;
    return SecureDTGetProperty(entry,key,&data,&length)==kSuccess && length==4 &&
        *(const uint32_t *)data==expected;
}

#include "renoir_bluetooth_probe.inc"
#include "renoir_pcie_start.inc"

static void
renoir_wifi_run(void)
{
    if(!renoir_io_stage_enabled() || os_atomic_load(&wfr.stop,acquire)) return;
    DTEntry profile;
    if(pe_arm_get_soc_base_phys()!=0x01000000 ||
        SecureDTLookupEntry(NULL,"/chosen/renoir-platform",&profile)!=kSuccess ||
        !profile_resource(profile,"wifi-pcie-parf",0x01c00000) ||
        !profile_resource(profile,"wifi-pcie-dbi",0x60000000) ||
        !profile_resource(profile,"wifi-pcie-gdsc",0x0016b004) ||
        !profile_resource(profile,"wifi-enable-gpio",64)) {
        printf("STAR WIFI PCIE: resource profile mismatch; skipped\n");return;
    }
    printf("STAR WIFI PCIE: QCA6490 root0 probe; GCC/TLMM read only\n");
    if(sweet_log_checkpoint()!=0) return;
    vm_offset_t gcc=ml_io_map(0x00100000,0x1f0000);
    vm_offset_t gpio=ml_io_map(0x0f140000,0x1000); /* TLMM GPIO64 */
    if(!gcc || !gpio) { printf("STAR WIFI PCIE: map failed\n");return; }
    renoir_bluetooth_preflight(gcc);
    uint32_t power=rd(gcc,0x6b004),reset=rd(gcc,0x6b000),phy_reset=rd(gcc,0x6c01c);
    uint32_t votes=rd(gcc,0x52008),config=rd(gpio,0),inout=rd(gpio,4);
    printf("STAR WIFI PCIE POWER: gdsc=%#x reset=%#x phyreset=%#x votes=%#x GPIO64 cfg=%#x io=%#x\n",
        power,reset,phy_reset,votes,config,inout);
    static const unsigned clocks[]={0x6b024,0x6b01c,0x6b014,0x6b028,0x6b030,0x6b010};
    bool running=true;
    for(unsigned i=0;i<sizeof(clocks)/sizeof(clocks[0]);++i) {
        uint32_t value=rd(gcc,clocks[i]);
        printf("STAR WIFI PCIE CLOCK: offset=%#x value=%#x\n",clocks[i],value);
        if(value & (1u<<31)) running=false;
    }
    /* GPIO config bit9=output-enable, bits2..5=function. PMIC rail voltage
     * cannot be inferred from GPIO high; a powered-down domain is never read. */
    bool wlan=(config&(1u<<9)) && !(config&0x3cu) && (inout&2);
    bool domain=(power&(1u<<31)) && !(power&1u);
    if(!domain || !running || (reset&1) || (phy_reset&1) || !wlan) {
        printf("STAR WIFI PCIE BLOCKED: domain=%u clocks=%u core_reset=%u phy_reset=%u wlan_en=%u; DBI/PHY/config/BAR unread; need managed rail/clock/PHY startup\n",
            domain,running,reset&1,phy_reset&1,wlan);
        renoir_pcie_start(gcc,gpio);return;
    }
    printf("STAR WIFI PCIE: prerequisites active; checkpoint before existing root DBI read\n");
    if(sweet_log_checkpoint()!=0) return;
    vm_offset_t dbi=ml_io_map(0x60000000,0x1000);
    if(!dbi) return;
    uint32_t id=rd(dbi,0),status=rd(dbi,4);
    printf("STAR WIFI PCIE ROOT: id=%#x command_status=%#x\n",id,status);
    if(id==0 || id==0xffffffff || !(status&(1u<<20))) return;
    unsigned cap=rd(dbi,0x34)&0xffu;uint64_t seen=0;
    for(unsigned n=0;n<48 && cap>=0x40 && cap<=0xfc && !(cap&3);++n) {
        uint64_t bit=1ULL<<(cap/4);if(seen&bit) break;seen|=bit;
        uint32_t header=rd(dbi,cap);
        if((header&0xff)==0x10) {
            uint32_t link=rd(dbi,cap+0x10);
            printf("STAR WIFI PCIE LINK: cap=%#x status=%#x dll_active=%u speed=%u width=%u; endpoint/iATU/MHI not configured\n",
                cap,link,(link>>29)&1,(link>>16)&15,(link>>20)&63);return;
        }
        cap=(header>>8)&0xff;
    }
    printf("STAR WIFI PCIE: no valid PCIe capability; endpoint reads skipped\n");
}

unsigned renoir_wifi_request_stop(void)
{
    /* Fixed protocol result, not platform errno: 0 accepted,1 already pending,
     * 2 already stopped,3 previously failed,4 not started. Never restart here. */
    uint32_t state=os_atomic_load(&wfr.state,acquire);
    if(!os_atomic_load(&wfr.queued,acquire) || state==0) return 4;
    if(state==4) return 2;
    if(state==5) return 3;
    if(!os_atomic_cmpxchg(&wfr.stop,false,true,acq_rel)) return 1;
    return 0;
}
void renoir_wifi_service_status(uint32_t values[10])
{
    values[0]=os_atomic_load(&wfr.state,acquire);
    values[1]=os_atomic_load(&wfr.stop,acquire);
    values[2]=os_atomic_load(&wfr.failed,acquire);
    values[3]=os_atomic_load(&wfr.seconds,acquire);
    values[4]=os_atomic_load(&wfr.events,acquire);
    values[5]=os_atomic_load(&wfr.replies,acquire);
    values[6]=os_atomic_load(&wfr.fsr,acquire);
    values[7]=os_atomic_load(&wfr.mhi,acquire);
    values[8]=os_atomic_load(&wfr.cleanup,acquire);
    values[9]=os_atomic_load(&wfr.cleanup_error,acquire);
}
void renoir_wifi_service_shutdown(void)
{
    if(!os_atomic_load(&wfr.queued,acquire)) return;
    renoir_wifi_request_stop();
    for(unsigned ms=0;ms<5000;++ms) {
        uint32_t state=os_atomic_load(&wfr.state,acquire);
        if(state==4 || state==5) return;
        uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
        (void)assert_wait_deadline(&wfr,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
    }
    printf("STAR WIFI SHUTDOWN: owner has not acknowledged within5s; permanent DMA memory not reused\n");
}
static void renoir_wifi_worker(void *arg __unused,wait_result_t wr __unused)
{
    renoir_wifi_run();
    uint32_t restore_errors=os_atomic_load(&wfr.cleanup_error,acquire);
    if(restore_errors) {os_atomic_and(&wfr.cleanup,~restore_errors,release);os_atomic_store(&wfr.failed,1,release);}
    if(wff.done && os_atomic_load(&wfr.stop,acquire) && os_atomic_load(&wfr.cleanup,acquire)!=0x7f)
        os_atomic_store(&wfr.failed,1,release);
    printf("STAR WIFI CLEANUP SUMMARY: policy=native-disable completed=%#x errors=%#x required=%#x; mappings never freed on uncertainty\n",
        os_atomic_load(&wfr.cleanup,acquire),os_atomic_load(&wfr.cleanup_error,acquire),wff.done?0x7f:0);
    if(os_atomic_load(&wfr.cleanup_error,acquire)) os_atomic_store(&wfr.failed,1,release);
    uint32_t state=os_atomic_load(&wfr.failed,acquire)?5:(os_atomic_load(&wfr.stop,acquire)?4:5);
    os_atomic_store(&wfr.state,state,release);
    printf("STAR WIFI OWNER EXIT: state=%u stop=%u failed=%u; cleanup finished or resources quarantined\n",
        state,os_atomic_load(&wfr.stop,acquire),os_atomic_load(&wfr.failed,acquire));
    (void)sweet_log_checkpoint();
    thread_terminate_self();
}
void renoir_wifi_probe_init(void)
{
    if(!renoir_io_stage_enabled() || !os_atomic_cmpxchg(&wfr.queued,false,true,acq_rel)) return;
    os_atomic_store(&wfr.state,1,release);thread_t worker;
    if(kernel_thread_start(renoir_wifi_worker,NULL,&worker)!=KERN_SUCCESS) {
        os_atomic_store(&wfr.failed,1,release);os_atomic_store(&wfr.state,5,release);
        printf("STAR WIFI OWNER: thread creation failed; no hardware ownership acquired\n");return;
    }
    thread_deallocate(worker);
    printf("STAR WIFI OWNER: independent kernel worker queued; system startup continues\n");
}
#endif
