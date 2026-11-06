/* Synthetic command-db fixture ONLY. Addresses/units/rail levels are not
 * sampled hardware and must never become runtime defaults. */
#ifndef CEPHEUS_GPU_CONFIG_FIXTURE_H
#define CEPHEUS_GPU_CONFIG_FIXTURE_H
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t config_db[1024];
static uint32_t config_read(void *p,uint32_t off)
{assert(!(off&3) && off<=sizeof(config_db)-4);const uint8_t *d=p;return (uint32_t)d[off]|(uint32_t)d[off+1]<<8|(uint32_t)d[off+2]<<16|(uint32_t)d[off+3]<<24;}
static void config_word(unsigned off,uint32_t v)
{assert(off<=sizeof(config_db)-4);for(unsigned i=0;i<4;++i)config_db[off+i]=(uint8_t)(v>>(8*i));}
static void config_fixture(void)
{
    memset(config_db,0,sizeof(config_db));config_word(0,1);config_word(4,0x0c0330db);
    config_word(8,3);config_word(12,(5u<<16)|120);config_word(16,1);
    config_word(24,5|(256u<<16));config_word(28,(6u<<16)|400);config_word(32,1);
    const char *arc[]={"gfx.lvl","cx.lvl","mx.lvl","pdc.a","pdc.b"};
    const uint32_t address[]={0x30008,0x30000,0x30004,0x30010,0x30090};
    const uint16_t levels[]={0,16,48,64,128,192,224,256,320};
    for(unsigned i=0;i<5;++i) {
        unsigned e=144+i*24;memcpy(config_db+e,arc[i],strlen(arc[i]));
        config_word(e+16,address[i]);config_word(e+20,18|(i*20<<16));
        for(unsigned j=0;j<9;++j){config_db[264+i*20+2*j]=(uint8_t)levels[j];config_db[265+i*20+2*j]=(uint8_t)(levels[j]>>8);}
    }
    const char *bcm[]={"SH0","MC0","ACV","SH2","SN15","SN3"};
    for(unsigned i=0;i<6;++i) {
        unsigned e=400+i*24;memcpy(config_db+e,bcm[i],strlen(bcm[i]));
        config_word(e+16,0x50000+4*i);config_word(e+20,8|(i*8<<16));
        config_word(544+i*8,1000000);config_word(548+i*8,16|((i%3)<<16));
    }
}
#endif
