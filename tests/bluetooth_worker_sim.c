/* Host model for the kernel Bluetooth worker: simulated clock (ns), UART
 * (TX done 1 ms after a write), scripted controller replies and a counting
 * semaphore. Exercises the production bt_step/bt_wait/bt_run/bt_send/bt_cmd/
 * bt_download extracted from renoir_bluetooth_hci.inc. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#define NSEC_PER_MSEC 1000000ull
#define KERN_SUCCESS 0
#define KERN_OPERATION_TIMED_OUT 49
#define ETIMEDOUT 60
#define EIO 5
typedef int semaphore_t;typedef unsigned long vm_offset_t;
static uint64_t now_ns;
static uint64_t mach_absolute_time(void){return now_ns;}
static void clock_interval_to_deadline(uint32_t n,uint64_t unit,uint64_t *d){*d=now_ns+n*unit;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t;}
static int bu_lock;static void lck_mtx_lock(int *l){(void)l;}static void lck_mtx_unlock(int *l){(void)l;}
static struct {vm_offset_t nc;} bts;static uint8_t dma[0x2000];
static void bt_wr(vm_offset_t b,unsigned o,uint32_t v){(void)b;(void)o;(void)v;}
static struct {uint64_t rx_dropped;} bt_sk;static uint64_t bt_sk_kicks_tx,bt_sk_kicks_rx;
static uint32_t renoir_bt_sk_tx_pending(void){return 0;}
static uint32_t renoir_bt_sk_tx_take(uint8_t *d,uint32_t m){(void)d;(void)m;return 0;}
static uint32_t renoir_bt_sk_rx_put(const uint8_t *d,uint32_t n){(void)d;return n;}
#include "worker_bu.inc"
static void bu_rx_dma_start_locked(void){}
/* simulated hardware */
static int sem_count;static semaphore_t bt_work=1;static unsigned tx_inflight;static uint64_t tx_done_at;
struct ev{uint64_t at;uint8_t data[300];unsigned n;bool used;};static struct ev rxev[64];
static unsigned writes;static int reply_mode;      /* 0 none, 1 ack every command, 2 ack + trailing other event */
static void schedule_rx(uint64_t at,const uint8_t *d,unsigned n){for(int i=0;i<64;++i)if(!rxev[i].used){rxev[i].used=true;rxev[i].at=at;memcpy(rxev[i].data,d,n);rxev[i].n=n;return;}abort();}
static bool bu_write(const uint8_t *d,unsigned n){
    if(n>sizeof(bu.xmit)-bu.xmit_len)return false;
    memcpy(bu.xmit+bu.xmit_len,d,n);bu.xmit_len+=n;++writes;
    if(!tx_inflight){tx_inflight=bu.xmit_len;tx_done_at=now_ns+NSEC_PER_MSEC;}
    if(d[0]==1 && reply_mode) {
        uint8_t r[16]={4,0x0e,5,1,d[1],d[2],0,d[1]==0&&d[2]==0xfc?d[4]:0},o[]={4,0x0e,4,1,0x48,0xfc,1};
        unsigned rn=d[1]==0&&d[2]==0xfc?8:7;r[2]=rn-3;
        if(reply_mode==2){uint8_t both[32];memcpy(both,r,rn);memcpy(both+rn,o,7);schedule_rx(now_ns+2*NSEC_PER_MSEC,both,rn+7);}
        else schedule_rx(now_ns+2*NSEC_PER_MSEC,r,rn);
    }
    return true;
}
static unsigned bu_room(void){return sizeof(bu.xmit)-bu.xmit_len;}
static bool bu_tx_empty(void){return !tx_inflight && !bu.xmit_len;}
static uint64_t next_event(void){uint64_t t=UINT64_MAX;if(tx_inflight&&tx_done_at<t)t=tx_done_at;for(int i=0;i<64;++i)if(rxev[i].used&&rxev[i].at<t)t=rxev[i].at;return t;}
static void fire(void){
    if(tx_inflight&&tx_done_at<=now_ns){memmove(bu.xmit,bu.xmit+tx_inflight,bu.xmit_len-tx_inflight);bu.xmit_len-=tx_inflight;tx_inflight=0;
        if(bu.xmit_len){tx_inflight=bu.xmit_len;tx_done_at=now_ns+NSEC_PER_MSEC;}++sem_count;}
    for(int i=0;i<64;++i)if(rxev[i].used&&rxev[i].at<=now_ns){for(unsigned k=0;k<rxev[i].n;++k)bu.rxq[(bu.rx_head+bu.rx_len+k)%BU_RXQ]=rxev[i].data[k];bu.rx_len+=rxev[i].n;rxev[i].used=false;++sem_count;}
}
static int deadline_wakes;
static int semaphore_wait_deadline(semaphore_t s,uint64_t deadline){(void)s;
    for(;;){if(sem_count){--sem_count;return KERN_SUCCESS;}
        uint64_t t=next_event();if(t>deadline){now_ns=deadline;++deadline_wakes;return KERN_OPERATION_TIMED_OUT;}
        now_ns=t;fire();}}
