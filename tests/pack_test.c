// Checks the encoder against the byte-exact vectors captured from the genuine
// Windows driver while it was actually playing audio.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../plugin/sample_convert.h"
typedef float Float32;
#define A8DJ_CHANNELS 8
#define WIRE_STREAMS 4
#define WIRE_BLOCKS  2
static inline uint8_t CheckByte(int s,int b){return (uint8_t)((s<<1)|(~b&1));}
static inline int WireChannel(int s,int b){return s*2+(~b&1);}
static void Encode(uint8_t *fr,const Float32 *src){
    for(int blk=0;blk<WIRE_BLOCKS;blk++){
        uint8_t *b=fr+blk*16;
        for(int s=0;s<WIRE_STREAMS;s++) b[s]=CheckByte(s,blk);
        for(int s=0;s<WIRE_STREAMS;s++){
            int32_t v=A8DJ_FloatToS24(src[WireChannel(s,blk)]);
            b[4+s]=(uint8_t)(v>>16); b[8+s]=(uint8_t)(v>>8); b[12+s]=(uint8_t)v;
        }
    }
}
struct V { int32_t ch0, ch1; const char *hex; };
int main(void){
    struct V vs[]={
        {1253632,  578816,"0103050708000000d50000000000000000020406130000002100000000000000"},
        {1114880,  493568,"0103050707000000880000000000000000020406110000000300000000000000"},
        {-147200,  310528,"0103050704000000bd0000000000000000020406fd000000c100000000000000"},
    };
    int bad=0;
    for(unsigned i=0;i<sizeof(vs)/sizeof(vs[0]);i++){
        Float32 s[8]={0}; s[0]=vs[i].ch0/8388607.0f; s[1]=vs[i].ch1/8388607.0f;
        uint8_t out[32]; Encode(out,s);
        char got[65]; for(int k=0;k<32;k++) sprintf(got+k*2,"%02x",out[k]);
        int ok=!strcmp(got,vs[i].hex);
        printf("vec%u ch0=%8d ch1=%8d : %s\n",i,vs[i].ch0,vs[i].ch1,ok?"一致":"不一致");
        if(!ok){ printf("  期待 %s\n  実際 %s\n",vs[i].hex,got); bad++; }
    }
    printf(bad?"\n=> 符号化に誤りあり\n":"\n=> 符号化は純正と完全一致\n");
    return bad;
}
