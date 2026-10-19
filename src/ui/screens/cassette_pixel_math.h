#pragma once
#include <stdint.h>
#include <array>
namespace fakepod_cassette {
// Exact floor((first+i)*256/scale), using additions after initialization.
struct ScaleCursor {
    uint32_t value, remainder, step, step_remainder, divisor;
    ScaleCursor(uint32_t first, uint32_t scale):
        value((uint64_t(first)*256U)/scale), remainder((uint64_t(first)*256U)%scale),
        step(256U/scale), step_remainder(256U%scale), divisor(scale) {}
    void advance() {
        value += step; remainder += step_remainder;
        if (remainder >= divisor) { remainder -= divisor; ++value; }
    }
};
template<uint8_t Opacity> struct Dim565 {
    static constexpr std::array<uint16_t,32> rb_table() {
        std::array<uint16_t,32> a{};
        for (uint32_t i=0;i<32;++i) a[i]=(i*(255U-Opacity)+127U)/255U;
        return a;
    }
    static constexpr std::array<uint16_t,64> green_table() {
        std::array<uint16_t,64> a{};
        for (uint32_t i=0;i<64;++i) a[i]=((i*(255U-Opacity)+127U)/255U)<<5U;
        return a;
    }
    inline static constexpr auto rb=rb_table();
    inline static constexpr auto green=green_table();
    static uint16_t apply(uint16_t c) {
        return uint16_t((rb[c>>11U]<<11U)|green[(c>>5U)&63U]|rb[c&31U]);
    }
};
}
