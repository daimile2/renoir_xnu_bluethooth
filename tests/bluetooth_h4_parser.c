#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
static uint64_t mach_absolute_time(void){return 0;}
static uint8_t sk[1<<16];static unsigned sk_len,room=1<<16;static bool ring=true;
static uint32_t renoir_bt_sk_rx_put(const uint8_t *d,uint32_t n){if(!ring)return n;if(n>room-sk_len)n=room-sk_len;memcpy(sk+sk_len,d,n);sk_len+=n;return n;}
#include "parser.inc"
static void feed(const uint8_t *s,unsigned n,unsigned step){
  for(unsigned i=0;i<n;){unsigned k=n-i<step?n-i:step;unsigned u=0;while(u<k){unsigned c=bt_parse(s+i+u,k-u);u+=c;if(u<k){room+=7;}}i+=k;}
}
int main(void){
  /* service: event, ACL with IBS byte values in payload, IBS between frames, debug ACL, SCO, ISO */
  uint8_t s[]={0x04,0x0e,0x04,0x01,0x03,0x0c,0x00, 0xfd, 0x02,0x01,0x20,0x03,0x00,0xfd,0xfe,0xfc, 0xfe,
               0x02,0xdc,0x2e,0x02,0x00,0xaa,0xbb, 0x03,0x01,0x00,0x02,0x11,0x22, 0x05,0x01,0x00,0x01,0x00,0x99, 0xfc};
  uint8_t want[]={0x04,0x0e,0x04,0x01,0x03,0x0c,0x00, 0x02,0x01,0x20,0x03,0x00,0xfd,0xfe,0xfc, 0x03,0x01,0x00,0x02,0x11,0x22, 0x05,0x01,0x00,0x01,0x00,0x99};
  for(unsigned step=1;step<=sizeof s;++step){
    memset(&bw,0,sizeof bw);bw.service=true;bw.ibs=true;bw.tx_ibs=TX_WAKING;sk_len=0;room=step==3?5:1<<16;
    feed(s,sizeof s,step);
    assert(sk_len==sizeof want && !memcmp(sk,want,sizeof want));
    assert(bw.recv_wakes==1 && bw.recv_slps==1 && bw.recv_wacks==1 && bw.tx_ibs==TX_AWAKE && bw.diag==1 && bw.ctrl_len==1 && bw.ctrl[0]==0xfc);
  }
  /* setup: the waited command complete is consumed, an unrelated vendor event and ACL are not forwarded */
  uint8_t b[]={0x04,0xff,0x02,0x01,0x02, 0x02,0x01,0x00,0x01,0x00,0x55, 0x04,0x0e,0x05,0x01,0x00,0xfc,0x00,0x19};
  for(unsigned step=1;step<=sizeof b;++step){
    memset(&bw,0,sizeof bw);sk_len=0;room=1<<16;bw.waiting=true;bw.wait_opcode=0xfc00;
    feed(b,sizeof b,step);
    assert(bw.done && bw.ev_len==8 && bw.ev[6]==0 && bw.ev[7]==0x19 && bw.unrouted==1 && bw.setup_drops==1 && sk_len==0);
  }
  /* no ring: everything is taken (discarded) */
  memset(&bw,0,sizeof bw);bw.service=true;ring=false;sk_len=0;assert(bt_parse(s,sizeof s)==sizeof s);ring=true;
  /* unknown type drops the rest */
  memset(&bw,0,sizeof bw);bw.service=true;sk_len=0;uint8_t u[]={0x07,0x04,0x0e};assert(bt_parse(u,3)==3&&bw.bad==1&&!bw.need);
  /* geni_se_clk_freq_match over the SE7 table */
  static const uint32_t table[]={7372800,14745600,19200000,29491200,32000000,48000000,64000000,75000000,80000000,96000000,100000000};
  uint32_t bauds[]={115200,3200000,3000000};
  for(int k=0;k<3;++k){uint32_t desired=bauds[k]*16,clk=0,best=0;
    for(unsigned i=0;i<11;++i){uint32_t div=(table[i]+desired-1)/desired,delta=desired-table[i]/div;if(!best||delta<best){clk=table[i];if(!delta)break;best=delta;}}
    uint32_t div=(clk+desired-1)/desired;printf("%u -> %u Hz div %u = %u bps CLK_CFG %#x\n",bauds[k],clk,div,clk/div/16,1|(div<<4));}
  puts("parser tests passed");
}
