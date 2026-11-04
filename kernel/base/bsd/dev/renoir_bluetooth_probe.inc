/* QCA6490 Bluetooth preflight from renoir-live.dts: hsuart0 SE7,
 * GPIO65 BT_EN, GPIO91 SW_CTRL, UART CTS/RTS/TX/RX68..71.
 * Read-only TLMM/GCC inventory. No GPIO/clock votes, UART FIFO reads,
 * reset/HCI/firmware writes or claims that an iOS transport is registered. */
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
}