static int semaphore_wait(semaphore_t s){return semaphore_wait_deadline(s,UINT64_MAX-1);}
#include "worker.inc"
static void reset(int mode){memset(&bw,0,sizeof bw);memset(&bu,0,sizeof bu);memset(rxev,0,sizeof rxev);bts.nc=(vm_offset_t)dma;
    sem_count=0;tx_inflight=0;writes=0;deadline_wakes=0;reply_mode=mode;now_ns=1000*NSEC_PER_MSEC;}
int main(void){
    uint8_t q=0x19;
    reset(1);uint64_t t0=now_ns;int e=bt_cmd(0xfc00,&q,1,0x19,"VER",true);
    printf("reply at 2 ms: e=%d elapsed=%llu ms deadline_wakes=%d\n",e,(now_ns-t0)/NSEC_PER_MSEC,deadline_wakes);
    assert(!e && now_ns-t0==2*NSEC_PER_MSEC && !deadline_wakes);
    reset(0);t0=now_ns;e=bt_cmd(0xfc00,&q,1,0x19,"VER",false);
    printf("no reply: e=%d elapsed=%llu ms\n",e,(now_ns-t0)/NSEC_PER_MSEC);
    assert(e==ETIMEDOUT && now_ns-t0==10000*NSEC_PER_MSEC);
    reset(2);t0=now_ns;e=bt_cmd(0xfc00,&q,1,0x19,"VER",true);
    printf("reply + trailing fc48 status 1 event: e=%d reply opcode=%02x%02x status=%u unrouted=%llu\n",e,bw.reply[5],bw.reply[4],bw.reply[6],bw.unrouted);
    assert(!e && bw.reply[4]==0 && bw.reply[5]==0xfc && bw.reply[6]==0 && bw.unrouted==1);
    static uint8_t nvm[6277];for(unsigned i=0;i<sizeof nvm;++i)nvm[i]=(uint8_t)i;
    reset(1);t0=now_ns;e=bt_download(nvm,sizeof nvm,false,"NVM");
    printf("NVM 6277 bytes, every segment acked: e=%d elapsed=%llu ms deadline_wakes=%d\n",e,(now_ns-t0)/NSEC_PER_MSEC,deadline_wakes);
    assert(!e && now_ns-t0<100*NSEC_PER_MSEC && !deadline_wakes);
    static uint8_t patch[243*20+10];
    reset(1);t0=now_ns;e=bt_download(patch,sizeof patch,true,"RAMPATCH");
    printf("rampatch 21 segments, last acked: e=%d elapsed=%llu ms deadline_wakes=%d\n",e,(now_ns-t0)/NSEC_PER_MSEC,deadline_wakes);
    assert(!e && !deadline_wakes);
    /* UART drain wait used by the speed switch */
    reset(0);uint8_t c[]={1,0x48,0xfc,1,0x11};uint64_t d;clock_interval_to_deadline(10000,NSEC_PER_MSEC,&d);
    t0=now_ns;e=bt_send(c,5,d);clock_interval_to_deadline(100,NSEC_PER_MSEC,&d);bool sent=bt_run(bt_tx_drained,d);
    printf("speed command queued+drained: e=%d sent=%d elapsed=%llu ms\n",e,sent,(now_ns-t0)/NSEC_PER_MSEC);
    assert(!e && sent && now_ns-t0==NSEC_PER_MSEC);
    puts("worker simulation passed");
}
