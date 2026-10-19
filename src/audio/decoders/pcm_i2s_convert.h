#pragma once
#include <stddef.h>
#include <stdint.h>
namespace fakepod_pcm {
template<unsigned Bytes> inline int32_t sample(const uint8_t *p){
    uint32_t raw=static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1])<<8U);
    if constexpr(Bytes>=3)raw|=static_cast<uint32_t>(p[2])<<16U;
    if constexpr(Bytes==4)raw|=static_cast<uint32_t>(p[3])<<24U;
    return static_cast<int32_t>(raw<<((4U-Bytes)*8U));
}
template<unsigned Bytes,bool Stereo> inline void convert(const uint8_t *src,int32_t *dst,size_t frames){
    for(size_t i=0;i<frames;++i){
        const int32_t left=sample<Bytes>(src);
        dst[2*i]=left;dst[2*i+1]=Stereo?sample<Bytes>(src+Bytes):left;
        src+=Bytes*(Stereo?2U:1U);
    }
}
inline void to_i2s32(const uint8_t *src,int32_t *dst,size_t frames,unsigned bits,unsigned channels){
    // Dispatch once per batch, with safe byte loads for packed/unaligned 24-bit PCM.
    if(bits==16){if(channels==2)convert<2,true>(src,dst,frames);else convert<2,false>(src,dst,frames);}
    else if(bits==24){if(channels==2)convert<3,true>(src,dst,frames);else convert<3,false>(src,dst,frames);}
    else {if(channels==2)convert<4,true>(src,dst,frames);else convert<4,false>(src,dst,frames);}
}
} // namespace fakepod_pcm
