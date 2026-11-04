/* QCA6490 Bluetooth: power-up, HS-UART in SE DMA mode, firmware and the HCI byte stream.
 * MiCode a9244def, in its own order:
 *  - btpower.c bt_configure_gpios(on): BT_EN (GPIO65) out-low, 50 ms; WLAN_EN
 *    high -> BT_EN high (else 100 ms more, then high); 50 ms; read SW_CTRL (91).
 *    Rails: the DT bt-vdd-* minimums (io 1.8V smpb10; aon/dig/rfacmn/rfa0p8
 *    0.95V smpc2; rfa1 1.88V smpb11; rfa2 1.35V smpb12; asd 2.8V ldoc11) equal
 *    the shared apps-DRV votes renoir_wifi_power() made and retains, so the
 *    aggregate already covers this consumer; nothing here lowers a rail.
 *  - pinctrl "active" for qupv3_se15: GPIO68/71 qup15 bias-disable, 69 qup15
 *    pull-down, 70 qup15 pull-up, 2 mA (pinctrl-shima: qup15 = function 1).
 *  - msm_geni_serial_port_setup (non-console: SE_DMA, packing 8/4/lsb),
 *    geni_se_init (rx_wm=depth/2, rfr=depth-2), geni_se_select_dma_mode,
 *    set_termios 115200 8N1 CRTSCTS with HALF_SAMPLING (16x): SE7 7.3728 MHz,
 *    divider 4; start_rx_sequencer (RX DMA 2048 bytes, S_CMD START_READ with
 *    RFR_OPEN); setup_tx + geni_se_tx_dma_prep.
 *  - DMA buffers through the wrapper's apps-SMMU stream 0x4c3 (DT iommus,
 *    qcom,iommu-dma-addr-pool 0x40000000), mapped like the Wi-Fi SID2401 bank.
 *  - The SE interrupt (INTID 640) runs msm_geni_serial_handle_isr in a
 *    thread (star_gpu_irq threaded adapter); one worker owns the H4 stream.
 *  - qca_setup, WCN6855/HSP branch (Linux 05441d697f7f): at the init speed
 *    115200 qca_read_soc_version's EDL_PATCH_VER_REQ (H4 01, opcode 0xfc00,
 *    1 byte 0x19; device 20261103-161143: product 0x13, patch 0x17f3, ROM
 *    0x0200, SoC 0x400c0200, i.e. hpbtfw20.tlv / hpnv20.*); qca_set_speed to
 *    the operating speed, the ADT transport-speed (0xfc48 with the QCA baud
 *    enum, flow control off/on around it, host termios via set_termios);
 *    then qca_uart_setup: rampatch TLV in 243-byte EDL_PATCH_TLV_REQ
 *    segments (download_mode 3 = SKIP_EVT_VSE_CC: only the last segment is
 *    acked), 10 ms, EDL_GET_BID_REQ (board ID 0 -> hpnv20.bin), NVM with
 *    qca_tlv_check_data's edits (HCI tag: in-band sleep bit, payload[1] =
 *    the same baud enum; deep-sleep bit), every NVM segment acked;
 *    qca_disable_soc_logging; HCI_Reset; qca_read_fw_build_info.
 *    The files come from this phone's bluetooth_a partition via the boot
 *    bundle (bt-hpbtfw20.tlv, bt-hpnv20.bin).
 * After a complete setup hci_qca clears QCA_IBS_DISABLED with both IBS
 * states ASLEEP, the address is set and the UART stays owned (bt_service).
 * The iOS side is renoir_bluetooth_transport.inc.
 * Not native: the serial clock votes of the IBS states are not applied
 * (clocks stay on) and the GPIO71 wake interrupt is not used; no
 * memdump/SSR handling. On a failed setup the RX sequencer is stopped, the
 * SE interrupt released, BT_EN goes low (100 ms, btpower off), the SID is
 * detached and the UART clocks/bus votes are released. */
