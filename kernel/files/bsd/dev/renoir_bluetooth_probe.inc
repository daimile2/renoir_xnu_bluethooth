/* MiCode msm_geni_serial_get_ver_info resource phase, HSUART branch.
 * DMA/IRQ/HCI are not registered yet: leave engine interrupt masks and
 * command/FIFO registers untouched. Protocol=UART is required before DMA.
 * The wrapper's two live-DT paths vote 50000/100000 and 0/76800, respectively.
 */
#include <sys/star_gpu_clock_provider.h>
#include <sys/star_gpu_icc.h>
#include <sys/errno.h>
#include <vm/pmap.h>
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,uint32_t);
extern void ml_io_unmap(vm_offset_t,vm_size_t);
static void bt_unmap(vm_offset_t address,vm_size_t size)
{
    if(address)ml_io_unmap(trunc_page(address),round_page(size+(address-trunc_page(address))));
}
/* After the Wi-Fi service is resident (renoir_bluetooth_hci.inc). */
static void renoir_bluetooth_hci_start(void);
static void renoir_bluetooth_uart_probe(void)
{
    int enabled=1;
    PE_parse_boot_argn("renoirbt",&enabled,sizeof(enabled));
    if(!enabled)return;
    struct star_gpu_icc *core=NULL,*ddr=NULL;
    vm_offset_t uart=0,wrapper=0;unsigned clocks=0;int e,cleanup=0;
    const enum star_gpu_clock_id order[]={STAR_CLK_QUP0_M_AHB,STAR_CLK_QUP0_S_AHB,STAR_CLK_SE7};
    e=star_gpu_clock_qup0_open();if(e)goto done;
    e=star_gpu_icc_get_qup0(&core,&ddr);if(e)goto done;
    /* XNU ml_io_map is permanent (io_submap). These two probe-owned
     * windows are released below and must be allocated in kernel_map. */
    uart=ml_io_map_unmappable(0x99c000,0x4000,VM_WIMG_IO);
    wrapper=ml_io_map_unmappable(0x9c0000,0x2000,VM_WIMG_IO);
    if(!uart||!wrapper){e=ENOMEM;goto done;}
    e=star_gpu_icc_set(core,50000,100000);if(e)goto done;
    e=star_gpu_icc_set(ddr,0,76800);if(e)goto done;
    printf("STAR BT BUS: core=50000/100000 ddr=0/76800 voter=%d/%d\n",
        star_gpu_icc_consumer_error(core),star_gpu_icc_consumer_error(ddr));
    for(;clocks<3;clocks++){e=star_gpu_clock_prepare_enable(order[clocks]);if(e)goto done;}
    uint32_t fw=rd(uart,0x68),sfw=rd(uart,0x6c),hw=rd(wrapper,4);
    unsigned protocol=(fw>>8)&255;
    printf("STAR BT GENI: protocol=%u expected=2 m_fw=%u s_fw=%u hw=%08x tx_param=%08x rx_param=%08x status=%08x dma=%08x rate=%u\n",
        protocol,fw&255,sfw&255,hw,rd(uart,0xe24),rd(uart,0xe28),rd(uart,0x40),rd(uart,0x258),
        star_gpu_clock_get_rate(STAR_CLK_SE7));
    if(protocol!=2)e=ENXIO;
done:
    while(clocks)star_gpu_clock_disable_unprepare(order[--clocks]);
    if(core){cleanup=star_gpu_icc_set(core,0,0);star_gpu_icc_put(core);}
    if(ddr){int result=star_gpu_icc_set(ddr,0,0);if(result)cleanup=result;star_gpu_icc_put(ddr);}
    bt_unmap(uart,0x4000);bt_unmap(wrapper,0x2000);
    printf("STAR BT UART PROBE: error=%d cleanup=%d hci=0 dma_started=0; native version boundary\n",e,cleanup);
}

/* QCA6490 Bluetooth preflight from renoir-live.dts: hsuart0 SE7,
 * GPIO65 BT_EN, GPIO91 SW_CTRL, UART CTS/RTS/TX/RX68..71.
 * Read-only TLMM/GCC inventory precedes the resource-backed GENI probe.
 * No GPIO reset, UART FIFO, HCI or firmware writes; no iOS transport yet. */
static void
renoir_bluetooth_preflight(vm_offset_t gcc)
{
    static const unsigned pins[]={65,91,68,69,70,71};
    printf("STAR BT PROFILE: QCA6490 UART=0x99c000 CTS68 RTS69 TX70 RX71 EN65 SW91; read-only\n");
    bool mapped=true,enabled=false;
    for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);++i) {
        vm_offset_t gpio=ml_io_map(0x0f100000+pins[i]*0x1000,0x1000);
        if(!gpio) {mapped=false;continue;}
        uint32_t cfg=rd(gpio,0),io=rd(gpio,4);
        printf("STAR BT GPIO: pin=%u cfg=%#x io=%#x function=%u output=%u\n",pins[i],cfg,io,(cfg>>2)&15,(cfg>>9)&1);
        if(pins[i]==65) enabled=(cfg&(1u<<9)) && !(cfg&0x3c) && (io&2);
    }
    uint32_t branch=rd(gcc,0x1785c),source=rd(gcc,0x17860),votes=rd(gcc,0x52008);
    printf("STAR BT AHB: master=%#x slave=%#x\n",rd(gcc,0x17004),rd(gcc,0x17008));
    printf("STAR BT CLOCK: SE7 branch=%#x source=%#x votes=%#x halted=%u\n",branch,source,votes,branch>>31);
    printf("STAR BT SUMMARY: gpio_maps=%u enabled=%u se_clock=%u uart_untouched=1 hci=0; shared rails require managed ownership before power-up\n",mapped,enabled,!(branch>>31));
    renoir_bluetooth_uart_probe();
}
