#define NC_SLEEP_MS(x) ((void)0)
#include "../bridge/nocturn_core.c"
#include <stdio.h>
#include <assert.h>
static uint8_t usb[256]; static size_t ul; static int npk;
static int midi_cc[64], midi_v[64], midi_p[64], nm, last_mode=-1;
static void u(void*c,const uint8_t*d,size_t n){(void)c;memcpy(usb+ul,d,n);ul+=n;npk++; assert(n<=8);}
static void m(void*c,int p,uint8_t cc,uint8_t v){(void)c;midi_p[nm]=p;midi_cc[nm]=cc;midi_v[nm++]=v;}
static void mc(void*c,int p){(void)c;last_mode=p;}
static void reset(void){ul=0;npk=0;nm=0;}
int main(void){
  nc_state s; nc_config c={1,0,1,1}; nc_init(&s,&c,u,m,0);
  nc_send_init(&s);
  uint8_t exp[]={0xB0,0,0,0x28,0,0x2B,0x4A,0x2C,0,0x2E,0x35,0x2A,2,0x2C,0x72,0x2E,0x30};
  assert(ul==sizeof exp && !memcmp(usb,exp,ul) && npk==3); puts("init sequence OK");
  reset(); uint8_t a[]={0xB0,0x40,0x01}; nc_on_usb_data(&s,a,3,1000000000ULL);
  assert(nm==1&&midi_cc[0]==0x40&&midi_v[0]==65); assert(ul==2&&usb[0]==0x40&&usb[1]==65); puts("encoder right + ring LED OK");
  reset(); uint8_t b[]={0x41,0x7F}; nc_on_usb_data(&s,b,2,2000000000ULL); assert(midi_cc[0]==0x41&&midi_v[0]==63); puts("encoder left OK");
  reset(); uint8_t x[]={0x48,0x64,0x49,0x40}; nc_on_usb_data(&s,x,4,0); assert(nm==1&&midi_cc[0]==0x48&&midi_v[0]==100); puts("crossfader OK");
  reset(); uint8_t bt[]={0x70,0x7F,0x70,0x00}; nc_on_usb_data(&s,bt,4,0); assert(nm==2&&midi_v[0]==127&&midi_v[1]==0); puts("button OK");
  reset(); for(int i=0;i<8;i++) nc_write(&s,0x40+i,10+i); nc_flush(&s); assert(npk==2 && ul==16); puts("output batching (4 pairs per 8-byte packet) OK");
  reset(); nc_write(&s,0x40,10); nc_flush(&s); assert(ul==0); puts("output dedupe OK");
  reset(); s.cfg.absolute_encoders=0; uint8_t r[]={0x42,0x7E}; nc_on_usb_data(&s,r,2,5000000000ULL); assert(midi_v[0]==0x7E); puts("relative mode OK");
  /* routing */
  s.cfg.absolute_encoders=0; s.cfg.local_feedback=0; s.mode_changed=mc; nc_reset_cache(&s);
  reset(); nc_on_host_cc(&s,0,0x70,127); assert(ul==2); puts("main port LED reaches hw OK");
  reset(); nc_on_host_cc(&s,1,0x71,127); assert(ul==0); puts("inactive port LED shadowed OK");
  reset(); uint8_t sx[]={0xF0,0,0x20,0x29,0x7F,1,1,0xF7}; assert(nc_on_host_sysex(&s,0,sx,8)); assert(last_mode==1&&s.mode==1);
  { int ok71=0, off70=0; for(size_t i=0;i<ul;i+=2){ if(usb[i]==0x71&&usb[i+1]==127)ok71=1; if(usb[i]==0x70&&usb[i+1]==0)off70=1;} assert(ok71&&off70); }
  puts("mode switch repaints LEDs OK");
  reset(); uint8_t e[]={0x43,0x02}; nc_on_usb_data(&s,e,2,9000000000ULL); assert(nm==1&&midi_p[0]==1&&midi_v[0]==2); puts("controls routed to automap port OK");
  reset(); sx[6]=0; nc_on_host_sysex(&s,1,sx,8); { int ok70=0; for(size_t i=0;i<ul;i+=2) if(usb[i]==0x70&&usb[i+1]==127) ok70=1; assert(ok70 && last_mode==0);} puts("switch back restores main LEDs OK");
  puts("ALL TESTS PASSED"); return 0; }