#include <sys/star_gpu_irq.h>
#define BT_IOVA 0x40000000ULL
#define BT_DMA_BYTES 0x4000u
static struct {
    vm_offset_t smmu,cb,buffer,tables,nc;
    uint64_t pa,table_pa;
    unsigned slot,bank,asid;
    uint32_t old_smr,old_s2cr,saved_sctlr;
    bool published;
} bts;
static void bt_wr(vm_offset_t base,unsigned off,uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(base+off)=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void bt_sleep_ms(unsigned ms)
{
    uint64_t deadline;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
    (void)assert_wait_deadline(&bts,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
}
#define BT_FAIL(label) do { printf("STAR BT SMMU GATE: %s line=%u\n",label,(unsigned)__LINE__);return false; } while(0)
static bool bt_smmu_prepare_locked(void)
{
    bts.smmu=ml_io_map(0x15000000,0x2000);if(!bts.smmu) BT_FAIL("SMMU map failed");
    if(wdrd(bts.smmu,0x20)!=0x4c017e59 || wdrd(bts.smmu,0x24)!=0x6000003c || wdrd(bts.smmu,0x28)!=0x5111 ||
       (wdrd(bts.smmu,0)&9) || !(wdrd(bts.smmu,0)&(1u<<10))) BT_FAIL("unsupported SMMU state");
    bool used[60]={false};unsigned free_slot=89;
    for(unsigned i=0;i<89;++i) {
        uint32_t smr=wdrd(bts.smmu,0x800+4*i),s2cr=wdrd(bts.smmu,0xc00+4*i);
        if(!(smr>>31)) {if(free_slot==89) free_slot=i;continue;}
        if(((s2cr>>16)&3)==0 && (s2cr&255)<60) used[s2cr&255]=true;
        if(!(((smr&0x7fff)^0x4c3)&~((smr>>16)&0x7fff))) {
            printf("STAR BT SMMU: SID 0x4c3 already matched by slot %u smr=%#x s2cr=%#x\n",i,smr,s2cr);
            BT_FAIL("existing stream owner");
        }
    }
    if(free_slot==89) BT_FAIL("no free SMR");bts.slot=free_slot;
    vm_offset_t banks=ml_io_map(0x15080000,60u<<12);if(!banks) BT_FAIL("context bank map failed");
    for(bts.bank=0;bts.bank<60;++bts.bank)
        if(!used[bts.bank] && !(wdrd(banks,bts.bank<<12)&1u) && !(wdrd(banks,(bts.bank<<12)+0x58)&0xc00001feu) &&
           !wdrd(bts.smmu,0x1000+4*bts.bank) && !wdrd(bts.smmu,0x1800+4*bts.bank)) break;
    if(bts.bank==60) BT_FAIL("no unused clean context bank");
    bts.cb=banks+(bts.bank<<12);
    for(bts.asid=254;bts.asid>0;--bts.asid) {
        bool taken=false;
        for(unsigned i=0;i<60;++i) if(used[i] || (wdrd(banks,i<<12)&1u))
            if((wdrd(banks,(i<<12)+0x24)>>16)==bts.asid || (wdrd(banks,(i<<12)+0x2c)>>16)==bts.asid) taken=true;
        if(!taken) break;
    }
    if(!bts.asid) BT_FAIL("no collision-free ASID");
    if(kmem_alloc_contig(kernel_map,&bts.buffer,BT_DMA_BYTES,PAGE_MASK,0,0,KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS ||
       kmem_alloc_contig(kernel_map,&bts.tables,0x4000,PAGE_MASK,0,0,KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) BT_FAIL("allocation");
    ppnum_t bp=pmap_find_phys(kernel_pmap,(addr64_t)bts.buffer),tp=pmap_find_phys(kernel_pmap,(addr64_t)bts.tables);
    if(!bp || !tp) BT_FAIL("physical address");
    bts.pa=ptoa_64(bp);bts.table_pa=ptoa_64(tp);
    if((bts.pa&0x3fff) || (bts.table_pa&0x3fff) || bts.pa+BT_DMA_BYTES>(1ULL<<36) || bts.table_pa+0x4000>(1ULL<<36)) BT_FAIL("alignment/range");
    /* pmap_find_phys returns XNU 16 KiB pages: compare at that granule (as Wi-Fi's page&3). */
    for(unsigned page=0;page<BT_DMA_BYTES/0x1000;page+=PAGE_SIZE/0x1000)
        if(ptoa_64(pmap_find_phys(kernel_pmap,(addr64_t)(bts.buffer+0x1000ULL*page)))!=bts.pa+0x1000ULL*page) BT_FAIL("not contiguous");
    flush_dcache64((addr64_t)bts.table_pa,0x4000,TRUE);
    flush_dcache64((addr64_t)bts.pa,BT_DMA_BYTES,TRUE);
    vm_offset_t tables=ml_io_map_unmappable(bts.table_pa,0x4000,VM_WIMG_WCOMB);
    bts.nc=ml_io_map_unmappable(bts.pa,BT_DMA_BYTES,VM_WIMG_WCOMB);
    if(!tables || !bts.nc) BT_FAIL("NC mapping");
    /* 0x40000000: L1 index 1, L2 index 0, one leaf table (4 KiB pages). */
    uint64_t *l1=(uint64_t *)tables,*l2=(uint64_t *)(tables+0x1000),*leaf=(uint64_t *)(tables+0x2000);
    l1[1]=(bts.table_pa+0x1000)|3;l2[0]=(bts.table_pa+0x2000)|3;
    for(unsigned i=0;i<BT_DMA_BYTES/0x1000;++i) leaf[i]=(bts.pa+0x1000ULL*i)|(3ULL<<53)|0xe43;
    __asm__ volatile("dsb sy" ::: "memory");
    if(l1[1]!=((bts.table_pa+0x1000)|3) || l2[0]!=((bts.table_pa+0x2000)|3) || (leaf[0]&0xffffff000ULL)!=bts.pa) BT_FAIL("table readback");
    bts.old_smr=wdrd(bts.smmu,0x800+4*bts.slot);bts.old_s2cr=wdrd(bts.smmu,0xc00+4*bts.slot);
    bts.saved_sctlr=wdrd(bts.cb,0);
    wdwr(bts.smmu,0x1800+4*bts.bank,1);wdwr(bts.smmu,0x1000+4*bts.bank,0x1f300);
    wdwr(bts.cb,0x10,0x38011);wdwr(bts.cb,0x30,0x802020);
    uint64_t ttbr=bts.table_pa|((uint64_t)bts.asid<<48);
    wdwr64(bts.cb,0x20,ttbr);wdwr64(bts.cb,0x28,0);
    if(wdrd64(bts.cb,0x20)!=ttbr) BT_FAIL("TTBR readback");
    wdwr(bts.cb,0x38,0x44);wdwr(bts.cb,0x3c,0);
    wdwr(bts.cb,0x610,bts.asid);wdwr(bts.cb,0x7f0,0xffffffffu);
    unsigned us;for(us=0;us<10000 && (wdrd(bts.cb,0x7f4)&1);++us) delay(1);
    if(us==10000) BT_FAIL("TLB sync");
    wdwr(bts.cb,0,0x00c01027u);
    bts.published=true;
    wdwr(bts.smmu,0xc00+4*bts.slot,0x300|bts.bank);wdwr(bts.smmu,0x800+4*bts.slot,0x800004c3);
    if(wdrd(bts.smmu,0xc00+4*bts.slot)!=(0x300|bts.bank) || wdrd(bts.smmu,0x800+4*bts.slot)!=0x800004c3) BT_FAIL("SID publication");
    printf("STAR BT SMMU: SID0x4c3 slot%u CB%u ASID%u IOVA=%#llx bytes=%#x\n",bts.slot,bts.bank,bts.asid,BT_IOVA,BT_DMA_BYTES);
    return true;
}
#undef BT_FAIL
/* arm_smmu_destroy_domain_context order: detach the stream, then disable the bank. */
static void bt_smmu_retire_locked(void)
{
    if(!bts.published) return;
    wdwr(bts.smmu,0x800+4*bts.slot,bts.old_smr);wdwr(bts.smmu,0xc00+4*bts.slot,bts.old_s2cr);
    wdwr(bts.cb,0,0);wdwr(bts.cb,0x610,bts.asid);wdwr(bts.cb,0x7f0,0xffffffffu);
    for(unsigned us=0;us<10000 && (wdrd(bts.cb,0x7f4)&1);++us) delay(1);
    bts.published=false;
    printf("STAR BT SMMU: retired slot%u CB%u SMR=%#x\n",bts.slot,bts.bank,wdrd(bts.smmu,0x800+4*bts.slot));
}
/* TLMM pin windows; ml_io_map is permanent, so each pin is mapped once. */
static vm_offset_t bt_gpio(unsigned pin)
{
    static const unsigned pins[]={64,65,68,69,70,71,91};static vm_offset_t maps[7];
    for(unsigned i=0;i<7;++i) if(pins[i]==pin) {
        if(!maps[i]) maps[i]=ml_io_map(0x0f100000+pin*0x1000,0x1000);
        return maps[i];
    }
    return 0;
}
static void bt_gpio_cfg(unsigned pin,uint32_t cfg)
{
    vm_offset_t g=bt_gpio(pin);
    if(g) bt_wr(g,0,cfg);
}
static uint32_t bt_gpio_in(unsigned pin)
{
    vm_offset_t g=bt_gpio(pin);
    return g?(rd(g,4)&1):0xff;
}
static void bt_gpio_out(unsigned pin,unsigned value)
{
    vm_offset_t g=bt_gpio(pin);
    if(!g) return;
    bt_wr(g,4,value?2:0);
    bt_wr(g,0,(rd(g,0)&~0x3cu)|(1u<<9));    /* function gpio, output enable */
}
static void bt_hex(const char *what,const uint8_t *data,unsigned n)
{
    char line[3*64+1];unsigned k=0;
    for(unsigned i=0;i<n && i<64;++i) {
        static const char hx[]="0123456789abcdef";uint8_t b=data[i];
        line[k++]=hx[b>>4];line[k++]=hx[b&15];line[k++]=' ';
    }
    line[k]=0;printf("STAR BT %s: %u bytes: %s\n",what,n,line);
}

/* UART layer: msm_geni_serial in SE_DMA mode. The threaded handler of the
 * SE interrupt (DT GIC_SPI 0x260 level-high, INTID 640) is
 * msm_geni_serial_handle_isr; it alone consumes the M/S/DMA status. bu.xmit
 * is the xmit circ buffer (copied into the fixed TX DMA area); bu.rxq is the
 * TTY flip buffer the RX DMA is pushed into before geni_se_rx_dma_start.
 * When rxq has no room for a completed RX DMA, the buffer stays owned and
 * RFR is held not-ready until the worker has made room. */
#define BU_IRQ 640u
#define BU_RXQ 65536u
#define POLL_WAIT_TIMEOUT_MSEC 100
#define M_IRQ_BITS ((1u<<26)|(1u<<27)|(1u<<4)|(1u<<5))   /* RX_FIFO_WATERMARK/LAST, CMD_CANCEL/ABORT */
#define S_IRQ_BITS ((1u<<26)|(1u<<27)|(1u<<4)|(1u<<5))
#define M_ILLEGAL_CMD_EN (1u<<2)
#define CMD_CANCEL_ABORT_EN ((1u<<4)|(1u<<5))   /* M_/S_CMD_CANCEL_EN | M_/S_CMD_ABORT_EN */
#define S_RX_FIFO_WR_ERR_EN (1u<<25)
#define TX_DMA_DONE (1u<<0)
#define TX_RESET_DONE (1u<<3)
#define TX_GENI_CANCEL_IRQ (1u<<14)
#define DMA_TX_IRQ_BITS (TX_RESET_DONE|TX_DMA_DONE|TX_GENI_CANCEL_IRQ|(1u<<1)|(1u<<2))
#define RX_DMA_DONE (1u<<0)
#define RX_EOT (1u<<1)
#define RX_SBE (1u<<2)
#define RX_RESET_DONE (1u<<3)
#define UART_DMA_RX_PARITY_ERR (1u<<5)
#define UART_DMA_RX_ERRS (3u<<5)
#define UART_DMA_RX_BREAK (3u<<7)
#define RX_GENI_CANCEL_IRQ (1u<<14)
#define DMA_RX_IRQ_BITS (RX_EOT|RX_GENI_CANCEL_IRQ|RX_RESET_DONE|UART_DMA_RX_ERRS|UART_DMA_RX_PARITY_ERR|UART_DMA_RX_BREAK|RX_DMA_DONE|RX_SBE)
#define S_GENI_CMD_ACTIVE (1u<<12)
#define UART_MANUAL_RFR_EN (1u<<31)
#define UART_RFR_NOT_READY (1u<<1)
static LCK_MTX_DECLARE(bu_lock,&bt_sk_grp);
static struct {
    vm_offset_t base;
    uint32_t cur_baud;
    bool s_cmd_done,manual_flow,tx_busy;
    unsigned tx_size,xmit_len,hold;
    uint32_t rx_head,rx_len;
    uint8_t xmit[0x1000],rxq[BU_RXQ];
    uint64_t irqs,irq_wakes,tx_bytes,rx_bytes,tx_dmas,rx_dmas,parity,brk,overrun,sbe,illegal,holds,rx_err_drops;
} bu;
static void bu_rxq_put_locked(const uint8_t *data,unsigned n)
{
    for(unsigned i=0;i<n;++i) bu.rxq[(bu.rx_head+bu.rx_len+i)%BU_RXQ]=data[i];
    bu.rx_len+=n;
}
static void bu_rx_dma_start_locked(void)
{
    bt_wr(bu.base,0xd4c,7);bt_wr(bu.base,0xd30,(uint32_t)(BT_IOVA+0x1000));bt_wr(bu.base,0xd34,(uint32_t)((BT_IOVA+0x1000)>>32));
    bt_wr(bu.base,0xd38,0);bt_wr(bu.base,0xd3c,2048);
}
/* msm_geni_serial_prep_dma_tx: setup_tx + geni_se_tx_dma_prep. */
static void bu_prep_dma_tx_locked(void)
{
    unsigned n=bu.xmit_len;
    for(unsigned i=0;i<n;++i) *(volatile uint8_t *)(bts.nc+i)=bu.xmit[i];
    __asm__ volatile("dsb sy" ::: "memory");
    bt_wr(bu.base,0x270,n);bt_wr(bu.base,0x600,1u<<27);
    bt_wr(bu.base,0xc4c,7);bt_wr(bu.base,0xc30,(uint32_t)BT_IOVA);bt_wr(bu.base,0xc34,(uint32_t)(BT_IOVA>>32));
    bt_wr(bu.base,0xc38,1);bt_wr(bu.base,0xc3c,n);
    bu.tx_busy=true;bu.tx_size=n;
}
static void bu_set_manual_flow(bool enable)
{
    if(!enable) {
        bt_wr(bu.base,0x2ac,UART_MANUAL_RFR_EN);delay(10);
        bt_wr(bu.base,0x2ac,UART_MANUAL_RFR_EN|UART_RFR_NOT_READY);
    } else bt_wr(bu.base,0x2ac,0);
}
/* msm_geni_serial_handle_dma_rx; false when the buffer is kept for lack of room. */
static bool bu_handle_dma_rx_locked(bool drop)
{
    if(!(rd(bu.base,0x40)&S_GENI_CMD_ACTIVE)) return true;
    unsigned n=rd(bu.base,0xd54);
    if(!n) return true;
    if(n>2048) n=2048;
    if(drop) {bu.rx_err_drops+=n;return true;}
    if(n>BU_RXQ-bu.rx_len) {
        bu.hold=n;++bu.holds;
        bu_set_manual_flow(false);
        return false;
    }
    for(unsigned i=0;i<n;++i) {
        bu.rxq[(bu.rx_head+bu.rx_len+i)%BU_RXQ]=*(volatile uint8_t *)(bts.nc+0x1000+i);
        *(volatile uint8_t *)(bts.nc+0x1000+i)=0;
    }
    bu.rx_len+=n;bu.rx_bytes+=n;
    return true;
}
static bool bu_handle_tx_dma_xfer_locked(uint32_t m,bool *wake)
{
    uint32_t st=rd(bu.base,0xc40);
    if(st) {
        bt_wr(bu.base,0xc44,st);
        if(st&(TX_RESET_DONE|TX_GENI_CANCEL_IRQ)) return true;
        if(st&TX_DMA_DONE) {
            /* msm_geni_serial_handle_dma_tx */
            memmove(bu.xmit,bu.xmit+bu.tx_size,bu.xmit_len-bu.tx_size);
            bu.xmit_len-=bu.tx_size;bu.tx_bytes+=bu.tx_size;++bu.tx_dmas;
            bu.tx_busy=false;bu.tx_size=0;
            if(bu.xmit_len) bu_prep_dma_tx_locked();
            *wake=true;
        }
    }
    return (m&CMD_CANCEL_ABORT_EN)!=0;
}
static bool bu_handle_rx_dma_xfer_locked(uint32_t s,bool *wake)
{
    bool ret=false,drop=false;
    uint32_t st=rd(bu.base,0xd40);
    if(st) {
        bt_wr(bu.base,0xd44,st);
        if(st&RX_RESET_DONE) return true;
        if(st&UART_DMA_RX_ERRS) {
            if(st&UART_DMA_RX_PARITY_ERR) ++bu.parity;
            drop=true;
        } else if(st&UART_DMA_RX_BREAK) ++bu.brk;
        if(st&(RX_EOT|RX_DMA_DONE)) {
            ++bu.rx_dmas;
            if(bu_handle_dma_rx_locked(drop) && !(st&RX_GENI_CANCEL_IRQ)) bu_rx_dma_start_locked();
            *wake=true;
        }
        if(st&RX_SBE) ++bu.sbe;
        if(st&(RX_EOT|RX_GENI_CANCEL_IRQ|RX_DMA_DONE)) ret=true;
    }
    if(s&CMD_CANCEL_ABORT_EN) ret=true;
    return ret;
}
/* msm_geni_serial_handle_isr (DMA mode). */
static void bu_isr(void *arg __unused)
{
    bool wake=false,s_done=false;
    lck_mtx_lock(&bu_lock);
    ++bu.irqs;
    uint32_t m=rd(bu.base,0x610),s=rd(bu.base,0x640);
    bt_wr(bu.base,0x618,m);bt_wr(bu.base,0x648,s);
    if(m&M_ILLEGAL_CMD_EN) {++bu.illegal;wake=true;goto out;}
    if(s&S_RX_FIFO_WR_ERR_EN) {
        /* tty_insert_flip_char(tport, 0, TTY_OVERRUN): hci_ldisc ignores the flag. */
        static const uint8_t zero=0;
        ++bu.overrun;wake=true;
        if(bu.rx_len<BU_RXQ) bu_rxq_put_locked(&zero,1);
    }
    (void)bu_handle_tx_dma_xfer_locked(m,&wake);
    s_done=bu_handle_rx_dma_xfer_locked(s,&wake);
out:
    if(s_done) {bu.s_cmd_done=true;wakeup(&bu.s_cmd_done);}
    lck_mtx_unlock(&bu_lock);
    if(wake) {++bu.irq_wakes;bt_kick();}
}
static void bu_enable_interrupts(void)
{
    bt_wr(bu.base,0x614,rd(bu.base,0x614)|M_IRQ_BITS);
    bt_wr(bu.base,0x644,rd(bu.base,0x644)|S_IRQ_BITS);
    bt_wr(bu.base,0xc4c,DMA_TX_IRQ_BITS);bt_wr(bu.base,0xd4c,DMA_RX_IRQ_BITS);
}
/* geni_wait_for_cmd_done, interrupt case: true on timeout. */
static bool bu_wait_s_cmd_done(void)
{
    uint64_t deadline;clock_interval_to_deadline(POLL_WAIT_TIMEOUT_MSEC,NSEC_PER_MSEC,&deadline);
    lck_mtx_lock(&bu_lock);
    while(!bu.s_cmd_done &&
        lck_mtx_sleep_deadline(&bu_lock,LCK_SLEEP_DEFAULT,(event_t)&bu.s_cmd_done,THREAD_UNINT,deadline)!=THREAD_TIMED_OUT) {}
    bool timeout=!bu.s_cmd_done;
    lck_mtx_unlock(&bu_lock);
    return timeout;
}
static void bu_issue_s_ctrl(uint32_t value)
{
    lck_mtx_lock(&bu_lock);bu.s_cmd_done=false;bt_wr(bu.base,0x634,value);lck_mtx_unlock(&bu_lock);
}
static void bu_stop_rx_sequencer(void)
{
    vm_offset_t u=bu.base;
    if(!(rd(u,0x40)&S_GENI_CMD_ACTIVE)) return;
    bu_set_manual_flow(false);
    delay(2*(160u*1000000u/bu.cur_baud)+500);
    lck_mtx_lock(&bu_lock);
    if(rd(u,0xd40)) {
        /* Completion reached the hardware before its interrupt ran. */
        uint32_t s=rd(u,0x640);bool wake=false;
        bt_wr(u,0x648,s);
        printf("STAR BT UART: stop_rx interrupt delay rx_dma=%#x s_irq=%#x\n",rd(u,0xd40),s);
        (void)bu_handle_rx_dma_xfer_locked(s,&wake);
        if(wake) bt_kick();
    }
    lck_mtx_unlock(&bu_lock);
    bu_issue_s_ctrl(1u<<2);                                      /* geni_cancel_s_cmd */
    bool timeout=bu_wait_s_cmd_done(),active=(rd(u,0x40)&S_GENI_CMD_ACTIVE)!=0;
    if(timeout || active) {
        printf("STAR BT UART: stop_rx cancel failed timeout:%u is_rx_active:%u status=%#x dma_dbg=%#x\n",timeout,active,rd(u,0x40),rd(u,0xe40));
        bu_issue_s_ctrl(1u<<1);                                  /* geni_abort_s_cmd */
        timeout=bu_wait_s_cmd_done();active=(rd(u,0x40)&S_GENI_CMD_ACTIVE)!=0;
        if(timeout || active) printf("STAR BT UART: stop_rx abort fail timeout:%u is_rx_active:%u status=%#x\n",timeout,active,rd(u,0x40));
        lck_mtx_lock(&bu_lock);bu.s_cmd_done=false;bt_wr(u,0xd58,1);lck_mtx_unlock(&bu_lock);
        if(bu_wait_s_cmd_done()) printf("STAR BT UART: stop_rx rx fsm reset failed\n");
    }
    bu_enable_interrupts();
    bu_set_manual_flow(true);
}
static void bu_start_rx_sequencer(void)
{
    if(rd(bu.base,0x40)&S_GENI_CMD_ACTIVE) {
        lck_mtx_lock(&bu_lock);bu_rx_dma_start_locked();lck_mtx_unlock(&bu_lock);
        bu_stop_rx_sequencer();
    }
    lck_mtx_lock(&bu_lock);
    if(bu.hold) {printf("STAR BT UART: start_rx drops %u held RX bytes\n",bu.hold);bu.hold=0;}
    bu_rx_dma_start_locked();
    bt_wr(bu.base,0x630,(1u<<27)|(1u<<7));                      /* UART_START_READ | RFR_OPEN */
    bu_enable_interrupts();
    lck_mtx_unlock(&bu_lock);
}
/* msm_geni_serial_set_mctrl: RTS off = manual RFR not-ready. */
static void bu_set_mctrl(bool rts)
{
    lck_mtx_lock(&bu_lock);
    bu.manual_flow=!rts;
    bt_wr(bu.base,0x2ac,rts?0:(UART_MANUAL_RFR_EN|UART_RFR_NOT_READY));
    lck_mtx_unlock(&bu_lock);
}
/* msm_geni_serial_set_termios, 8N1, HALF_SAMPLING (16x); geni_se_clk_freq_match
 * (exact=false) over the SE7 table of gcc-shima.c. */
static void bu_set_termios(uint32_t baud,bool crtscts)
{
    static const uint32_t table[]={7372800,14745600,19200000,29491200,32000000,
        48000000,64000000,75000000,80000000,96000000,100000000};
    vm_offset_t u=bu.base;
    bu_stop_rx_sequencer();
    bu.cur_baud=baud;
    uint32_t desired=baud*16,clk=0,best=0;
    for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);++i) {
        uint32_t div=(table[i]+desired-1)/desired,delta=desired-table[i]/div;
        if(!best || delta<best) {clk=table[i];if(!delta) break;best=delta;}
    }
    uint32_t div=(clk+desired-1)/desired;
    int e=star_gpu_clock_set_rate(STAR_CLK_SE7,clk);
    bt_wr(u,0x48,1u|(div<<4));bt_wr(u,0x4c,1u|(div<<4));(void)rd(u,0x48);
    /* msm_geni_serial_termios_cfg */
    uint32_t tx_trans=rd(u,0x25c)&~1u,tx_parity=rd(u,0x2a4)&~1u,rx_trans=rd(u,0x280)&~(1u<<3),rx_parity=rd(u,0x2a8)&~1u;
    tx_trans=crtscts?tx_trans&~2u:tx_trans|2u;
    bt_wr(u,0x22c,0);bt_wr(u,0x25c,tx_trans);bt_wr(u,0x2a4,tx_parity);bt_wr(u,0x280,rx_trans);bt_wr(u,0x2a8,rx_parity);
    bt_wr(u,0x268,8);bt_wr(u,0x28c,8);bt_wr(u,0x26c,0);
    if(crtscts) bt_wr(u,0x2ac,0);
    printf("STAR BT UART: termios %u 8N1%s: src %u Hz (set_rate error %d, provider %u Hz) div %u -> %u bps, CLK_CFG M/S %#x/%#x\n",
        baud,crtscts?" CRTSCTS":"",clk,e,star_gpu_clock_get_rate(STAR_CLK_SE7),div,clk/div/16,rd(u,0x48),rd(u,0x4c));
    bu_start_rx_sequencer();
}
/* Whole buffer into xmit, or nothing; the caller is the only writer. */
static bool bu_write(const uint8_t *data,unsigned n)
{
    lck_mtx_lock(&bu_lock);
    bool ok=n<=sizeof(bu.xmit)-bu.xmit_len;
    if(ok && n) {
        memcpy(bu.xmit+bu.xmit_len,data,n);bu.xmit_len+=n;
        if(!bu.tx_busy) bu_prep_dma_tx_locked();
    }
    lck_mtx_unlock(&bu_lock);
    return ok;
}
static unsigned bu_room(void)
{
    lck_mtx_lock(&bu_lock);unsigned n=sizeof(bu.xmit)-bu.xmit_len;lck_mtx_unlock(&bu_lock);
    return n;
}
static bool bu_tx_empty(void)
{
    lck_mtx_lock(&bu_lock);bool empty=!bu.tx_busy && !bu.xmit_len;lck_mtx_unlock(&bu_lock);
    return empty;
}

/* H4/IBS worker (hci_qca): one byte stream for setup and service. Setup
 * commands wait for their command complete like __hci_cmd_sync_ev; after
 * setup (QCA_IBS_DISABLED cleared) host bytes follow qca_enqueue: ASLEEP ->
 * WAKE_IND, WAKING until WAKE_ACK with the 100 ms wake_retrans, AWAKE
 * re-arms the 2000 ms tx_idle_delay on enqueue, whose expiry sends
 * SLEEP_IND. Device WAKE_IND is acknowledged, SLEEP_IND marks RX asleep.
 * ACL debug logs (QCA_DEBUG_HANDLE) go to diag, not the host. The worker
 * sleeps on bt_work until an interrupt, a Skywalk TX/RX-space signal or the
 * earliest pending deadline. */
#define HCI_IBS_SLEEP_IND 0xfe
#define HCI_IBS_WAKE_IND 0xfd
#define HCI_IBS_WAKE_ACK 0xfc
#define QCA_DEBUG_HANDLE 0x2edc
static struct {
    bool ibs,service,rx_awake,waiting,done;
    enum {TX_ASLEEP,TX_WAKING,TX_AWAKE} tx_ibs;
    uint64_t retrans,idle,retrans_ticks,idle_ticks,last_stats;
    uint8_t ctrl[16];unsigned ctrl_len;
    uint8_t cmd[4+255];unsigned cmd_len;
    uint16_t wait_opcode;
    uint8_t ev[3+255];unsigned ev_len;             /* H4 event packet being parsed, ev[0]=4 */
    uint8_t reply[3+255];unsigned reply_len;       /* the waited command's complete event */
    /* h4_recv_buf state */
    uint8_t type,hdr[4];unsigned need,got,body;bool drop;
    uint8_t fwd[2048];unsigned fwd_len,fwd_off;
    uint64_t wakes,wacks,slps,recv_wakes,recv_wacks,recv_slps,diag,bad,setup_drops,unrouted;
    uint64_t waits,timeouts,last_errors;
} bw;
static bool bt_flush(void)
{
    while(bw.fwd_off<bw.fwd_len) {
        uint32_t n=renoir_bt_sk_rx_put(bw.fwd+bw.fwd_off,bw.fwd_len-bw.fwd_off);
        if(!n) return false;
        bw.fwd_off+=n;
    }
    bw.fwd_len=bw.fwd_off=0;
    return true;
}
static void bt_stage(const uint8_t *data,unsigned n)
{
    memcpy(bw.fwd+bw.fwd_len,data,n);bw.fwd_len+=n;
}
static void bt_ibs_send(uint8_t byte)
{
    if(bw.ctrl_len<sizeof(bw.ctrl)) bw.ctrl[bw.ctrl_len++]=byte;
}
static void bt_event(void)
{
    if(bw.waiting && !bw.done && bw.ev[1]==0x0e && bw.ev_len>=6 && (bw.ev[4]|(bw.ev[5]<<8))==bw.wait_opcode) {
        memcpy(bw.reply,bw.ev,bw.ev_len);bw.reply_len=bw.ev_len;
        bw.done=true;return;
    }
    if(bw.service) {bt_stage(bw.ev,bw.ev_len);return;}
    ++bw.unrouted;
    bt_hex("RX unrouted event",bw.ev,bw.ev_len);
}
/* qca_recv: returns the bytes consumed; stops while Skywalk has no room. */
static unsigned bt_parse(const uint8_t *in,unsigned len)
{
    unsigned i=0;
    while(i<len) {
        if(!bt_flush()) return i;
        if(!bw.need && !bw.body) {
            uint8_t b=in[i++];
            if(b==HCI_IBS_WAKE_IND) {
                ++bw.recv_wakes;bw.rx_awake=true;bt_ibs_send(HCI_IBS_WAKE_ACK);++bw.wacks;
            } else if(b==HCI_IBS_SLEEP_IND) {++bw.recv_slps;bw.rx_awake=false;}
            else if(b==HCI_IBS_WAKE_ACK) {
                ++bw.recv_wacks;
                if(bw.tx_ibs==TX_WAKING) {bw.tx_ibs=TX_AWAKE;bw.idle=mach_absolute_time()+bw.idle_ticks;}
            } else if(b==2 || b==5) {bw.type=b;bw.need=4;bw.got=0;}
            else if(b==3) {bw.type=b;bw.need=3;bw.got=0;}
            else if(b==4) {bw.type=b;bw.need=2;bw.got=0;}
            else {
                /* h4_recv_buf: unknown type -> -EILSEQ, rest of the received data dropped */
                ++bw.bad;printf("STAR BT H4: unknown packet type %#x, %u bytes dropped\n",b,len-i+1);
                return len;
            }
            continue;
        }
        if(bw.need) {
            bw.hdr[bw.got++]=in[i++];
            if(bw.got<bw.need) continue;
            bw.need=0;
            uint8_t t=bw.type;
            bw.body=t==4?bw.hdr[1]:t==3?bw.hdr[2]:t==2?(unsigned)(bw.hdr[2]|(bw.hdr[3]<<8)):(unsigned)((bw.hdr[2]|(bw.hdr[3]<<8))&0x3fff);
            bw.drop=t!=4 && (!bw.service || (t==2 && (bw.hdr[0]|(bw.hdr[1]<<8))==QCA_DEBUG_HANDLE));
            if(t!=4 && bw.drop) {if(bw.service) ++bw.diag; else ++bw.setup_drops;}
            if(t==4) {bw.ev[0]=4;bw.ev[1]=bw.hdr[0];bw.ev[2]=bw.hdr[1];bw.ev_len=3;}
            else if(!bw.drop) {bt_stage(&t,1);bt_stage(bw.hdr,bw.got);}
            if(!bw.body && t==4) bt_event();
            continue;
        }
        unsigned n=len-i<bw.body?len-i:bw.body;
        if(bw.type==4) {memcpy(bw.ev+bw.ev_len,in+i,n);bw.ev_len+=n;}
        else if(!bw.drop) {if(n>sizeof(bw.fwd)) n=sizeof(bw.fwd);bt_stage(in+i,n);}
        i+=n;bw.body-=n;
        if(!bw.body && bw.type==4) bt_event();
    }
    (void)bt_flush();
    return i;
}
static void bt_rx_process(void)
{
    static uint8_t chunk[2048];
    for(;;) {
        lck_mtx_lock(&bu_lock);
        unsigned n=bu.rx_len<sizeof(chunk)?bu.rx_len:sizeof(chunk);
        for(unsigned i=0;i<n;++i) chunk[i]=bu.rxq[(bu.rx_head+i)%BU_RXQ];
        lck_mtx_unlock(&bu_lock);
        unsigned used=n?bt_parse(chunk,n):(bt_flush(),0);
        lck_mtx_lock(&bu_lock);
        bu.rx_head=(bu.rx_head+used)%BU_RXQ;bu.rx_len-=used;
        if(bu.hold && bu.hold<=BU_RXQ-bu.rx_len) {
            /* The held RX DMA: push, re-arm, release RFR. */
            for(unsigned i=0;i<bu.hold;++i) {
                bu.rxq[(bu.rx_head+bu.rx_len+i)%BU_RXQ]=*(volatile uint8_t *)(bts.nc+0x1000+i);
                *(volatile uint8_t *)(bts.nc+0x1000+i)=0;
            }
            bu.rx_len+=bu.hold;bu.rx_bytes+=bu.hold;bu.hold=0;
            bu_rx_dma_start_locked();
            if(!bu.manual_flow) bt_wr(bu.base,0x2ac,0);
        }
        bool more=bu.rx_len!=0;
        lck_mtx_unlock(&bu_lock);
        if(!n || used<n || !more) return;
    }
}
static void bt_stats(const char *why)
{
    lck_mtx_lock(&bu_lock);
    printf("STAR BT STATS(%s): irq=%llu irq_wakes=%llu waits=%llu deadline_wakes=%llu tx=%llu B/%llu DMA rx=%llu B/%llu DMA "
        "parity=%llu break=%llu overrun=%llu sbe=%llu illegal=%llu rx_err_drop=%llu holds=%llu "
        "IBS sent wake=%llu wack=%llu sleep=%llu recv wake=%llu wack=%llu sleep=%llu H4 bad=%llu diag=%llu setup_drop=%llu unrouted=%llu sk_tx=%llu sk_rx=%llu sk_rx_dropped=%llu\n",
        why,bu.irqs,bu.irq_wakes,bw.waits,bw.timeouts,bu.tx_bytes,bu.tx_dmas,bu.rx_bytes,bu.rx_dmas,
        bu.parity,bu.brk,bu.overrun,bu.sbe,bu.illegal,bu.rx_err_drops,bu.holds,
        bw.wakes,bw.wacks,bw.slps,bw.recv_wakes,bw.recv_wacks,bw.recv_slps,bw.bad,bw.diag,bw.setup_drops,bw.unrouted,
        bt_sk_kicks_tx,bt_sk_kicks_rx,bt_sk.rx_dropped);
    lck_mtx_unlock(&bu_lock);
}
/* One worker pass: RX, IBS deadlines, TX. */
static void bt_step(void)
{
    bt_rx_process();
    uint64_t now=mach_absolute_time();
    bool pending=bw.cmd_len || (bw.service && renoir_bt_sk_tx_pending());
    if(bw.ibs) {
        if(pending && bw.tx_ibs==TX_ASLEEP) {
            bw.tx_ibs=TX_WAKING;bt_ibs_send(HCI_IBS_WAKE_IND);++bw.wakes;bw.retrans=now+bw.retrans_ticks;
        } else if(bw.tx_ibs==TX_WAKING && now>=bw.retrans) {
            bt_ibs_send(HCI_IBS_WAKE_IND);++bw.wakes;bw.retrans=now+bw.retrans_ticks;
        }
    }
    if(bw.ctrl_len && bu_write(bw.ctrl,bw.ctrl_len)) bw.ctrl_len=0;
    if(!bw.ctrl_len && (!bw.ibs || bw.tx_ibs==TX_AWAKE)) {
        bool sent=false;
        if(bw.cmd_len && bu_write(bw.cmd,bw.cmd_len)) {bw.cmd_len=0;sent=true;}
        if(!bw.cmd_len && bw.service) {
            static uint8_t out[0x1000];
            unsigned room=bu_room();
            uint32_t n=room?renoir_bt_sk_tx_take(out,room):0;
            if(n) {(void)bu_write(out,n);sent=true;}
        }
        if(bw.ibs && sent) bw.idle=now+bw.idle_ticks;
        pending=bw.cmd_len || (bw.service && renoir_bt_sk_tx_pending());
        if(bw.ibs && bw.tx_ibs==TX_AWAKE && !pending && now>=bw.idle) {
            bt_ibs_send(HCI_IBS_SLEEP_IND);++bw.slps;bw.tx_ibs=TX_ASLEEP;
            if(bu_write(bw.ctrl,bw.ctrl_len)) bw.ctrl_len=0;
            if(bw.service && now-bw.last_stats>=30*bw.idle_ticks/2) {bt_stats("tx sleep");bw.last_stats=now;}
        }
    }
    uint64_t errors=bu.parity+bu.brk+bu.overrun+bu.sbe+bu.illegal+bw.bad;
    if(errors!=bw.last_errors) {bw.last_errors=errors;bt_stats("error");}
}
/* Sleep until a signal or the earliest of `deadline` (0: none) and the armed
 * IBS deadline; signals that arrived since the last pass are still counted. */
static void bt_wait(uint64_t deadline)
{
    uint64_t next=deadline;
    if(bw.ibs && bw.tx_ibs==TX_WAKING && (!next || bw.retrans<next)) next=bw.retrans;
    if(bw.ibs && bw.tx_ibs==TX_AWAKE && (!next || bw.idle<next)) next=bw.idle;
    ++bw.waits;
    if(!next) (void)semaphore_wait(bt_work);
    else if(semaphore_wait_deadline(bt_work,next)==KERN_OPERATION_TIMED_OUT) ++bw.timeouts;
}
/* Work, then the caller's condition, and only then a wait. */
static bool bt_run(bool (*done)(void),uint64_t deadline)
{
    for(;;) {
        bt_step();
        if(done()) return true;
        if(mach_absolute_time()>=deadline) return false;
        bt_wait(deadline);
    }
}
static bool bt_cmd_queued(void) {return !bw.cmd_len;}
static bool bt_cmd_completed(void) {return bw.done;}
static bool bt_tx_drained(void) {return bu_tx_empty();}
/* __hci_cmd_send: queued once the previous command left for the UART. */
static int bt_send(const uint8_t *packet,unsigned n,uint64_t deadline)
{
    if(!bt_run(bt_cmd_queued,deadline)) return ETIMEDOUT;
    memcpy(bw.cmd,packet,n);bw.cmd_len=n;
    return bt_run(bt_cmd_queued,deadline)?0:ETIMEDOUT;
}
/* __hci_cmd_sync_ev(event 0): command complete for opcode in HCI_INIT_TIMEOUT;
 * status 0 (and the EDL rtype when given). */
static int bt_cmd(uint16_t opcode,const uint8_t *params,unsigned n,int rtype,const char *name,bool log)
{
    uint8_t packet[4+255];
    packet[0]=1;packet[1]=opcode&255;packet[2]=opcode>>8;packet[3]=n;
    for(unsigned i=0;i<n;++i) packet[4+i]=params[i];
    uint64_t start=mach_absolute_time(),deadline;clock_interval_to_deadline(10000,NSEC_PER_MSEC,&deadline);
    bw.wait_opcode=opcode;bw.done=false;bw.reply_len=0;bw.waiting=true;
    int e=bt_send(packet,4+n,deadline);
    if(!e) (void)bt_run(bt_cmd_completed,deadline);
    bw.waiting=false;
    uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time()-start,&ns);
    if(!bw.done) {printf("STAR BT RX: %s no command complete in %llu ms%s\n",name,ns/NSEC_PER_MSEC,e?" (not sent)":"");return ETIMEDOUT;}
    if(log) {printf("STAR BT RX: %s ms=%llu\n",name,ns/NSEC_PER_MSEC);bt_hex("RX EVENT",bw.reply,bw.reply_len);}
    return bw.reply_len>=7 && bw.reply[6]==0 && (rtype<0 || (bw.reply_len>=8 && bw.reply[7]==rtype))?0:EIO;
}
static int bt_edl(uint8_t request,const char *name)
{
    return bt_cmd(0xfc00,&request,1,request,name,true);
}
/* qca_download_firmware + qca_tlv_send_segment (WCN3991-style command complete acks). */
static int bt_download(const uint8_t *data,unsigned size,bool skip_acks,const char *name)
{
    uint8_t packet[4+2+243];unsigned segments=0;
    uint64_t start=mach_absolute_time(),ns;
    for(unsigned at=0;at<size;) {
        unsigned seg=size-at>243?243:size-at;
        bool ack=!skip_acks || at+seg==size || seg<243;
        int e;
        if(ack) {
            packet[0]=0x1e;packet[1]=seg;
            for(unsigned i=0;i<seg;++i) packet[2+i]=data[at+i];
            e=bt_cmd(0xfc00,packet,2+seg,0x1e,name,false);
        } else {
            packet[0]=1;packet[1]=0x00;packet[2]=0xfc;packet[3]=seg+2;packet[4]=0x1e;packet[5]=seg;
            for(unsigned i=0;i<seg;++i) packet[6+i]=data[at+i];
            uint64_t deadline;clock_interval_to_deadline(10000,NSEC_PER_MSEC,&deadline);
            e=bt_send(packet,6+seg,deadline);
        }
        ++segments;at+=seg;
        if(e) {
            printf("STAR BT %s: segment %u failed (%d)\n",name,segments,e);
            if(e==EIO) bt_hex("RX EVENT",bw.reply,bw.reply_len);
            return e;
        }
    }
    absolutetime_to_nanoseconds(mach_absolute_time()-start,&ns);
    printf("STAR BT %s: %u bytes in %u segments acked, %llu ms\n",name,size,segments,ns/NSEC_PER_MSEC);
    return 0;
}
/* qca_get_baudrate_value */
static uint8_t bt_qca_baudrate(uint32_t speed)
{
    switch(speed) {
    case 9600: return 4;
    case 19200: return 3;
    case 38400: return 2;
    case 57600: return 1;
    case 230400: return 5;
    case 460800: return 7;
    case 500000: return 8;
    case 921600: return 10;
    case 1000000: return 11;
    case 2000000: return 13;
    case 3000000: return 14;
    case 3200000: return 0x11;
    case 3500000: return 0x12;
    default: return 0;
    }
}
/* The operating speed (hu->oper_speed): ADT /arm-io/bluetooth transport-speed,
 * which bluetoothd also applies with cfsetspeed. */
static uint32_t bt_oper_speed(void)
{
    DTEntry node;const void *data;unsigned length=0;
    if(SecureDTLookupEntry(NULL,"/arm-io/bluetooth",&node)!=kSuccess ||
       SecureDTGetProperty(node,"transport-speed",&data,&length)!=kSuccess || length!=4) return 0;
    return *(const uint32_t *)data;
}
/* qca_set_speed(QCA_OPER_SPEED), WCN6855 branch, with the tty-path
 * hci_uart_set_flow_control and hci_uart_set_baudrate. */
static int bt_set_speed(uint32_t speed,uint8_t qca_baudrate)
{
    /* hci_uart_set_flow_control(hu, true): termios without CRTSCTS, then RTS off */
    bu_set_termios(bu.cur_baud,false);
    bu_set_mctrl(false);
    /* qca_set_baudrate: queued, sent, CMD_TRANS_TIMEOUT for the UART to drain */
    const uint8_t cmd[]={0x01,0x48,0xfc,0x01,qca_baudrate};
    uint64_t deadline;clock_interval_to_deadline(10000,NSEC_PER_MSEC,&deadline);
    int e=bt_send(cmd,sizeof(cmd),deadline);
    if(!e) {
        clock_interval_to_deadline(POLL_WAIT_TIMEOUT_MSEC,NSEC_PER_MSEC,&deadline);
        bool sent=bt_run(bt_tx_drained,deadline);
        printf("STAR BT SPEED: 0xfc48 %#x %s at %u\n",qca_baudrate,sent?"sent":"not drained in 100 ms",bu.cur_baud);
        bt_sleep_ms(1);                                          /* usleep_range(1000, 10000) */
        bu_set_termios(speed,false);                             /* host_set_baudrate */
    } else printf("STAR BT SPEED: 0xfc48 not queued (%d)\n",e);
    /* hci_uart_set_flow_control(hu, false): RTS on, then termios with CRTSCTS */
    bu_set_mctrl(true);
    bu_set_termios(bu.cur_baud,true);
    return e;
}
/* hci_dev_setup after hdev->setup: with a public address property the core
 * calls hdev->set_bdaddr = qca_set_bdaddr (EDL_WRITE_BD_ADDR 0xfc14, the
 * address byte-swapped, __hci_cmd_sync_ev for HCI_INIT_TIMEOUT) before the
 * device is up. IBS is already enabled then, so it goes through the same
 * qca_enqueue path; its command complete stays in the kernel. The address
 * is the ADT's /arm-io/bluetooth local-mac-address (display byte order). */
static bool bt_local_address(uint8_t address[6])
{
    DTEntry node;const void *data;unsigned length=0;
    if(SecureDTLookupEntry(NULL,"/arm-io/bluetooth",&node)!=kSuccess ||
       SecureDTGetProperty(node,"local-mac-address",&data,&length)!=kSuccess || length!=6) return false;
    memcpy(address,data,6);
    for(unsigned i=0;i<6;++i) if(address[i]) return true;
    return false;
}
static __attribute__((noreturn)) void bt_service(void)
{
    printf("STAR BT SERVICE: UART owned, IRQ %u driven, IBS tx/rx ASLEEP, wake_retrans=100 ms tx_idle=2000 ms\n",BU_IRQ);
    bw.service=true;
    bt_stats("service start");bw.last_stats=mach_absolute_time();
    for(;;) {bt_step();bt_wait(0);}
}
static void renoir_bluetooth_hci_run(void *arg __unused,wait_result_t result __unused)
{
    struct star_gpu_icc *core=NULL,*ddr=NULL;
    vm_offset_t uart=0;unsigned clocks=0;int e=0;bool powered=false,smmu=false,irq=false;
    const enum star_gpu_clock_id order[]={STAR_CLK_QUP0_M_AHB,STAR_CLK_QUP0_S_AHB,STAR_CLK_SE7};
    unsigned wl=bt_gpio_in(64);
    printf("STAR BT POWER: rails=shared Wi-Fi votes (BT minimums equal); WLAN_EN=%u SW_CTRL=%u before\n",wl,bt_gpio_in(91));
    if(wl!=1) {printf("STAR BT POWER: WLAN_EN not high; no BT power-up\n");goto done;}
    /* bt_en_sleep pinctrl: gpio, 2 mA, pull-down, output-low */
    bt_gpio_cfg(65,1u|(1u<<9));bt_gpio_out(65,0);bt_sleep_ms(50);
    bt_gpio_out(65,1);powered=true;bt_sleep_ms(50);
    printf("STAR BT POWER: BT_EN=%u SW_CTRL=%u after 50 ms\n",bt_gpio_in(65),bt_gpio_in(91));
    /* qupv3_se15 active pinctrl */
    bt_gpio_cfg(68,1u<<2);bt_gpio_cfg(71,1u<<2);bt_gpio_cfg(69,(1u<<2)|1u);bt_gpio_cfg(70,(1u<<2)|3u);
    e=star_gpu_clock_qup0_open();if(e)goto done;
    e=star_gpu_icc_get_qup0(&core,&ddr);if(e)goto done;
    e=star_gpu_icc_set(core,50000,100000);if(e)goto done;
    e=star_gpu_icc_set(ddr,0,76800);if(e)goto done;
    e=star_gpu_clock_set_rate(STAR_CLK_SE7,7372800);if(e)goto done;
    for(;clocks<3;clocks++){e=star_gpu_clock_prepare_enable(order[clocks]);if(e)goto done;}
    uart=ml_io_map_unmappable(0x99c000,0x4000,VM_WIMG_IO);if(!uart){e=ENOMEM;goto done;}
    if(((rd(uart,0x68)>>8)&255)!=2){e=ENXIO;goto done;}
    renoir_dma_lock();smmu=bt_smmu_prepare_locked();renoir_dma_unlock();
    if(!smmu){e=EIO;goto done;}
    unsigned depth=(rd(uart,0xe28)>>16)&63;
    /* port_setup */
    bt_wr(uart,0x294,160);                                   /* RX_STALE_CNT = 10 bits * 16 */
    bt_wr(uart,0x260,0x4380e);bt_wr(uart,0x264,0xc3e0e);     /* TX packing 8/4 */
    bt_wr(uart,0x284,0x4380e);bt_wr(uart,0x288,0xc3e0e);     /* RX packing 8/4 */
    /* geni_se_init: se_io_init, se_io_set_mode, rfr/wm, common IRQ enables */
    bt_wr(uart,0x28,rd(uart,0x28)|0x7f);bt_wr(uart,0xe30,rd(uart,0xe30)|0xf);
    bt_wr(uart,0x24,0x7f);bt_wr(uart,0x20,1);
    bt_wr(uart,0xe1c,rd(uart,0xe1c)|0xf);bt_wr(uart,0x258,rd(uart,0x258)&~1u);bt_wr(uart,0xe18,0);
    bt_wr(uart,0x810,depth/2);bt_wr(uart,0x814,depth-2);
    bt_wr(uart,0x614,rd(uart,0x614)|0x7eu|(1u<<22)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<28)|(1u<<29));
    bt_wr(uart,0x644,rd(uart,0x644)|0x3eu|0x3e00u|(1u<<24)|(1u<<25));
    /* geni_se_select_dma_mode (UART keeps its M IRQ enables) */
    bt_wr(uart,0xe18,0);bt_wr(uart,0x618,~0u);bt_wr(uart,0x648,~0u);bt_wr(uart,0xc44,~0u);bt_wr(uart,0xd44,~0u);
    bt_wr(uart,0xe1c,~0u);bt_wr(uart,0x258,rd(uart,0x258)|1u);
    /* Worker state and the SE interrupt (IRQF_NOAUTOEN in MiCode: visible
     * only after the port state and DMA mapping exist). */
    bu.base=uart;bu.cur_baud=115200;
    nanoseconds_to_absolutetime(100*NSEC_PER_MSEC,&bw.retrans_ticks);
    nanoseconds_to_absolutetime(2000*NSEC_PER_MSEC,&bw.idle_ticks);
    if(semaphore_create(kernel_task,&bt_work,SYNC_POLICY_FIFO,0)!=KERN_SUCCESS) {bt_work=SEMAPHORE_NULL;e=ENOMEM;goto done;}
    e=star_gpu_irq_request_threaded(BU_IRQ,bu_isr,NULL);
    printf("STAR BT UART: IRQ %u (GIC SPI 0x260 level-high) threaded request error=%d\n",BU_IRQ,e);
    if(e) goto done;
    irq=true;
    /* set_termios 115200 8N1 CRTSCTS + start_rx_sequencer (RX DMA 2048, START_READ | RFR_OPEN) */
    bu_set_termios(115200,true);
    printf("STAR BT UART: SE_DMA depth=%u status=%#x\n",depth,rd(uart,0x40));
    e=bt_edl(0x19,"EDL_PATCH_VER_REQ");
    uint32_t speed=bt_oper_speed();uint8_t qca_baudrate=0;            /* QCA_BAUDRATE_115200 */
    if(!e && speed) {
        qca_baudrate=bt_qca_baudrate(speed);
        e=bt_set_speed(speed,qca_baudrate);
    }
    if(!e) {
        uint64_t psize=0,nsize=0;
        const uint8_t *patch=renoir_adsp_fw_file("bt-hpbtfw20.tlv",&psize),*nvm=renoir_adsp_fw_file("bt-hpnv20.bin",&nsize);
        static uint8_t nvm_copy[8192];
        if(!patch || !nvm || psize<28 || nsize<16 || nsize>sizeof(nvm_copy) || patch[0]!=1 || nvm[0]!=2) {
            printf("STAR BT FIRMWARE: bundle has no valid bt-hpbtfw20.tlv/bt-hpnv20.bin (%llu/%llu bytes)\n",psize,nsize);e=ENOENT;
        } else {
            printf("STAR BT FIRMWARE: rampatch %llu bytes download_mode=%u product=%#x rom=%#x patch=%#x; NVM %llu bytes; UART %u (QCA baud %#x)\n",
                psize,patch[14],patch[16]|(patch[17]<<8),patch[18]|(patch[19]<<8),patch[20]|(patch[21]<<8),nsize,bu.cur_baud,qca_baudrate);
            e=bt_download(patch,(unsigned)psize,patch[14]==3,"RAMPATCH");
            if(!e) {
                bt_sleep_ms(10);
                /* WCN6855 NVM name: qca_read_fw_board_id; its failure leaves board ID 0 */
                if(!bt_edl(0x23,"EDL_GET_BID_REQ")) printf("STAR BT FIRMWARE: board ID %#x -> hpnv20.bin\n",bw.reply_len>=11?(bw.reply[9]<<8)|bw.reply[10]:0);
                memcpy(nvm_copy,nvm,nsize);
                unsigned length=(nvm_copy[1]|(nvm_copy[2]<<8)|(nvm_copy[3]<<16)),edits=0;
                for(unsigned idx=0;idx+12<=length && 4+idx+12<=nsize;) {
                    uint8_t *tag=nvm_copy+4+idx;unsigned id=tag[0]|(tag[1]<<8),len=tag[2]|(tag[3]<<8);
                    if(4+idx+12+len>nsize) break;
                    if(id==17 && len>=3) {tag[12]|=0x80;tag[13]=qca_baudrate;++edits;}   /* IBS on, user baud */
                    if(id==27 && len>=1) {tag[12]|=0x01;++edits;}                      /* deep sleep */
                    idx+=12+len;
                }
                printf("STAR BT NVM: tag edits=%u (HCI in-band sleep + baud %#x, deep sleep)\n",edits,qca_baudrate);
                e=edits==2?bt_download(nvm_copy,(unsigned)nsize,false,"NVM"):EINVAL;
            }
            if(!e) {const uint8_t p[2]={0x14,0x00};e=bt_cmd(0xfc17,p,2,-1,"DISABLE_SOC_LOGGING",true);}
            if(!e) e=bt_cmd(0x0c03,NULL,0,-1,"HCI_RESET",true);
            if(!e) e=bt_edl(0x20,"EDL_GET_BUILD_INFO");
            if(!e) {
                /* qca_read_fw_build_info: data[0] is the label length */
                char label[64];unsigned n=bw.reply[8],len=bw.reply_len;
                if(n>sizeof(label)-1) n=sizeof(label)-1;
                if(9+n>len) n=len>9?len-9:0;
                for(unsigned i=0;i<n;++i) label[i]=(char)bw.reply[9+i];
                label[n]=0;
                printf("STAR BT HCI SUMMARY: rampatch/NVM/HCI_Reset at %u done; firmware %s\n",bu.cur_baud,label);
                bw.ibs=true;bw.tx_ibs=TX_ASLEEP;bw.rx_awake=false;      /* clear_bit(QCA_IBS_DISABLED) */
                uint8_t address[6];
                if(bt_local_address(address)) {
                    uint8_t swapped[6];
                    for(unsigned i=0;i<6;++i) swapped[i]=address[5-i];       /* baswap */
                    printf("STAR BT SERVICE: set_bdaddr %02x:%02x:%02x:%02x:%02x:%02x via EDL_WRITE_BD_ADDR\n",
                        address[0],address[1],address[2],address[3],address[4],address[5]);
                    e=bt_cmd(0xfc14,swapped,6,-1,"EDL_WRITE_BD_ADDR",true);
                } else printf("STAR BT SERVICE: no ADT local-mac-address; controller keeps its NVM address\n");
                if(!e) {
                    renoir_bt_sk_boot_result(0,label,bu.cur_baud);
                    bt_service();
                }
            }
        }
    }
done:
    renoir_bt_sk_boot_result(e?e:EIO,NULL,0);
    if(irq) {
        bu_stop_rx_sequencer();
        bt_wr(uart,0x614,0);bt_wr(uart,0x644,0);bt_wr(uart,0xc50,~0u);bt_wr(uart,0xd50,~0u);bt_wr(uart,0xe1c,0);
        star_gpu_irq_free(BU_IRQ);
        bt_stats("setup failed");
    }
    if(bt_work) {semaphore_t s=bt_work;bt_work=SEMAPHORE_NULL;semaphore_destroy(kernel_task,s);}
    if(powered) {bt_gpio_out(65,0);bt_sleep_ms(100);}
    if(smmu||bts.published){renoir_dma_lock();bt_smmu_retire_locked();renoir_dma_unlock();}
    while(clocks)star_gpu_clock_disable_unprepare(order[--clocks]);
    int cleanup=0;
    if(core){cleanup=star_gpu_icc_set(core,0,0);star_gpu_icc_put(core);}
    if(ddr){int r=star_gpu_icc_set(ddr,0,0);if(r)cleanup=r;star_gpu_icc_put(ddr);}
    if(uart) bt_unmap(uart,0x4000);
    printf("STAR BT HCI SUMMARY: error=%d cleanup=%d powered_then_off=%u smmu=%u irq=%u; setup failed, no iOS transport\n",e,cleanup,powered,smmu,irq);
    thread_terminate_self();
}
static void renoir_bluetooth_hci_start(void)
{
    int enabled=1;
    PE_parse_boot_argn("renoirbt",&enabled,sizeof(enabled));
    if(!enabled) {renoir_bt_sk_boot_result(ENODEV,NULL,0);return;}
    thread_t thread;
    if(kernel_thread_start(renoir_bluetooth_hci_run,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
    else {printf("STAR BT HCI: worker not started\n");renoir_bt_sk_boot_result(ENOMEM,NULL,0);}
}
