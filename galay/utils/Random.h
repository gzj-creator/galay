#ifndef __GALAY_RANDOM_H__
#define __GALAY_RANDOM_H__

#include <cstdint>

namespace galay::utils
{
    class Randomizer
    {
    public:
        static int randomInt(int RandomMin, int RandomMax);
        static uint32_t randomUint32(uint32_t RandomMin, uint32_t RandomMax);
        static uint64_t randomUint64(uint64_t RandomMin, uint64_t RandomMax);
        static double randomDouble(double RandomMin, double RandomMax);
    };
}

#endif