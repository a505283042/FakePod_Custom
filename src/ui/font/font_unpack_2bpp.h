#pragma once
#include <array>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace fakepod_font {
constexpr std::array<std::array<uint8_t,4>,256> make_alpha_lut() {
    std::array<std::array<uint8_t,4>,256> a{};
    for(unsigned b=0;b<256;++b) for(unsigned x=0;x<4;++x) a[b][x]=((b>>(6-2*x))&3U)*85U;
    return a;
}
inline constexpr auto alpha_lut = make_alpha_lut();
inline void unpack(const uint8_t *packed, uint8_t *out, uint32_t w, uint32_t h, uint32_t stride) {
    const size_t row_bytes=(size_t(w)+3U)/4U;
    for(uint32_t y=0;y<h;++y) {
        const uint8_t *src=packed+size_t(y)*row_bytes;
        uint8_t *dst=out+size_t(y)*stride;
        const uint32_t groups=w/4U;
        for(uint32_t g=0;g<groups;++g) memcpy(dst+g*4U, alpha_lut[src[g]].data(),4U);
        const uint32_t tail=w%4U;
        if(tail) memcpy(dst+groups*4U,alpha_lut[src[groups]].data(),tail);
        if(stride>w) memset(dst+w,0,stride-w);
    }
}
}
