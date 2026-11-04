static void wb_mhi_snapshot(vm_offset_t,vm_offset_t,unsigned,const char *);
/* Finite QRTR discovery over native IPCR, no QMI requests or calibration.
 * MiCode net/qrtr/{mhi.c,qrtr.c,ns.c}; packet framing/HELLO follows the
 * pinned HackiOS redwood QRTR approach, with MHI replacing its GLINK link. */
static struct {
    bool active,tx_busy,rx_posted,ack,tx_db_requested,rx_db_requested;
    unsigned rx_slot,tx_slot,received,sent,tx_done,hellos,servers;
    uint64_t rx_tre,tx_tre;
    uint32_t ack_node,ack_port,ack_local_node,ack_local_port;
} wq;
static uint32_t wq_u32(vm_offset_t p,unsigned off)
{
    volatile uint8_t *b=(volatile uint8_t *)(p+off);
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static unsigned wq_u16(vm_offset_t p,unsigned off)
{
    volatile uint8_t *b=(volatile uint8_t *)(p+off);return b[0]|((unsigned)b[1]<<8);
}
#include "renoir_wifi_qmi.inc"
static bool wq_receive(vm_offset_t p,unsigned len)
{
    if(len<16) return false;
    unsigned ver=*(volatile uint8_t *)p,header,type,bytes,confirm;
    uint32_t src,port,dst,dport;
    if(ver==1) {
        if(len<32 || wq_u32(p,0)!=1) return false;
        header=32;type=wq_u32(p,4);src=wq_u32(p,8);port=wq_u32(p,12);
        confirm=wq_u32(p,16);bytes=wq_u32(p,20);dst=wq_u32(p,24);dport=wq_u32(p,28);
    } else if(ver==3) { /* downstream QRTR_PROTO_VER_2 has wire value3 */
        volatile uint8_t *b=(volatile uint8_t *)p;
        header=16+b[3];type=b[1];confirm=b[2]&1;bytes=wq_u32(p,4);
        src=wq_u16(p,8);port=wq_u16(p,10);dst=wq_u16(p,12);dport=wq_u16(p,14);
        if(port==0xfffe) port=0xfffffffeu;if(dport==0xfffe) dport=0xfffffffeu;
    } else return false;
    if(header>len || bytes>len-header || ((bytes+3u)&~3u)!=len-header ||
       (dst!=1 && dst!=0xffffffffu) || !src || src==0xffffffffu) return false;
    ++wq.received;
    printf("STAR QRTR RX: version=%u type=%u src=%u/%#x dst=%u/%#x bytes=%u confirm=%u\n",
        ver,type,src,port,dst,dport,bytes,confirm);
    if(dport==0xfffffffeu) {
        if(bytes<20 || wq_u32(p,header)!=type) return false;
        if(type==2) ++wq.hellos;
        if(type==3) wq_qmi_depart(src);
        if(type==4 || type==5) {
            uint32_t service=wq_u32(p,header+4),instance=wq_u32(p,header+8);
            uint32_t node=wq_u32(p,header+12),server_port=wq_u32(p,header+16);
            if(type==4) ++wq.servers;
            wq_qmi_server(type==4,service,instance,node,server_port);
            printf("STAR QRTR SERVICE: present=%u service=%#x version=%u instance=%u node=%u port=%#x\n",
                type==4,service,instance&255,instance>>8,node,server_port);
        }
    }
    if(type==1 && dport==0x4000 && !wq_qmi_receive(p+header,bytes,src,port)) {
        wqm.failed=true;printf("STAR QMI: malformed response or TLV boundary; stop\n");return false;
    }
    if(confirm) {
        if(wq.ack) return false; /* bounded one-item queue; never overwrite an ACK */
        wq.ack=true;wq.ack_node=src;wq.ack_port=port;
        wq.ack_local_node=dst==0xffffffffu?1:dst;wq.ack_local_port=dport;
    }
    return true;
}
static void wq_post_rx(vm_offset_t bar,vm_offset_t ctrl)
{
    unsigned at=0xa000+16*wq.rx_slot;
    wb_put64(ctrl,at,WD_IOVA+0x720000);wdwr(ctrl,at+8,0x8000);
    wdwr(ctrl,at+12,(2u<<16)|(1u<<9)); /* transfer, interrupt at EOT */
    wq.rx_tre=WD_IOVA+0x700000+at;
    unsigned next=(wq.rx_slot+1)&31;uint32_t wp=(uint32_t)(WD_IOVA+0x70a000+16*next);
    wb_put64(ctrl,44*21+36,wp);wq.rx_posted=true;
    __asm__ volatile("dsb sy" ::: "memory");
    unsigned db=wdrd(bar,0x18)+8*21;wdwr(bar,db+4,0);wdwr(bar,db,wp);
}
static bool wq_transfer_event(vm_offset_t ctrl,unsigned at)
{
    if(!wq.active) return true;
    unsigned code=wdrd(ctrl,at+8)>>24,ch=wdrd(ctrl,at+12)>>24;
    if(ch!=20 && ch!=21) return true;
    uint64_t ptr=((uint64_t)wdrd(ctrl,at+4)<<32)|wdrd(ctrl,at);
    /* Native mhi_main.c: only OOB/DB_MODE events request a doorbell;
     * they do not complete a descriptor. No periodic speculative re-ringing. */
    if(code==5 || code==6) {
        if(ch==20) wq.tx_db_requested=true;else wq.rx_db_requested=true;
        return true;
    }
    if(code!=2 && code!=4) return false;
    if(ch==20) {
        if(!wq.tx_busy || ptr!=wq.tx_tre) return false;
        wq.tx_busy=false;++wq.tx_done;wq.tx_slot=(wq.tx_slot+1)&31;
        printf("STAR IPCR TX COMPLETE: ptr=%#llx code=%u total=%u\n",ptr,code,wq.tx_done);
    } else {
        unsigned len=wdrd(ctrl,at+8)&0xffff;
        if(!wq.rx_posted || ptr!=wq.rx_tre || len>0x8000 || !len) return false;
        wq.rx_posted=false;
        /* Exactly one RX buffer outstanding: parse before reusing it. */
        if(!wq_receive(wi.rx_alias,len)) return false;
        wq.rx_slot=(wq.rx_slot+1)&31;
    }
    return true;
}
static bool wq_send_payload(vm_offset_t bar,vm_offset_t ctrl,vm_offset_t tx,
    unsigned type,uint32_t node,uint32_t port,uint32_t local_node,uint32_t local_port,
    const uint8_t *payload,unsigned bytes)
{
    if(wq.tx_busy || bytes>0x8000-32) return false;
    unsigned padded=(bytes+3)&~3u,total=32+padded;
    uint32_t words[8]={1,type,local_node,local_port,0,bytes,node,port};
    for(unsigned i=0;i<8;++i) wdwr(tx,4*i,words[i]);
    for(unsigned off=0;off<padded;off+=4) {
        uint32_t value=0;
        for(unsigned n=0;n<4 && off+n<bytes;++n) value|=(uint32_t)payload[off+n]<<(8*n);
        wdwr(tx,32+off,value);
    }
    unsigned at=0x9000+16*wq.tx_slot;
    wb_put64(ctrl,at,WD_IOVA+0x728000);wdwr(ctrl,at+8,total);
    wdwr(ctrl,at+12,(2u<<16)|(1u<<9));
    wq.tx_tre=WD_IOVA+0x700000+at;wq.tx_busy=true;++wq.sent;
    unsigned next=(wq.tx_slot+1)&31;uint32_t wp=(uint32_t)(WD_IOVA+0x709000+16*next);
    wb_put64(ctrl,44*20+36,wp);
    __asm__ volatile("dsb sy" ::: "memory");
    unsigned db=wdrd(bar,0x18)+8*20;wdwr(bar,db+4,0);wdwr(bar,db,wp);
    printf("STAR QRTR TX: type=%u dst=%u/%#x bytes=%u ptr=%#llx\n",type,node,port,total,wq.tx_tre);
    return true;
}
static bool wq_send(vm_offset_t bar,vm_offset_t ctrl,vm_offset_t tx,
    unsigned type,uint32_t node,uint32_t port,uint32_t local_node,uint32_t local_port)
{
    uint32_t words[5]={type,0,0,0,0};
    if(type==7) {words[1]=local_node;words[2]=local_port;}
    return wq_send_payload(bar,ctrl,tx,type,node,port,local_node,local_port,(const uint8_t *)words,sizeof(words));
}
static void wb_ipcr_discover(vm_offset_t bar,uint64_t bar_size,vm_offset_t ctrl,unsigned erdb,unsigned bhi,
    uint32_t rp[3],uint32_t wp[3],unsigned *events)
{
    vm_offset_t rx=ml_io_map_unmappable(wdm.pa+0x720000,0x8000,VM_WIMG_WCOMB);
    vm_offset_t tx=ml_io_map_unmappable(wdm.pa+0x728000,0x8000,VM_WIMG_WCOMB);
    if(!rx || !tx) return;
    wi.rx_alias=rx;wq_qmi_reset();wq_wlfw_reset();wq_wlfw_init_reset();wq_wlfw_files_reset();wc_reset();wc_config_reset();wc_srng_reset();wc_rx_reset();wc_tx_reset();wc_htc_reset();wc_bootstrap_reset();wc_wmi_tx_reset();wc_init_reset();wc_mac_reset();wc_htt_reset();wc_rxdma_reset();wc_dp_reset();wc_mode_reset();wc_rddm_reset();
    wq.tx_db_requested=wq.rx_db_requested=false;
    wq.active=true;wq.tx_busy=false;wq.rx_posted=false;wq.ack=false;
    wq.rx_slot=wq.tx_slot=wq.received=wq.sent=wq.tx_done=wq.hellos=wq.servers=0;
    wq_post_rx(bar,ctrl);
    if(!wq_send(bar,ctrl,tx,2,0xffffffffu,0xfffffffeu,1,0xfffffffeu)) return;
    bool good=true,resident=false,stopping=false;unsigned ms=0;uint64_t begin=mach_absolute_time();
    for(;;) {
        uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time()-begin,&ns);ms=(unsigned)(ns/1000000ULL);
        if(os_atomic_load(&wfr.stop,acquire)) {
            if(!stopping) {wc_dp_stop();stopping=true;os_atomic_store(&wfr.state,3,release);}
            if(!wm.phase || wm.phase==4) break;
        }
        uint32_t state=wdrd(bar,0x48),fault=wdrd(wdm.cb,0x58)&0xc00001feu;
        unsigned ee=rd(bar,bhi+0x28);
        if(ee!=2 || state==0xffffffff || (state&4) || fault ||
           !wb_mhi_drain(bar,ctrl,erdb,rp,wp,events)) {good=false;break;}
        /* Native event-triggered OOB/DB_MODE handling, with queued TREs. */
        if(wq.tx_db_requested || wq.rx_db_requested) {
            unsigned db=wdrd(bar,0x18);
            if(wq.rx_db_requested && wq.rx_posted) {wdwr(bar,db+8*21+4,0);wdwr(bar,db+8*21,wdrd(ctrl,44*21+36));}
            if(wq.tx_db_requested && wq.tx_busy) {wdwr(bar,db+8*20+4,0);wdwr(bar,db+8*20,wdrd(ctrl,44*20+36));}
            wq.tx_db_requested=wq.rx_db_requested=false;
        }
        if(!wq.rx_posted) wq_post_rx(bar,ctrl);
        if(wq.ack && !wq.tx_busy) {
            if(!wq_send(bar,ctrl,tx,7,wq.ack_node,wq.ack_port,wq.ack_local_node,wq.ack_local_port)) {good=false;break;}
            wq.ack=false;
        }
        wq_qmi_tick(bar,ctrl,tx,ms);
        if(wqm.failed) {good=false;break;}
        wc_config_tick(bar,ctrl,tx,ms);
        if(wqm.failed) {good=false;break;}
        if(wff.done && !wce.ready && !wc_prepare()) {
            printf("STAR CE: resource validation failed; no SRNG or NORMAL mode submitted\n");good=false;break;
        }
        if(wcfg.done && bar_size<0x100000) {printf("STAR WLAN MODE: BAR too small; no NORMAL\n");good=false;break;}
        if(wcfg.done && !wm.phase && !stopping && !wrd.attempted && !wc_rddm_prepare(bar,bar_size)) {
            printf("STAR RDDM: optional capture unavailable phase=%u publish_uncertain=%u; NORMAL uses independent hardware gates\n",os_atomic_load(&wrd.phase,acquire),wrd.publish_uncertain);
        }
        /* Optional capture must not hide actual device/translation faults. */
        if(wrd.publish_uncertain || (wdrd(wdm.cb,0x58)&0xc00001feu) || (wdrd(bar,0x48)&4) ||
           ((wdrd(bar,0x48)>>8)&255)!=2 || rd(bar,bhi+0x28)!=2) {good=false;break;}
        if(!stopping && os_atomic_load(&wfr.stop,acquire)) continue;
        wc_mode_tick(bar,ctrl,tx,ms,stopping);
        if(wqm.failed) {good=false;break;}
        if(stopping && wm.phase==4) break;
        /* Mode success is the native boundary for host CE configuration. */
        if(wm.phase==2 && !stopping && (!wc_srng_prepare(bar,bar_size) || !wc_srng_arm(bar))) {good=false;break;}
        if(wcs.enabled && !wc_rx_drain(bar)) {good=false;break;}
        /* Reuse the existing HTC READY event; no new wait or handshake. */
        if(wm.phase==2 && wcr.ready && !wdp.prepared && !stopping && !wc_dp_prepare(bar)) {good=false;break;}
        if(wcs.enabled && !wc_tx_poll()) {good=false;break;}
        if(wm.phase==2 && !wc_htc_tick(bar,ms,stopping)) {good=false;break;}
        if(wm.phase==2 && !wc_htc_start_tick(bar,ms,stopping)) {good=false;break;}
        if(wcs.enabled && !wc_wmi_tx_poll()) {good=false;break;}
        if(wm.phase==2 && !wc_init_tick(bar,ms,stopping)) {good=false;break;}
        if(wcs.enabled && !wc_htt_tx_poll()) {good=false;break;}
        if(wm.phase==2 && !wc_htt_tick(bar,ms,stopping)) {good=false;break;}
        if(wm.phase==2 && !wc_rxdma_tick(bar,ms,stopping)) {good=false;break;}
        if(!wc_dp_drain(bar)) {good=false;break;}
        if(wff.done && wcfg.done && wce.ready && wcs.enabled && wm.phase==2 && wcr.ready && wh.connected && wh.htt_connected && wbs.setup_sent && wic.ready && wmac.usable && wrx.submitted && wdp.refill_sent && wdp.error_ring_sent && !stopping && !wq.tx_busy && !wq.ack && !resident) {
            resident=true;
            os_atomic_store(&wfr.seconds,ms/1000,release);os_atomic_store(&wfr.events,*events,release);
            os_atomic_store(&wfr.replies,wqm.replies,release);os_atomic_store(&wfr.fsr,fault,release);os_atomic_store(&wfr.mhi,state,release);
            os_atomic_store(&wfr.state,2,release);
            printf("STAR WIFI SERVICE ONLINE: INIT_DONE=1 WLAN_CFG=1 SRNG=1 RX_posted=1 NORMAL=1 HTC_READY=1 WMI_CONNECTED=1 HTT_CONNECTED=1 HTC_SETUP_SUBMITTED=1 WMI_READY=1 HOST_MAC=1 HTT_VERSION_REQUESTED=1 RXDMA_SETUP_SUBMITTED=1 owner retained; polling control transport, no WLAN network yet\n");
            (void)sweet_log_checkpoint();
        }
        if(wm.phase==2 && resident && !wc_scan_tick(bar,ms,stopping)) {good=false;break;}
        if(wm.phase==2 && resident && !stopping && !wc_sta_tick(bar,ms)) {good=false;break;}
        os_atomic_store(&wfr.seconds,ms/1000,release);
        os_atomic_store(&wfr.events,*events,release);os_atomic_store(&wfr.replies,wqm.replies,release);
        os_atomic_store(&wfr.fsr,fault,release);os_atomic_store(&wfr.mhi,state,release);
        uint64_t deadline;clock_interval_to_deadline(resident?5:1,NSEC_PER_MSEC,&deadline);
        (void)assert_wait_deadline(&wfr,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
    }
    wc_dp_stop();
    renoir_wifi_net_stopped(!good);
    if(!good) {
        printf("STAR WLAN FAILURE BOUNDARY: ms=%u mode_phase=%u CE_touched=%u CE_enabled=%u pending=%u QMI_id=%#x CTRL=%#x STATUS=%#x EE=%u FSR=%#x; before CE cleanup\n",
            ms,wm.phase,wcs.touched,wcs.enabled,wqm.pending,wqm.message,wdrd(bar,0x38),wdrd(bar,0x48),rd(bar,bhi+0x28),wdrd(wdm.cb,0x58)&0xc00001feu);
        wc_srng_failure_snapshot(bar,bar_size);
        wc_rddm_collect(bar,bar_size);
    }
    if(stopping && wm.phase && wm.phase!=4) wm.off_failed=true;
    printf("STAR HTC CONNECT SUMMARY: pending=%u connected=%u endpoint=%u max_message=%u credits=%u TX_busy=%u submitted=%u completed=%u; see WMI INIT summary\n",wh.pending,wh.connected,wh.endpoint,wh.max_message,wh.credits,wct.busy,wct.sent,wct.completed);
    printf("STAR HTC HTT SUMMARY: connected=%u endpoint=%u max_message=%u pending_service=%#x; data counters reported separately\n",wh.htt_connected,wh.htt_endpoint,wh.htt_max,wh.service);
    printf("STAR WMI BOOT SUMMARY: setup_sent=%u consumed=%u events=%u TLVs=%u service_seen=%u unknown=%u; see WMI INIT summary; no WLAN network\n",wbs.setup_sent,wbs.setup_consumed,wbs.events,wbs.tlvs,wbs.service_seen,wbs.unknown);
    wc_cap_summary();wc_init_summary();wc_mac_summary();wc_scan_summary();wc_htt_summary();wc_rxdma_summary();wc_dp_summary();
    printf("STAR WMI TX SUMMARY: CE3 submitted=%u consumed=%u busy=%u credits=%u debt=%u; see WMI INIT summary\n",wwt.sent,wwt.completed,wwt.busy,wwt.available,wwt.debt);
    wc_mode_summary();wc_rx_summary();
    if(!wc_srng_stop(bar)) good=false;
    if(!good) {
        os_atomic_store(&wfr.failed,1,release);
        os_atomic_store(&wfr.fsr,wdrd(wdm.cb,0x58)&0xc00001feu,release);
        os_atomic_store(&wfr.mhi,wdrd(bar,0x48),release);
        wb_mhi_snapshot(bar,ctrl,bhi,"resident-or-init-failure");
    }
    os_atomic_store(&wfr.state,3,release);
    printf("STAR QRTR SUMMARY: errors=%u ms=%u tx=%u completed=%u rx=%u hello=%u announcements=%u; owner exiting, no WLAN network\n",
        !good,ms,wq.sent,wq.tx_done,wq.received,wq.hellos,wq.servers);
    printf("STAR QMI SUMMARY: service=%u pending=%u replies=%u indications=%u failed=%u\n",
        wqm.service,wqm.pending,wqm.replies,wqm.indications,wqm.failed);
    wq_wlfw_init_summary(bar);wq_wlfw_files_summary(bar);
    wq.active=false;
}
