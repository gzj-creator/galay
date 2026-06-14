#include "http2_hpack.h"
#include <algorithm>
#include <cstring>

namespace galay::http2
{

// ==================== Huffman 编码表 (RFC 7541 Appendix B) ====================

struct HuffmanCode {
    uint32_t code;
    uint8_t bits;
};

// RFC 7541 Appendix B - Huffman Codes
static const HuffmanCode kHuffmanCodes[257] = {
    {0x1ff8, 13}, {0x7fffd8, 23}, {0xfffffe2, 28}, {0xfffffe3, 28},
    {0xfffffe4, 28}, {0xfffffe5, 28}, {0xfffffe6, 28}, {0xfffffe7, 28},
    {0xfffffe8, 28}, {0xffffea, 24}, {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28},
    {0xfffffed, 28}, {0xfffffee, 28}, {0xfffffef, 28}, {0xffffff0, 28},
    {0xffffff1, 28}, {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28}, {0xffffff6, 28}, {0xffffff7, 28},
    {0xffffff8, 28}, {0xffffff9, 28}, {0xffffffa, 28}, {0xffffffb, 28},
    {0x14, 6}, {0x3f8, 10}, {0x3f9, 10}, {0xffa, 12},
    {0x1ff9, 13}, {0x15, 6}, {0xf8, 8}, {0x7fa, 11},
    {0x3fa, 10}, {0x3fb, 10}, {0xf9, 8}, {0x7fb, 11},
    {0xfa, 8}, {0x16, 6}, {0x17, 6}, {0x18, 6},
    {0x0, 5}, {0x1, 5}, {0x2, 5}, {0x19, 6},
    {0x1a, 6}, {0x1b, 6}, {0x1c, 6}, {0x1d, 6},
    {0x1e, 6}, {0x1f, 6}, {0x5c, 7}, {0xfb, 8},
    {0x7ffc, 15}, {0x20, 6}, {0xffb, 12}, {0x3fc, 10},
    {0x1ffa, 13}, {0x21, 6}, {0x5d, 7}, {0x5e, 7},
    {0x5f, 7}, {0x60, 7}, {0x61, 7}, {0x62, 7},
    {0x63, 7}, {0x64, 7}, {0x65, 7}, {0x66, 7},
    {0x67, 7}, {0x68, 7}, {0x69, 7}, {0x6a, 7},
    {0x6b, 7}, {0x6c, 7}, {0x6d, 7}, {0x6e, 7},
    {0x6f, 7}, {0x70, 7}, {0x71, 7}, {0x72, 7},
    {0xfc, 8}, {0x73, 7}, {0xfd, 8}, {0x1ffb, 13},
    {0x7fff0, 19}, {0x1ffc, 13}, {0x3ffc, 14}, {0x22, 6},
    {0x7ffd, 15}, {0x3, 5}, {0x23, 6}, {0x4, 5},
    {0x24, 6}, {0x5, 5}, {0x25, 6}, {0x26, 6},
    {0x27, 6}, {0x6, 5}, {0x74, 7}, {0x75, 7},
    {0x28, 6}, {0x29, 6}, {0x2a, 6}, {0x7, 5},
    {0x2b, 6}, {0x76, 7}, {0x2c, 6}, {0x8, 5},
    {0x9, 5}, {0x2d, 6}, {0x77, 7}, {0x78, 7},
    {0x79, 7}, {0x7a, 7}, {0x7b, 7}, {0x7ffe, 15},
    {0x7fc, 11}, {0x3ffd, 14}, {0x1ffd, 13}, {0xffffffc, 28},
    {0xfffe6, 20}, {0x3fffd2, 22}, {0xfffe7, 20}, {0xfffe8, 20},
    {0x3fffd3, 22}, {0x3fffd4, 22}, {0x3fffd5, 22}, {0x7fffd9, 23},
    {0x3fffd6, 22}, {0x7fffda, 23}, {0x7fffdb, 23}, {0x7fffdc, 23},
    {0x7fffdd, 23}, {0x7fffde, 23}, {0xffffeb, 24}, {0x7fffdf, 23},
    {0xffffec, 24}, {0xffffed, 24}, {0x3fffd7, 22}, {0x7fffe0, 23},
    {0xffffee, 24}, {0x7fffe1, 23}, {0x7fffe2, 23}, {0x7fffe3, 23},
    {0x7fffe4, 23}, {0x1fffdc, 21}, {0x3fffd8, 22}, {0x7fffe5, 23},
    {0x3fffd9, 22}, {0x7fffe6, 23}, {0x7fffe7, 23}, {0xffffef, 24},
    {0x3fffda, 22}, {0x1fffdd, 21}, {0xfffe9, 20}, {0x3fffdb, 22},
    {0x3fffdc, 22}, {0x7fffe8, 23}, {0x7fffe9, 23}, {0x1fffde, 21},
    {0x7fffea, 23}, {0x3fffdd, 22}, {0x3fffde, 22}, {0xfffff0, 24},
    {0x1fffdf, 21}, {0x3fffdf, 22}, {0x7fffeb, 23}, {0x7fffec, 23},
    {0x1fffe0, 21}, {0x1fffe1, 21}, {0x3fffe0, 22}, {0x1fffe2, 21},
    {0x7fffed, 23}, {0x3fffe1, 22}, {0x7fffee, 23}, {0x7fffef, 23},
    {0xfffea, 20}, {0x3fffe2, 22}, {0x3fffe3, 22}, {0x3fffe4, 22},
    {0x7ffff0, 23}, {0x3fffe5, 22}, {0x3fffe6, 22}, {0x7ffff1, 23},
    {0x3ffffe0, 26}, {0x3ffffe1, 26}, {0xfffeb, 20}, {0x7fff1, 19},
    {0x3fffe7, 22}, {0x7ffff2, 23}, {0x3fffe8, 22}, {0x1ffffec, 25},
    {0x3ffffe2, 26}, {0x3ffffe3, 26}, {0x3ffffe4, 26}, {0x7ffffde, 27},
    {0x7ffffdf, 27}, {0x3ffffe5, 26}, {0xfffff1, 24}, {0x1ffffed, 25},
    {0x7fff2, 19}, {0x1fffe3, 21}, {0x3ffffe6, 26}, {0x7ffffe0, 27},
    {0x7ffffe1, 27}, {0x3ffffe7, 26}, {0x7ffffe2, 27}, {0xfffff2, 24},
    {0x1fffe4, 21}, {0x1fffe5, 21}, {0x3ffffe8, 26}, {0x3ffffe9, 26},
    {0xffffffd, 28}, {0x7ffffe3, 27}, {0x7ffffe4, 27}, {0x7ffffe5, 27},
    {0xfffec, 20}, {0xfffff3, 24}, {0xfffed, 20}, {0x1fffe6, 21},
    {0x3fffe9, 22}, {0x1fffe7, 21}, {0x1fffe8, 21}, {0x7ffff3, 23},
    {0x3fffea, 22}, {0x3fffeb, 22}, {0x1ffffee, 25}, {0x1ffffef, 25},
    {0xfffff4, 24}, {0xfffff5, 24}, {0x3ffffea, 26}, {0x7ffff4, 23},
    {0x3ffffeb, 26}, {0x7ffffe6, 27}, {0x3ffffec, 26}, {0x3ffffed, 26},
    {0x7ffffe7, 27}, {0x7ffffe8, 27}, {0x7ffffe9, 27}, {0x7ffffea, 27},
    {0x7ffffeb, 27}, {0xffffffe, 28}, {0x7ffffec, 27}, {0x7ffffed, 27},
    {0x7ffffee, 27}, {0x7ffffef, 27}, {0x7fffff0, 27}, {0x3ffffee, 26},
    {0x3fffffff, 30}  // EOS
};

// ==================== Huffman 4-bit 解码状态机 ====================

enum HuffmanDecodeFlag : uint8_t {
    HUFFMAN_NONE = 0x00,
    HUFFMAN_EMIT = 0x01,  // 输出一个符号
    HUFFMAN_FAIL = 0x04,  // 非法序列
};

struct HuffmanDecodeEntry {
    uint8_t state;
    uint8_t flags;
    uint8_t symbol;
};

// clang-format off
static const HuffmanDecodeEntry kHuffmanDecodeTable[256][16] = {
    /* state 0 */ {
        {  1, HUFFMAN_NONE,   0},
        {  2, HUFFMAN_NONE,   0},
        {  3, HUFFMAN_NONE,   0},
        {  4, HUFFMAN_NONE,   0},
        {  5, HUFFMAN_NONE,   0},
        {  6, HUFFMAN_NONE,   0},
        {  7, HUFFMAN_NONE,   0},
        {  8, HUFFMAN_NONE,   0},
        {  9, HUFFMAN_NONE,   0},
        { 10, HUFFMAN_NONE,   0},
        { 11, HUFFMAN_NONE,   0},
        { 12, HUFFMAN_NONE,   0},
        { 13, HUFFMAN_NONE,   0},
        { 14, HUFFMAN_NONE,   0},
        { 15, HUFFMAN_NONE,   0},
        { 16, HUFFMAN_NONE,   0}
    },
    /* state 1 */ {
        { 17, HUFFMAN_EMIT,  48},
        { 18, HUFFMAN_EMIT,  48},
        { 19, HUFFMAN_EMIT,  48},
        { 20, HUFFMAN_EMIT,  48},
        { 21, HUFFMAN_EMIT,  48},
        { 22, HUFFMAN_EMIT,  48},
        { 23, HUFFMAN_EMIT,  48},
        { 24, HUFFMAN_EMIT,  48},
        { 17, HUFFMAN_EMIT,  49},
        { 18, HUFFMAN_EMIT,  49},
        { 19, HUFFMAN_EMIT,  49},
        { 20, HUFFMAN_EMIT,  49},
        { 21, HUFFMAN_EMIT,  49},
        { 22, HUFFMAN_EMIT,  49},
        { 23, HUFFMAN_EMIT,  49},
        { 24, HUFFMAN_EMIT,  49}
    },
    /* state 2 */ {
        { 17, HUFFMAN_EMIT,  50},
        { 18, HUFFMAN_EMIT,  50},
        { 19, HUFFMAN_EMIT,  50},
        { 20, HUFFMAN_EMIT,  50},
        { 21, HUFFMAN_EMIT,  50},
        { 22, HUFFMAN_EMIT,  50},
        { 23, HUFFMAN_EMIT,  50},
        { 24, HUFFMAN_EMIT,  50},
        { 17, HUFFMAN_EMIT,  97},
        { 18, HUFFMAN_EMIT,  97},
        { 19, HUFFMAN_EMIT,  97},
        { 20, HUFFMAN_EMIT,  97},
        { 21, HUFFMAN_EMIT,  97},
        { 22, HUFFMAN_EMIT,  97},
        { 23, HUFFMAN_EMIT,  97},
        { 24, HUFFMAN_EMIT,  97}
    },
    /* state 3 */ {
        { 17, HUFFMAN_EMIT,  99},
        { 18, HUFFMAN_EMIT,  99},
        { 19, HUFFMAN_EMIT,  99},
        { 20, HUFFMAN_EMIT,  99},
        { 21, HUFFMAN_EMIT,  99},
        { 22, HUFFMAN_EMIT,  99},
        { 23, HUFFMAN_EMIT,  99},
        { 24, HUFFMAN_EMIT,  99},
        { 17, HUFFMAN_EMIT, 101},
        { 18, HUFFMAN_EMIT, 101},
        { 19, HUFFMAN_EMIT, 101},
        { 20, HUFFMAN_EMIT, 101},
        { 21, HUFFMAN_EMIT, 101},
        { 22, HUFFMAN_EMIT, 101},
        { 23, HUFFMAN_EMIT, 101},
        { 24, HUFFMAN_EMIT, 101}
    },
    /* state 4 */ {
        { 17, HUFFMAN_EMIT, 105},
        { 18, HUFFMAN_EMIT, 105},
        { 19, HUFFMAN_EMIT, 105},
        { 20, HUFFMAN_EMIT, 105},
        { 21, HUFFMAN_EMIT, 105},
        { 22, HUFFMAN_EMIT, 105},
        { 23, HUFFMAN_EMIT, 105},
        { 24, HUFFMAN_EMIT, 105},
        { 17, HUFFMAN_EMIT, 111},
        { 18, HUFFMAN_EMIT, 111},
        { 19, HUFFMAN_EMIT, 111},
        { 20, HUFFMAN_EMIT, 111},
        { 21, HUFFMAN_EMIT, 111},
        { 22, HUFFMAN_EMIT, 111},
        { 23, HUFFMAN_EMIT, 111},
        { 24, HUFFMAN_EMIT, 111}
    },
    /* state 5 */ {
        { 17, HUFFMAN_EMIT, 115},
        { 18, HUFFMAN_EMIT, 115},
        { 19, HUFFMAN_EMIT, 115},
        { 20, HUFFMAN_EMIT, 115},
        { 21, HUFFMAN_EMIT, 115},
        { 22, HUFFMAN_EMIT, 115},
        { 23, HUFFMAN_EMIT, 115},
        { 24, HUFFMAN_EMIT, 115},
        { 17, HUFFMAN_EMIT, 116},
        { 18, HUFFMAN_EMIT, 116},
        { 19, HUFFMAN_EMIT, 116},
        { 20, HUFFMAN_EMIT, 116},
        { 21, HUFFMAN_EMIT, 116},
        { 22, HUFFMAN_EMIT, 116},
        { 23, HUFFMAN_EMIT, 116},
        { 24, HUFFMAN_EMIT, 116}
    },
    /* state 6 */ {
        { 25, HUFFMAN_EMIT,  32},
        { 26, HUFFMAN_EMIT,  32},
        { 27, HUFFMAN_EMIT,  32},
        { 28, HUFFMAN_EMIT,  32},
        { 25, HUFFMAN_EMIT,  37},
        { 26, HUFFMAN_EMIT,  37},
        { 27, HUFFMAN_EMIT,  37},
        { 28, HUFFMAN_EMIT,  37},
        { 25, HUFFMAN_EMIT,  45},
        { 26, HUFFMAN_EMIT,  45},
        { 27, HUFFMAN_EMIT,  45},
        { 28, HUFFMAN_EMIT,  45},
        { 25, HUFFMAN_EMIT,  46},
        { 26, HUFFMAN_EMIT,  46},
        { 27, HUFFMAN_EMIT,  46},
        { 28, HUFFMAN_EMIT,  46}
    },
    /* state 7 */ {
        { 25, HUFFMAN_EMIT,  47},
        { 26, HUFFMAN_EMIT,  47},
        { 27, HUFFMAN_EMIT,  47},
        { 28, HUFFMAN_EMIT,  47},
        { 25, HUFFMAN_EMIT,  51},
        { 26, HUFFMAN_EMIT,  51},
        { 27, HUFFMAN_EMIT,  51},
        { 28, HUFFMAN_EMIT,  51},
        { 25, HUFFMAN_EMIT,  52},
        { 26, HUFFMAN_EMIT,  52},
        { 27, HUFFMAN_EMIT,  52},
        { 28, HUFFMAN_EMIT,  52},
        { 25, HUFFMAN_EMIT,  53},
        { 26, HUFFMAN_EMIT,  53},
        { 27, HUFFMAN_EMIT,  53},
        { 28, HUFFMAN_EMIT,  53}
    },
    /* state 8 */ {
        { 25, HUFFMAN_EMIT,  54},
        { 26, HUFFMAN_EMIT,  54},
        { 27, HUFFMAN_EMIT,  54},
        { 28, HUFFMAN_EMIT,  54},
        { 25, HUFFMAN_EMIT,  55},
        { 26, HUFFMAN_EMIT,  55},
        { 27, HUFFMAN_EMIT,  55},
        { 28, HUFFMAN_EMIT,  55},
        { 25, HUFFMAN_EMIT,  56},
        { 26, HUFFMAN_EMIT,  56},
        { 27, HUFFMAN_EMIT,  56},
        { 28, HUFFMAN_EMIT,  56},
        { 25, HUFFMAN_EMIT,  57},
        { 26, HUFFMAN_EMIT,  57},
        { 27, HUFFMAN_EMIT,  57},
        { 28, HUFFMAN_EMIT,  57}
    },
    /* state 9 */ {
        { 25, HUFFMAN_EMIT,  61},
        { 26, HUFFMAN_EMIT,  61},
        { 27, HUFFMAN_EMIT,  61},
        { 28, HUFFMAN_EMIT,  61},
        { 25, HUFFMAN_EMIT,  65},
        { 26, HUFFMAN_EMIT,  65},
        { 27, HUFFMAN_EMIT,  65},
        { 28, HUFFMAN_EMIT,  65},
        { 25, HUFFMAN_EMIT,  95},
        { 26, HUFFMAN_EMIT,  95},
        { 27, HUFFMAN_EMIT,  95},
        { 28, HUFFMAN_EMIT,  95},
        { 25, HUFFMAN_EMIT,  98},
        { 26, HUFFMAN_EMIT,  98},
        { 27, HUFFMAN_EMIT,  98},
        { 28, HUFFMAN_EMIT,  98}
    },
    /* state 10 */ {
        { 25, HUFFMAN_EMIT, 100},
        { 26, HUFFMAN_EMIT, 100},
        { 27, HUFFMAN_EMIT, 100},
        { 28, HUFFMAN_EMIT, 100},
        { 25, HUFFMAN_EMIT, 102},
        { 26, HUFFMAN_EMIT, 102},
        { 27, HUFFMAN_EMIT, 102},
        { 28, HUFFMAN_EMIT, 102},
        { 25, HUFFMAN_EMIT, 103},
        { 26, HUFFMAN_EMIT, 103},
        { 27, HUFFMAN_EMIT, 103},
        { 28, HUFFMAN_EMIT, 103},
        { 25, HUFFMAN_EMIT, 104},
        { 26, HUFFMAN_EMIT, 104},
        { 27, HUFFMAN_EMIT, 104},
        { 28, HUFFMAN_EMIT, 104}
    },
    /* state 11 */ {
        { 25, HUFFMAN_EMIT, 108},
        { 26, HUFFMAN_EMIT, 108},
        { 27, HUFFMAN_EMIT, 108},
        { 28, HUFFMAN_EMIT, 108},
        { 25, HUFFMAN_EMIT, 109},
        { 26, HUFFMAN_EMIT, 109},
        { 27, HUFFMAN_EMIT, 109},
        { 28, HUFFMAN_EMIT, 109},
        { 25, HUFFMAN_EMIT, 110},
        { 26, HUFFMAN_EMIT, 110},
        { 27, HUFFMAN_EMIT, 110},
        { 28, HUFFMAN_EMIT, 110},
        { 25, HUFFMAN_EMIT, 112},
        { 26, HUFFMAN_EMIT, 112},
        { 27, HUFFMAN_EMIT, 112},
        { 28, HUFFMAN_EMIT, 112}
    },
    /* state 12 */ {
        { 25, HUFFMAN_EMIT, 114},
        { 26, HUFFMAN_EMIT, 114},
        { 27, HUFFMAN_EMIT, 114},
        { 28, HUFFMAN_EMIT, 114},
        { 25, HUFFMAN_EMIT, 117},
        { 26, HUFFMAN_EMIT, 117},
        { 27, HUFFMAN_EMIT, 117},
        { 28, HUFFMAN_EMIT, 117},
        { 29, HUFFMAN_EMIT,  58},
        { 30, HUFFMAN_EMIT,  58},
        { 29, HUFFMAN_EMIT,  66},
        { 30, HUFFMAN_EMIT,  66},
        { 29, HUFFMAN_EMIT,  67},
        { 30, HUFFMAN_EMIT,  67},
        { 29, HUFFMAN_EMIT,  68},
        { 30, HUFFMAN_EMIT,  68}
    },
    /* state 13 */ {
        { 29, HUFFMAN_EMIT,  69},
        { 30, HUFFMAN_EMIT,  69},
        { 29, HUFFMAN_EMIT,  70},
        { 30, HUFFMAN_EMIT,  70},
        { 29, HUFFMAN_EMIT,  71},
        { 30, HUFFMAN_EMIT,  71},
        { 29, HUFFMAN_EMIT,  72},
        { 30, HUFFMAN_EMIT,  72},
        { 29, HUFFMAN_EMIT,  73},
        { 30, HUFFMAN_EMIT,  73},
        { 29, HUFFMAN_EMIT,  74},
        { 30, HUFFMAN_EMIT,  74},
        { 29, HUFFMAN_EMIT,  75},
        { 30, HUFFMAN_EMIT,  75},
        { 29, HUFFMAN_EMIT,  76},
        { 30, HUFFMAN_EMIT,  76}
    },
    /* state 14 */ {
        { 29, HUFFMAN_EMIT,  77},
        { 30, HUFFMAN_EMIT,  77},
        { 29, HUFFMAN_EMIT,  78},
        { 30, HUFFMAN_EMIT,  78},
        { 29, HUFFMAN_EMIT,  79},
        { 30, HUFFMAN_EMIT,  79},
        { 29, HUFFMAN_EMIT,  80},
        { 30, HUFFMAN_EMIT,  80},
        { 29, HUFFMAN_EMIT,  81},
        { 30, HUFFMAN_EMIT,  81},
        { 29, HUFFMAN_EMIT,  82},
        { 30, HUFFMAN_EMIT,  82},
        { 29, HUFFMAN_EMIT,  83},
        { 30, HUFFMAN_EMIT,  83},
        { 29, HUFFMAN_EMIT,  84},
        { 30, HUFFMAN_EMIT,  84}
    },
    /* state 15 */ {
        { 29, HUFFMAN_EMIT,  85},
        { 30, HUFFMAN_EMIT,  85},
        { 29, HUFFMAN_EMIT,  86},
        { 30, HUFFMAN_EMIT,  86},
        { 29, HUFFMAN_EMIT,  87},
        { 30, HUFFMAN_EMIT,  87},
        { 29, HUFFMAN_EMIT,  89},
        { 30, HUFFMAN_EMIT,  89},
        { 29, HUFFMAN_EMIT, 106},
        { 30, HUFFMAN_EMIT, 106},
        { 29, HUFFMAN_EMIT, 107},
        { 30, HUFFMAN_EMIT, 107},
        { 29, HUFFMAN_EMIT, 113},
        { 30, HUFFMAN_EMIT, 113},
        { 29, HUFFMAN_EMIT, 118},
        { 30, HUFFMAN_EMIT, 118}
    },
    /* state 16 */ {
        { 29, HUFFMAN_EMIT, 119},
        { 30, HUFFMAN_EMIT, 119},
        { 29, HUFFMAN_EMIT, 120},
        { 30, HUFFMAN_EMIT, 120},
        { 29, HUFFMAN_EMIT, 121},
        { 30, HUFFMAN_EMIT, 121},
        { 29, HUFFMAN_EMIT, 122},
        { 30, HUFFMAN_EMIT, 122},
        {  0, HUFFMAN_EMIT,  38},
        {  0, HUFFMAN_EMIT,  42},
        {  0, HUFFMAN_EMIT,  44},
        {  0, HUFFMAN_EMIT,  59},
        {  0, HUFFMAN_EMIT,  88},
        {  0, HUFFMAN_EMIT,  90},
        { 31, HUFFMAN_NONE,   0},
        { 32, HUFFMAN_NONE,   0}
    },
    /* state 17 */ {
        { 25, HUFFMAN_EMIT,  48},
        { 26, HUFFMAN_EMIT,  48},
        { 27, HUFFMAN_EMIT,  48},
        { 28, HUFFMAN_EMIT,  48},
        { 25, HUFFMAN_EMIT,  49},
        { 26, HUFFMAN_EMIT,  49},
        { 27, HUFFMAN_EMIT,  49},
        { 28, HUFFMAN_EMIT,  49},
        { 25, HUFFMAN_EMIT,  50},
        { 26, HUFFMAN_EMIT,  50},
        { 27, HUFFMAN_EMIT,  50},
        { 28, HUFFMAN_EMIT,  50},
        { 25, HUFFMAN_EMIT,  97},
        { 26, HUFFMAN_EMIT,  97},
        { 27, HUFFMAN_EMIT,  97},
        { 28, HUFFMAN_EMIT,  97}
    },
    /* state 18 */ {
        { 25, HUFFMAN_EMIT,  99},
        { 26, HUFFMAN_EMIT,  99},
        { 27, HUFFMAN_EMIT,  99},
        { 28, HUFFMAN_EMIT,  99},
        { 25, HUFFMAN_EMIT, 101},
        { 26, HUFFMAN_EMIT, 101},
        { 27, HUFFMAN_EMIT, 101},
        { 28, HUFFMAN_EMIT, 101},
        { 25, HUFFMAN_EMIT, 105},
        { 26, HUFFMAN_EMIT, 105},
        { 27, HUFFMAN_EMIT, 105},
        { 28, HUFFMAN_EMIT, 105},
        { 25, HUFFMAN_EMIT, 111},
        { 26, HUFFMAN_EMIT, 111},
        { 27, HUFFMAN_EMIT, 111},
        { 28, HUFFMAN_EMIT, 111}
    },
    /* state 19 */ {
        { 25, HUFFMAN_EMIT, 115},
        { 26, HUFFMAN_EMIT, 115},
        { 27, HUFFMAN_EMIT, 115},
        { 28, HUFFMAN_EMIT, 115},
        { 25, HUFFMAN_EMIT, 116},
        { 26, HUFFMAN_EMIT, 116},
        { 27, HUFFMAN_EMIT, 116},
        { 28, HUFFMAN_EMIT, 116},
        { 29, HUFFMAN_EMIT,  32},
        { 30, HUFFMAN_EMIT,  32},
        { 29, HUFFMAN_EMIT,  37},
        { 30, HUFFMAN_EMIT,  37},
        { 29, HUFFMAN_EMIT,  45},
        { 30, HUFFMAN_EMIT,  45},
        { 29, HUFFMAN_EMIT,  46},
        { 30, HUFFMAN_EMIT,  46}
    },
    /* state 20 */ {
        { 29, HUFFMAN_EMIT,  47},
        { 30, HUFFMAN_EMIT,  47},
        { 29, HUFFMAN_EMIT,  51},
        { 30, HUFFMAN_EMIT,  51},
        { 29, HUFFMAN_EMIT,  52},
        { 30, HUFFMAN_EMIT,  52},
        { 29, HUFFMAN_EMIT,  53},
        { 30, HUFFMAN_EMIT,  53},
        { 29, HUFFMAN_EMIT,  54},
        { 30, HUFFMAN_EMIT,  54},
        { 29, HUFFMAN_EMIT,  55},
        { 30, HUFFMAN_EMIT,  55},
        { 29, HUFFMAN_EMIT,  56},
        { 30, HUFFMAN_EMIT,  56},
        { 29, HUFFMAN_EMIT,  57},
        { 30, HUFFMAN_EMIT,  57}
    },
    /* state 21 */ {
        { 29, HUFFMAN_EMIT,  61},
        { 30, HUFFMAN_EMIT,  61},
        { 29, HUFFMAN_EMIT,  65},
        { 30, HUFFMAN_EMIT,  65},
        { 29, HUFFMAN_EMIT,  95},
        { 30, HUFFMAN_EMIT,  95},
        { 29, HUFFMAN_EMIT,  98},
        { 30, HUFFMAN_EMIT,  98},
        { 29, HUFFMAN_EMIT, 100},
        { 30, HUFFMAN_EMIT, 100},
        { 29, HUFFMAN_EMIT, 102},
        { 30, HUFFMAN_EMIT, 102},
        { 29, HUFFMAN_EMIT, 103},
        { 30, HUFFMAN_EMIT, 103},
        { 29, HUFFMAN_EMIT, 104},
        { 30, HUFFMAN_EMIT, 104}
    },
    /* state 22 */ {
        { 29, HUFFMAN_EMIT, 108},
        { 30, HUFFMAN_EMIT, 108},
        { 29, HUFFMAN_EMIT, 109},
        { 30, HUFFMAN_EMIT, 109},
        { 29, HUFFMAN_EMIT, 110},
        { 30, HUFFMAN_EMIT, 110},
        { 29, HUFFMAN_EMIT, 112},
        { 30, HUFFMAN_EMIT, 112},
        { 29, HUFFMAN_EMIT, 114},
        { 30, HUFFMAN_EMIT, 114},
        { 29, HUFFMAN_EMIT, 117},
        { 30, HUFFMAN_EMIT, 117},
        {  0, HUFFMAN_EMIT,  58},
        {  0, HUFFMAN_EMIT,  66},
        {  0, HUFFMAN_EMIT,  67},
        {  0, HUFFMAN_EMIT,  68}
    },
    /* state 23 */ {
        {  0, HUFFMAN_EMIT,  69},
        {  0, HUFFMAN_EMIT,  70},
        {  0, HUFFMAN_EMIT,  71},
        {  0, HUFFMAN_EMIT,  72},
        {  0, HUFFMAN_EMIT,  73},
        {  0, HUFFMAN_EMIT,  74},
        {  0, HUFFMAN_EMIT,  75},
        {  0, HUFFMAN_EMIT,  76},
        {  0, HUFFMAN_EMIT,  77},
        {  0, HUFFMAN_EMIT,  78},
        {  0, HUFFMAN_EMIT,  79},
        {  0, HUFFMAN_EMIT,  80},
        {  0, HUFFMAN_EMIT,  81},
        {  0, HUFFMAN_EMIT,  82},
        {  0, HUFFMAN_EMIT,  83},
        {  0, HUFFMAN_EMIT,  84}
    },
    /* state 24 */ {
        {  0, HUFFMAN_EMIT,  85},
        {  0, HUFFMAN_EMIT,  86},
        {  0, HUFFMAN_EMIT,  87},
        {  0, HUFFMAN_EMIT,  89},
        {  0, HUFFMAN_EMIT, 106},
        {  0, HUFFMAN_EMIT, 107},
        {  0, HUFFMAN_EMIT, 113},
        {  0, HUFFMAN_EMIT, 118},
        {  0, HUFFMAN_EMIT, 119},
        {  0, HUFFMAN_EMIT, 120},
        {  0, HUFFMAN_EMIT, 121},
        {  0, HUFFMAN_EMIT, 122},
        { 33, HUFFMAN_NONE,   0},
        { 34, HUFFMAN_NONE,   0},
        { 35, HUFFMAN_NONE,   0},
        { 36, HUFFMAN_NONE,   0}
    },
    /* state 25 */ {
        { 29, HUFFMAN_EMIT,  48},
        { 30, HUFFMAN_EMIT,  48},
        { 29, HUFFMAN_EMIT,  49},
        { 30, HUFFMAN_EMIT,  49},
        { 29, HUFFMAN_EMIT,  50},
        { 30, HUFFMAN_EMIT,  50},
        { 29, HUFFMAN_EMIT,  97},
        { 30, HUFFMAN_EMIT,  97},
        { 29, HUFFMAN_EMIT,  99},
        { 30, HUFFMAN_EMIT,  99},
        { 29, HUFFMAN_EMIT, 101},
        { 30, HUFFMAN_EMIT, 101},
        { 29, HUFFMAN_EMIT, 105},
        { 30, HUFFMAN_EMIT, 105},
        { 29, HUFFMAN_EMIT, 111},
        { 30, HUFFMAN_EMIT, 111}
    },
    /* state 26 */ {
        { 29, HUFFMAN_EMIT, 115},
        { 30, HUFFMAN_EMIT, 115},
        { 29, HUFFMAN_EMIT, 116},
        { 30, HUFFMAN_EMIT, 116},
        {  0, HUFFMAN_EMIT,  32},
        {  0, HUFFMAN_EMIT,  37},
        {  0, HUFFMAN_EMIT,  45},
        {  0, HUFFMAN_EMIT,  46},
        {  0, HUFFMAN_EMIT,  47},
        {  0, HUFFMAN_EMIT,  51},
        {  0, HUFFMAN_EMIT,  52},
        {  0, HUFFMAN_EMIT,  53},
        {  0, HUFFMAN_EMIT,  54},
        {  0, HUFFMAN_EMIT,  55},
        {  0, HUFFMAN_EMIT,  56},
        {  0, HUFFMAN_EMIT,  57}
    },
    /* state 27 */ {
        {  0, HUFFMAN_EMIT,  61},
        {  0, HUFFMAN_EMIT,  65},
        {  0, HUFFMAN_EMIT,  95},
        {  0, HUFFMAN_EMIT,  98},
        {  0, HUFFMAN_EMIT, 100},
        {  0, HUFFMAN_EMIT, 102},
        {  0, HUFFMAN_EMIT, 103},
        {  0, HUFFMAN_EMIT, 104},
        {  0, HUFFMAN_EMIT, 108},
        {  0, HUFFMAN_EMIT, 109},
        {  0, HUFFMAN_EMIT, 110},
        {  0, HUFFMAN_EMIT, 112},
        {  0, HUFFMAN_EMIT, 114},
        {  0, HUFFMAN_EMIT, 117},
        { 37, HUFFMAN_NONE,   0},
        { 38, HUFFMAN_NONE,   0}
    },
    /* state 28 */ {
        { 39, HUFFMAN_NONE,   0},
        { 40, HUFFMAN_NONE,   0},
        { 41, HUFFMAN_NONE,   0},
        { 42, HUFFMAN_NONE,   0},
        { 43, HUFFMAN_NONE,   0},
        { 44, HUFFMAN_NONE,   0},
        { 45, HUFFMAN_NONE,   0},
        { 46, HUFFMAN_NONE,   0},
        { 47, HUFFMAN_NONE,   0},
        { 48, HUFFMAN_NONE,   0},
        { 49, HUFFMAN_NONE,   0},
        { 50, HUFFMAN_NONE,   0},
        { 51, HUFFMAN_NONE,   0},
        { 52, HUFFMAN_NONE,   0},
        { 53, HUFFMAN_NONE,   0},
        { 54, HUFFMAN_NONE,   0}
    },
    /* state 29 */ {
        {  0, HUFFMAN_EMIT,  48},
        {  0, HUFFMAN_EMIT,  49},
        {  0, HUFFMAN_EMIT,  50},
        {  0, HUFFMAN_EMIT,  97},
        {  0, HUFFMAN_EMIT,  99},
        {  0, HUFFMAN_EMIT, 101},
        {  0, HUFFMAN_EMIT, 105},
        {  0, HUFFMAN_EMIT, 111},
        {  0, HUFFMAN_EMIT, 115},
        {  0, HUFFMAN_EMIT, 116},
        { 55, HUFFMAN_NONE,   0},
        { 56, HUFFMAN_NONE,   0},
        { 57, HUFFMAN_NONE,   0},
        { 58, HUFFMAN_NONE,   0},
        { 59, HUFFMAN_NONE,   0},
        { 60, HUFFMAN_NONE,   0}
    },
    /* state 30 */ {
        { 61, HUFFMAN_NONE,   0},
        { 62, HUFFMAN_NONE,   0},
        { 63, HUFFMAN_NONE,   0},
        { 64, HUFFMAN_NONE,   0},
        { 65, HUFFMAN_NONE,   0},
        { 66, HUFFMAN_NONE,   0},
        { 67, HUFFMAN_NONE,   0},
        { 68, HUFFMAN_NONE,   0},
        { 69, HUFFMAN_NONE,   0},
        { 70, HUFFMAN_NONE,   0},
        { 71, HUFFMAN_NONE,   0},
        { 72, HUFFMAN_NONE,   0},
        { 73, HUFFMAN_NONE,   0},
        { 74, HUFFMAN_NONE,   0},
        { 75, HUFFMAN_NONE,   0},
        { 76, HUFFMAN_NONE,   0}
    },
    /* state 31 */ {
        { 25, HUFFMAN_EMIT,  33},
        { 26, HUFFMAN_EMIT,  33},
        { 27, HUFFMAN_EMIT,  33},
        { 28, HUFFMAN_EMIT,  33},
        { 25, HUFFMAN_EMIT,  34},
        { 26, HUFFMAN_EMIT,  34},
        { 27, HUFFMAN_EMIT,  34},
        { 28, HUFFMAN_EMIT,  34},
        { 25, HUFFMAN_EMIT,  40},
        { 26, HUFFMAN_EMIT,  40},
        { 27, HUFFMAN_EMIT,  40},
        { 28, HUFFMAN_EMIT,  40},
        { 25, HUFFMAN_EMIT,  41},
        { 26, HUFFMAN_EMIT,  41},
        { 27, HUFFMAN_EMIT,  41},
        { 28, HUFFMAN_EMIT,  41}
    },
    /* state 32 */ {
        { 25, HUFFMAN_EMIT,  63},
        { 26, HUFFMAN_EMIT,  63},
        { 27, HUFFMAN_EMIT,  63},
        { 28, HUFFMAN_EMIT,  63},
        { 29, HUFFMAN_EMIT,  39},
        { 30, HUFFMAN_EMIT,  39},
        { 29, HUFFMAN_EMIT,  43},
        { 30, HUFFMAN_EMIT,  43},
        { 29, HUFFMAN_EMIT, 124},
        { 30, HUFFMAN_EMIT, 124},
        {  0, HUFFMAN_EMIT,  35},
        {  0, HUFFMAN_EMIT,  62},
        { 77, HUFFMAN_NONE,   0},
        { 78, HUFFMAN_NONE,   0},
        { 79, HUFFMAN_NONE,   0},
        { 80, HUFFMAN_NONE,   0}
    },
    /* state 33 */ {
        { 17, HUFFMAN_EMIT,  38},
        { 18, HUFFMAN_EMIT,  38},
        { 19, HUFFMAN_EMIT,  38},
        { 20, HUFFMAN_EMIT,  38},
        { 21, HUFFMAN_EMIT,  38},
        { 22, HUFFMAN_EMIT,  38},
        { 23, HUFFMAN_EMIT,  38},
        { 24, HUFFMAN_EMIT,  38},
        { 17, HUFFMAN_EMIT,  42},
        { 18, HUFFMAN_EMIT,  42},
        { 19, HUFFMAN_EMIT,  42},
        { 20, HUFFMAN_EMIT,  42},
        { 21, HUFFMAN_EMIT,  42},
        { 22, HUFFMAN_EMIT,  42},
        { 23, HUFFMAN_EMIT,  42},
        { 24, HUFFMAN_EMIT,  42}
    },
    /* state 34 */ {
        { 17, HUFFMAN_EMIT,  44},
        { 18, HUFFMAN_EMIT,  44},
        { 19, HUFFMAN_EMIT,  44},
        { 20, HUFFMAN_EMIT,  44},
        { 21, HUFFMAN_EMIT,  44},
        { 22, HUFFMAN_EMIT,  44},
        { 23, HUFFMAN_EMIT,  44},
        { 24, HUFFMAN_EMIT,  44},
        { 17, HUFFMAN_EMIT,  59},
        { 18, HUFFMAN_EMIT,  59},
        { 19, HUFFMAN_EMIT,  59},
        { 20, HUFFMAN_EMIT,  59},
        { 21, HUFFMAN_EMIT,  59},
        { 22, HUFFMAN_EMIT,  59},
        { 23, HUFFMAN_EMIT,  59},
        { 24, HUFFMAN_EMIT,  59}
    },
    /* state 35 */ {
        { 17, HUFFMAN_EMIT,  88},
        { 18, HUFFMAN_EMIT,  88},
        { 19, HUFFMAN_EMIT,  88},
        { 20, HUFFMAN_EMIT,  88},
        { 21, HUFFMAN_EMIT,  88},
        { 22, HUFFMAN_EMIT,  88},
        { 23, HUFFMAN_EMIT,  88},
        { 24, HUFFMAN_EMIT,  88},
        { 17, HUFFMAN_EMIT,  90},
        { 18, HUFFMAN_EMIT,  90},
        { 19, HUFFMAN_EMIT,  90},
        { 20, HUFFMAN_EMIT,  90},
        { 21, HUFFMAN_EMIT,  90},
        { 22, HUFFMAN_EMIT,  90},
        { 23, HUFFMAN_EMIT,  90},
        { 24, HUFFMAN_EMIT,  90}
    },
    /* state 36 */ {
        { 29, HUFFMAN_EMIT,  33},
        { 30, HUFFMAN_EMIT,  33},
        { 29, HUFFMAN_EMIT,  34},
        { 30, HUFFMAN_EMIT,  34},
        { 29, HUFFMAN_EMIT,  40},
        { 30, HUFFMAN_EMIT,  40},
        { 29, HUFFMAN_EMIT,  41},
        { 30, HUFFMAN_EMIT,  41},
        { 29, HUFFMAN_EMIT,  63},
        { 30, HUFFMAN_EMIT,  63},
        {  0, HUFFMAN_EMIT,  39},
        {  0, HUFFMAN_EMIT,  43},
        {  0, HUFFMAN_EMIT, 124},
        { 81, HUFFMAN_NONE,   0},
        { 82, HUFFMAN_NONE,   0},
        { 83, HUFFMAN_NONE,   0}
    },
    /* state 37 */ {
        { 17, HUFFMAN_EMIT,  58},
        { 18, HUFFMAN_EMIT,  58},
        { 19, HUFFMAN_EMIT,  58},
        { 20, HUFFMAN_EMIT,  58},
        { 21, HUFFMAN_EMIT,  58},
        { 22, HUFFMAN_EMIT,  58},
        { 23, HUFFMAN_EMIT,  58},
        { 24, HUFFMAN_EMIT,  58},
        { 17, HUFFMAN_EMIT,  66},
        { 18, HUFFMAN_EMIT,  66},
        { 19, HUFFMAN_EMIT,  66},
        { 20, HUFFMAN_EMIT,  66},
        { 21, HUFFMAN_EMIT,  66},
        { 22, HUFFMAN_EMIT,  66},
        { 23, HUFFMAN_EMIT,  66},
        { 24, HUFFMAN_EMIT,  66}
    },
    /* state 38 */ {
        { 17, HUFFMAN_EMIT,  67},
        { 18, HUFFMAN_EMIT,  67},
        { 19, HUFFMAN_EMIT,  67},
        { 20, HUFFMAN_EMIT,  67},
        { 21, HUFFMAN_EMIT,  67},
        { 22, HUFFMAN_EMIT,  67},
        { 23, HUFFMAN_EMIT,  67},
        { 24, HUFFMAN_EMIT,  67},
        { 17, HUFFMAN_EMIT,  68},
        { 18, HUFFMAN_EMIT,  68},
        { 19, HUFFMAN_EMIT,  68},
        { 20, HUFFMAN_EMIT,  68},
        { 21, HUFFMAN_EMIT,  68},
        { 22, HUFFMAN_EMIT,  68},
        { 23, HUFFMAN_EMIT,  68},
        { 24, HUFFMAN_EMIT,  68}
    },
    /* state 39 */ {
        { 17, HUFFMAN_EMIT,  69},
        { 18, HUFFMAN_EMIT,  69},
        { 19, HUFFMAN_EMIT,  69},
        { 20, HUFFMAN_EMIT,  69},
        { 21, HUFFMAN_EMIT,  69},
        { 22, HUFFMAN_EMIT,  69},
        { 23, HUFFMAN_EMIT,  69},
        { 24, HUFFMAN_EMIT,  69},
        { 17, HUFFMAN_EMIT,  70},
        { 18, HUFFMAN_EMIT,  70},
        { 19, HUFFMAN_EMIT,  70},
        { 20, HUFFMAN_EMIT,  70},
        { 21, HUFFMAN_EMIT,  70},
        { 22, HUFFMAN_EMIT,  70},
        { 23, HUFFMAN_EMIT,  70},
        { 24, HUFFMAN_EMIT,  70}
    },
    /* state 40 */ {
        { 17, HUFFMAN_EMIT,  71},
        { 18, HUFFMAN_EMIT,  71},
        { 19, HUFFMAN_EMIT,  71},
        { 20, HUFFMAN_EMIT,  71},
        { 21, HUFFMAN_EMIT,  71},
        { 22, HUFFMAN_EMIT,  71},
        { 23, HUFFMAN_EMIT,  71},
        { 24, HUFFMAN_EMIT,  71},
        { 17, HUFFMAN_EMIT,  72},
        { 18, HUFFMAN_EMIT,  72},
        { 19, HUFFMAN_EMIT,  72},
        { 20, HUFFMAN_EMIT,  72},
        { 21, HUFFMAN_EMIT,  72},
        { 22, HUFFMAN_EMIT,  72},
        { 23, HUFFMAN_EMIT,  72},
        { 24, HUFFMAN_EMIT,  72}
    },
    /* state 41 */ {
        { 17, HUFFMAN_EMIT,  73},
        { 18, HUFFMAN_EMIT,  73},
        { 19, HUFFMAN_EMIT,  73},
        { 20, HUFFMAN_EMIT,  73},
        { 21, HUFFMAN_EMIT,  73},
        { 22, HUFFMAN_EMIT,  73},
        { 23, HUFFMAN_EMIT,  73},
        { 24, HUFFMAN_EMIT,  73},
        { 17, HUFFMAN_EMIT,  74},
        { 18, HUFFMAN_EMIT,  74},
        { 19, HUFFMAN_EMIT,  74},
        { 20, HUFFMAN_EMIT,  74},
        { 21, HUFFMAN_EMIT,  74},
        { 22, HUFFMAN_EMIT,  74},
        { 23, HUFFMAN_EMIT,  74},
        { 24, HUFFMAN_EMIT,  74}
    },
    /* state 42 */ {
        { 17, HUFFMAN_EMIT,  75},
        { 18, HUFFMAN_EMIT,  75},
        { 19, HUFFMAN_EMIT,  75},
        { 20, HUFFMAN_EMIT,  75},
        { 21, HUFFMAN_EMIT,  75},
        { 22, HUFFMAN_EMIT,  75},
        { 23, HUFFMAN_EMIT,  75},
        { 24, HUFFMAN_EMIT,  75},
        { 17, HUFFMAN_EMIT,  76},
        { 18, HUFFMAN_EMIT,  76},
        { 19, HUFFMAN_EMIT,  76},
        { 20, HUFFMAN_EMIT,  76},
        { 21, HUFFMAN_EMIT,  76},
        { 22, HUFFMAN_EMIT,  76},
        { 23, HUFFMAN_EMIT,  76},
        { 24, HUFFMAN_EMIT,  76}
    },
    /* state 43 */ {
        { 17, HUFFMAN_EMIT,  77},
        { 18, HUFFMAN_EMIT,  77},
        { 19, HUFFMAN_EMIT,  77},
        { 20, HUFFMAN_EMIT,  77},
        { 21, HUFFMAN_EMIT,  77},
        { 22, HUFFMAN_EMIT,  77},
        { 23, HUFFMAN_EMIT,  77},
        { 24, HUFFMAN_EMIT,  77},
        { 17, HUFFMAN_EMIT,  78},
        { 18, HUFFMAN_EMIT,  78},
        { 19, HUFFMAN_EMIT,  78},
        { 20, HUFFMAN_EMIT,  78},
        { 21, HUFFMAN_EMIT,  78},
        { 22, HUFFMAN_EMIT,  78},
        { 23, HUFFMAN_EMIT,  78},
        { 24, HUFFMAN_EMIT,  78}
    },
    /* state 44 */ {
        { 17, HUFFMAN_EMIT,  79},
        { 18, HUFFMAN_EMIT,  79},
        { 19, HUFFMAN_EMIT,  79},
        { 20, HUFFMAN_EMIT,  79},
        { 21, HUFFMAN_EMIT,  79},
        { 22, HUFFMAN_EMIT,  79},
        { 23, HUFFMAN_EMIT,  79},
        { 24, HUFFMAN_EMIT,  79},
        { 17, HUFFMAN_EMIT,  80},
        { 18, HUFFMAN_EMIT,  80},
        { 19, HUFFMAN_EMIT,  80},
        { 20, HUFFMAN_EMIT,  80},
        { 21, HUFFMAN_EMIT,  80},
        { 22, HUFFMAN_EMIT,  80},
        { 23, HUFFMAN_EMIT,  80},
        { 24, HUFFMAN_EMIT,  80}
    },
    /* state 45 */ {
        { 17, HUFFMAN_EMIT,  81},
        { 18, HUFFMAN_EMIT,  81},
        { 19, HUFFMAN_EMIT,  81},
        { 20, HUFFMAN_EMIT,  81},
        { 21, HUFFMAN_EMIT,  81},
        { 22, HUFFMAN_EMIT,  81},
        { 23, HUFFMAN_EMIT,  81},
        { 24, HUFFMAN_EMIT,  81},
        { 17, HUFFMAN_EMIT,  82},
        { 18, HUFFMAN_EMIT,  82},
        { 19, HUFFMAN_EMIT,  82},
        { 20, HUFFMAN_EMIT,  82},
        { 21, HUFFMAN_EMIT,  82},
        { 22, HUFFMAN_EMIT,  82},
        { 23, HUFFMAN_EMIT,  82},
        { 24, HUFFMAN_EMIT,  82}
    },
    /* state 46 */ {
        { 17, HUFFMAN_EMIT,  83},
        { 18, HUFFMAN_EMIT,  83},
        { 19, HUFFMAN_EMIT,  83},
        { 20, HUFFMAN_EMIT,  83},
        { 21, HUFFMAN_EMIT,  83},
        { 22, HUFFMAN_EMIT,  83},
        { 23, HUFFMAN_EMIT,  83},
        { 24, HUFFMAN_EMIT,  83},
        { 17, HUFFMAN_EMIT,  84},
        { 18, HUFFMAN_EMIT,  84},
        { 19, HUFFMAN_EMIT,  84},
        { 20, HUFFMAN_EMIT,  84},
        { 21, HUFFMAN_EMIT,  84},
        { 22, HUFFMAN_EMIT,  84},
        { 23, HUFFMAN_EMIT,  84},
        { 24, HUFFMAN_EMIT,  84}
    },
    /* state 47 */ {
        { 17, HUFFMAN_EMIT,  85},
        { 18, HUFFMAN_EMIT,  85},
        { 19, HUFFMAN_EMIT,  85},
        { 20, HUFFMAN_EMIT,  85},
        { 21, HUFFMAN_EMIT,  85},
        { 22, HUFFMAN_EMIT,  85},
        { 23, HUFFMAN_EMIT,  85},
        { 24, HUFFMAN_EMIT,  85},
        { 17, HUFFMAN_EMIT,  86},
        { 18, HUFFMAN_EMIT,  86},
        { 19, HUFFMAN_EMIT,  86},
        { 20, HUFFMAN_EMIT,  86},
        { 21, HUFFMAN_EMIT,  86},
        { 22, HUFFMAN_EMIT,  86},
        { 23, HUFFMAN_EMIT,  86},
        { 24, HUFFMAN_EMIT,  86}
    },
    /* state 48 */ {
        { 17, HUFFMAN_EMIT,  87},
        { 18, HUFFMAN_EMIT,  87},
        { 19, HUFFMAN_EMIT,  87},
        { 20, HUFFMAN_EMIT,  87},
        { 21, HUFFMAN_EMIT,  87},
        { 22, HUFFMAN_EMIT,  87},
        { 23, HUFFMAN_EMIT,  87},
        { 24, HUFFMAN_EMIT,  87},
        { 17, HUFFMAN_EMIT,  89},
        { 18, HUFFMAN_EMIT,  89},
        { 19, HUFFMAN_EMIT,  89},
        { 20, HUFFMAN_EMIT,  89},
        { 21, HUFFMAN_EMIT,  89},
        { 22, HUFFMAN_EMIT,  89},
        { 23, HUFFMAN_EMIT,  89},
        { 24, HUFFMAN_EMIT,  89}
    },
    /* state 49 */ {
        { 17, HUFFMAN_EMIT, 106},
        { 18, HUFFMAN_EMIT, 106},
        { 19, HUFFMAN_EMIT, 106},
        { 20, HUFFMAN_EMIT, 106},
        { 21, HUFFMAN_EMIT, 106},
        { 22, HUFFMAN_EMIT, 106},
        { 23, HUFFMAN_EMIT, 106},
        { 24, HUFFMAN_EMIT, 106},
        { 17, HUFFMAN_EMIT, 107},
        { 18, HUFFMAN_EMIT, 107},
        { 19, HUFFMAN_EMIT, 107},
        { 20, HUFFMAN_EMIT, 107},
        { 21, HUFFMAN_EMIT, 107},
        { 22, HUFFMAN_EMIT, 107},
        { 23, HUFFMAN_EMIT, 107},
        { 24, HUFFMAN_EMIT, 107}
    },
    /* state 50 */ {
        { 17, HUFFMAN_EMIT, 113},
        { 18, HUFFMAN_EMIT, 113},
        { 19, HUFFMAN_EMIT, 113},
        { 20, HUFFMAN_EMIT, 113},
        { 21, HUFFMAN_EMIT, 113},
        { 22, HUFFMAN_EMIT, 113},
        { 23, HUFFMAN_EMIT, 113},
        { 24, HUFFMAN_EMIT, 113},
        { 17, HUFFMAN_EMIT, 118},
        { 18, HUFFMAN_EMIT, 118},
        { 19, HUFFMAN_EMIT, 118},
        { 20, HUFFMAN_EMIT, 118},
        { 21, HUFFMAN_EMIT, 118},
        { 22, HUFFMAN_EMIT, 118},
        { 23, HUFFMAN_EMIT, 118},
        { 24, HUFFMAN_EMIT, 118}
    },
    /* state 51 */ {
        { 17, HUFFMAN_EMIT, 119},
        { 18, HUFFMAN_EMIT, 119},
        { 19, HUFFMAN_EMIT, 119},
        { 20, HUFFMAN_EMIT, 119},
        { 21, HUFFMAN_EMIT, 119},
        { 22, HUFFMAN_EMIT, 119},
        { 23, HUFFMAN_EMIT, 119},
        { 24, HUFFMAN_EMIT, 119},
        { 17, HUFFMAN_EMIT, 120},
        { 18, HUFFMAN_EMIT, 120},
        { 19, HUFFMAN_EMIT, 120},
        { 20, HUFFMAN_EMIT, 120},
        { 21, HUFFMAN_EMIT, 120},
        { 22, HUFFMAN_EMIT, 120},
        { 23, HUFFMAN_EMIT, 120},
        { 24, HUFFMAN_EMIT, 120}
    },
    /* state 52 */ {
        { 17, HUFFMAN_EMIT, 121},
        { 18, HUFFMAN_EMIT, 121},
        { 19, HUFFMAN_EMIT, 121},
        { 20, HUFFMAN_EMIT, 121},
        { 21, HUFFMAN_EMIT, 121},
        { 22, HUFFMAN_EMIT, 121},
        { 23, HUFFMAN_EMIT, 121},
        { 24, HUFFMAN_EMIT, 121},
        { 17, HUFFMAN_EMIT, 122},
        { 18, HUFFMAN_EMIT, 122},
        { 19, HUFFMAN_EMIT, 122},
        { 20, HUFFMAN_EMIT, 122},
        { 21, HUFFMAN_EMIT, 122},
        { 22, HUFFMAN_EMIT, 122},
        { 23, HUFFMAN_EMIT, 122},
        { 24, HUFFMAN_EMIT, 122}
    },
    /* state 53 */ {
        { 25, HUFFMAN_EMIT,  38},
        { 26, HUFFMAN_EMIT,  38},
        { 27, HUFFMAN_EMIT,  38},
        { 28, HUFFMAN_EMIT,  38},
        { 25, HUFFMAN_EMIT,  42},
        { 26, HUFFMAN_EMIT,  42},
        { 27, HUFFMAN_EMIT,  42},
        { 28, HUFFMAN_EMIT,  42},
        { 25, HUFFMAN_EMIT,  44},
        { 26, HUFFMAN_EMIT,  44},
        { 27, HUFFMAN_EMIT,  44},
        { 28, HUFFMAN_EMIT,  44},
        { 25, HUFFMAN_EMIT,  59},
        { 26, HUFFMAN_EMIT,  59},
        { 27, HUFFMAN_EMIT,  59},
        { 28, HUFFMAN_EMIT,  59}
    },
    /* state 54 */ {
        { 25, HUFFMAN_EMIT,  88},
        { 26, HUFFMAN_EMIT,  88},
        { 27, HUFFMAN_EMIT,  88},
        { 28, HUFFMAN_EMIT,  88},
        { 25, HUFFMAN_EMIT,  90},
        { 26, HUFFMAN_EMIT,  90},
        { 27, HUFFMAN_EMIT,  90},
        { 28, HUFFMAN_EMIT,  90},
        {  0, HUFFMAN_EMIT,  33},
        {  0, HUFFMAN_EMIT,  34},
        {  0, HUFFMAN_EMIT,  40},
        {  0, HUFFMAN_EMIT,  41},
        {  0, HUFFMAN_EMIT,  63},
        { 84, HUFFMAN_NONE,   0},
        { 85, HUFFMAN_NONE,   0},
        { 86, HUFFMAN_NONE,   0}
    },
    /* state 55 */ {
        { 17, HUFFMAN_EMIT,  32},
        { 18, HUFFMAN_EMIT,  32},
        { 19, HUFFMAN_EMIT,  32},
        { 20, HUFFMAN_EMIT,  32},
        { 21, HUFFMAN_EMIT,  32},
        { 22, HUFFMAN_EMIT,  32},
        { 23, HUFFMAN_EMIT,  32},
        { 24, HUFFMAN_EMIT,  32},
        { 17, HUFFMAN_EMIT,  37},
        { 18, HUFFMAN_EMIT,  37},
        { 19, HUFFMAN_EMIT,  37},
        { 20, HUFFMAN_EMIT,  37},
        { 21, HUFFMAN_EMIT,  37},
        { 22, HUFFMAN_EMIT,  37},
        { 23, HUFFMAN_EMIT,  37},
        { 24, HUFFMAN_EMIT,  37}
    },
    /* state 56 */ {
        { 17, HUFFMAN_EMIT,  45},
        { 18, HUFFMAN_EMIT,  45},
        { 19, HUFFMAN_EMIT,  45},
        { 20, HUFFMAN_EMIT,  45},
        { 21, HUFFMAN_EMIT,  45},
        { 22, HUFFMAN_EMIT,  45},
        { 23, HUFFMAN_EMIT,  45},
        { 24, HUFFMAN_EMIT,  45},
        { 17, HUFFMAN_EMIT,  46},
        { 18, HUFFMAN_EMIT,  46},
        { 19, HUFFMAN_EMIT,  46},
        { 20, HUFFMAN_EMIT,  46},
        { 21, HUFFMAN_EMIT,  46},
        { 22, HUFFMAN_EMIT,  46},
        { 23, HUFFMAN_EMIT,  46},
        { 24, HUFFMAN_EMIT,  46}
    },
    /* state 57 */ {
        { 17, HUFFMAN_EMIT,  47},
        { 18, HUFFMAN_EMIT,  47},
        { 19, HUFFMAN_EMIT,  47},
        { 20, HUFFMAN_EMIT,  47},
        { 21, HUFFMAN_EMIT,  47},
        { 22, HUFFMAN_EMIT,  47},
        { 23, HUFFMAN_EMIT,  47},
        { 24, HUFFMAN_EMIT,  47},
        { 17, HUFFMAN_EMIT,  51},
        { 18, HUFFMAN_EMIT,  51},
        { 19, HUFFMAN_EMIT,  51},
        { 20, HUFFMAN_EMIT,  51},
        { 21, HUFFMAN_EMIT,  51},
        { 22, HUFFMAN_EMIT,  51},
        { 23, HUFFMAN_EMIT,  51},
        { 24, HUFFMAN_EMIT,  51}
    },
    /* state 58 */ {
        { 17, HUFFMAN_EMIT,  52},
        { 18, HUFFMAN_EMIT,  52},
        { 19, HUFFMAN_EMIT,  52},
        { 20, HUFFMAN_EMIT,  52},
        { 21, HUFFMAN_EMIT,  52},
        { 22, HUFFMAN_EMIT,  52},
        { 23, HUFFMAN_EMIT,  52},
        { 24, HUFFMAN_EMIT,  52},
        { 17, HUFFMAN_EMIT,  53},
        { 18, HUFFMAN_EMIT,  53},
        { 19, HUFFMAN_EMIT,  53},
        { 20, HUFFMAN_EMIT,  53},
        { 21, HUFFMAN_EMIT,  53},
        { 22, HUFFMAN_EMIT,  53},
        { 23, HUFFMAN_EMIT,  53},
        { 24, HUFFMAN_EMIT,  53}
    },
    /* state 59 */ {
        { 17, HUFFMAN_EMIT,  54},
        { 18, HUFFMAN_EMIT,  54},
        { 19, HUFFMAN_EMIT,  54},
        { 20, HUFFMAN_EMIT,  54},
        { 21, HUFFMAN_EMIT,  54},
        { 22, HUFFMAN_EMIT,  54},
        { 23, HUFFMAN_EMIT,  54},
        { 24, HUFFMAN_EMIT,  54},
        { 17, HUFFMAN_EMIT,  55},
        { 18, HUFFMAN_EMIT,  55},
        { 19, HUFFMAN_EMIT,  55},
        { 20, HUFFMAN_EMIT,  55},
        { 21, HUFFMAN_EMIT,  55},
        { 22, HUFFMAN_EMIT,  55},
        { 23, HUFFMAN_EMIT,  55},
        { 24, HUFFMAN_EMIT,  55}
    },
    /* state 60 */ {
        { 17, HUFFMAN_EMIT,  56},
        { 18, HUFFMAN_EMIT,  56},
        { 19, HUFFMAN_EMIT,  56},
        { 20, HUFFMAN_EMIT,  56},
        { 21, HUFFMAN_EMIT,  56},
        { 22, HUFFMAN_EMIT,  56},
        { 23, HUFFMAN_EMIT,  56},
        { 24, HUFFMAN_EMIT,  56},
        { 17, HUFFMAN_EMIT,  57},
        { 18, HUFFMAN_EMIT,  57},
        { 19, HUFFMAN_EMIT,  57},
        { 20, HUFFMAN_EMIT,  57},
        { 21, HUFFMAN_EMIT,  57},
        { 22, HUFFMAN_EMIT,  57},
        { 23, HUFFMAN_EMIT,  57},
        { 24, HUFFMAN_EMIT,  57}
    },
    /* state 61 */ {
        { 17, HUFFMAN_EMIT,  61},
        { 18, HUFFMAN_EMIT,  61},
        { 19, HUFFMAN_EMIT,  61},
        { 20, HUFFMAN_EMIT,  61},
        { 21, HUFFMAN_EMIT,  61},
        { 22, HUFFMAN_EMIT,  61},
        { 23, HUFFMAN_EMIT,  61},
        { 24, HUFFMAN_EMIT,  61},
        { 17, HUFFMAN_EMIT,  65},
        { 18, HUFFMAN_EMIT,  65},
        { 19, HUFFMAN_EMIT,  65},
        { 20, HUFFMAN_EMIT,  65},
        { 21, HUFFMAN_EMIT,  65},
        { 22, HUFFMAN_EMIT,  65},
        { 23, HUFFMAN_EMIT,  65},
        { 24, HUFFMAN_EMIT,  65}
    },
    /* state 62 */ {
        { 17, HUFFMAN_EMIT,  95},
        { 18, HUFFMAN_EMIT,  95},
        { 19, HUFFMAN_EMIT,  95},
        { 20, HUFFMAN_EMIT,  95},
        { 21, HUFFMAN_EMIT,  95},
        { 22, HUFFMAN_EMIT,  95},
        { 23, HUFFMAN_EMIT,  95},
        { 24, HUFFMAN_EMIT,  95},
        { 17, HUFFMAN_EMIT,  98},
        { 18, HUFFMAN_EMIT,  98},
        { 19, HUFFMAN_EMIT,  98},
        { 20, HUFFMAN_EMIT,  98},
        { 21, HUFFMAN_EMIT,  98},
        { 22, HUFFMAN_EMIT,  98},
        { 23, HUFFMAN_EMIT,  98},
        { 24, HUFFMAN_EMIT,  98}
    },
    /* state 63 */ {
        { 17, HUFFMAN_EMIT, 100},
        { 18, HUFFMAN_EMIT, 100},
        { 19, HUFFMAN_EMIT, 100},
        { 20, HUFFMAN_EMIT, 100},
        { 21, HUFFMAN_EMIT, 100},
        { 22, HUFFMAN_EMIT, 100},
        { 23, HUFFMAN_EMIT, 100},
        { 24, HUFFMAN_EMIT, 100},
        { 17, HUFFMAN_EMIT, 102},
        { 18, HUFFMAN_EMIT, 102},
        { 19, HUFFMAN_EMIT, 102},
        { 20, HUFFMAN_EMIT, 102},
        { 21, HUFFMAN_EMIT, 102},
        { 22, HUFFMAN_EMIT, 102},
        { 23, HUFFMAN_EMIT, 102},
        { 24, HUFFMAN_EMIT, 102}
    },
    /* state 64 */ {
        { 17, HUFFMAN_EMIT, 103},
        { 18, HUFFMAN_EMIT, 103},
        { 19, HUFFMAN_EMIT, 103},
        { 20, HUFFMAN_EMIT, 103},
        { 21, HUFFMAN_EMIT, 103},
        { 22, HUFFMAN_EMIT, 103},
        { 23, HUFFMAN_EMIT, 103},
        { 24, HUFFMAN_EMIT, 103},
        { 17, HUFFMAN_EMIT, 104},
        { 18, HUFFMAN_EMIT, 104},
        { 19, HUFFMAN_EMIT, 104},
        { 20, HUFFMAN_EMIT, 104},
        { 21, HUFFMAN_EMIT, 104},
        { 22, HUFFMAN_EMIT, 104},
        { 23, HUFFMAN_EMIT, 104},
        { 24, HUFFMAN_EMIT, 104}
    },
    /* state 65 */ {
        { 17, HUFFMAN_EMIT, 108},
        { 18, HUFFMAN_EMIT, 108},
        { 19, HUFFMAN_EMIT, 108},
        { 20, HUFFMAN_EMIT, 108},
        { 21, HUFFMAN_EMIT, 108},
        { 22, HUFFMAN_EMIT, 108},
        { 23, HUFFMAN_EMIT, 108},
        { 24, HUFFMAN_EMIT, 108},
        { 17, HUFFMAN_EMIT, 109},
        { 18, HUFFMAN_EMIT, 109},
        { 19, HUFFMAN_EMIT, 109},
        { 20, HUFFMAN_EMIT, 109},
        { 21, HUFFMAN_EMIT, 109},
        { 22, HUFFMAN_EMIT, 109},
        { 23, HUFFMAN_EMIT, 109},
        { 24, HUFFMAN_EMIT, 109}
    },
    /* state 66 */ {
        { 17, HUFFMAN_EMIT, 110},
        { 18, HUFFMAN_EMIT, 110},
        { 19, HUFFMAN_EMIT, 110},
        { 20, HUFFMAN_EMIT, 110},
        { 21, HUFFMAN_EMIT, 110},
        { 22, HUFFMAN_EMIT, 110},
        { 23, HUFFMAN_EMIT, 110},
        { 24, HUFFMAN_EMIT, 110},
        { 17, HUFFMAN_EMIT, 112},
        { 18, HUFFMAN_EMIT, 112},
        { 19, HUFFMAN_EMIT, 112},
        { 20, HUFFMAN_EMIT, 112},
        { 21, HUFFMAN_EMIT, 112},
        { 22, HUFFMAN_EMIT, 112},
        { 23, HUFFMAN_EMIT, 112},
        { 24, HUFFMAN_EMIT, 112}
    },
    /* state 67 */ {
        { 17, HUFFMAN_EMIT, 114},
        { 18, HUFFMAN_EMIT, 114},
        { 19, HUFFMAN_EMIT, 114},
        { 20, HUFFMAN_EMIT, 114},
        { 21, HUFFMAN_EMIT, 114},
        { 22, HUFFMAN_EMIT, 114},
        { 23, HUFFMAN_EMIT, 114},
        { 24, HUFFMAN_EMIT, 114},
        { 17, HUFFMAN_EMIT, 117},
        { 18, HUFFMAN_EMIT, 117},
        { 19, HUFFMAN_EMIT, 117},
        { 20, HUFFMAN_EMIT, 117},
        { 21, HUFFMAN_EMIT, 117},
        { 22, HUFFMAN_EMIT, 117},
        { 23, HUFFMAN_EMIT, 117},
        { 24, HUFFMAN_EMIT, 117}
    },
    /* state 68 */ {
        { 25, HUFFMAN_EMIT,  58},
        { 26, HUFFMAN_EMIT,  58},
        { 27, HUFFMAN_EMIT,  58},
        { 28, HUFFMAN_EMIT,  58},
        { 25, HUFFMAN_EMIT,  66},
        { 26, HUFFMAN_EMIT,  66},
        { 27, HUFFMAN_EMIT,  66},
        { 28, HUFFMAN_EMIT,  66},
        { 25, HUFFMAN_EMIT,  67},
        { 26, HUFFMAN_EMIT,  67},
        { 27, HUFFMAN_EMIT,  67},
        { 28, HUFFMAN_EMIT,  67},
        { 25, HUFFMAN_EMIT,  68},
        { 26, HUFFMAN_EMIT,  68},
        { 27, HUFFMAN_EMIT,  68},
        { 28, HUFFMAN_EMIT,  68}
    },
    /* state 69 */ {
        { 25, HUFFMAN_EMIT,  69},
        { 26, HUFFMAN_EMIT,  69},
        { 27, HUFFMAN_EMIT,  69},
        { 28, HUFFMAN_EMIT,  69},
        { 25, HUFFMAN_EMIT,  70},
        { 26, HUFFMAN_EMIT,  70},
        { 27, HUFFMAN_EMIT,  70},
        { 28, HUFFMAN_EMIT,  70},
        { 25, HUFFMAN_EMIT,  71},
        { 26, HUFFMAN_EMIT,  71},
        { 27, HUFFMAN_EMIT,  71},
        { 28, HUFFMAN_EMIT,  71},
        { 25, HUFFMAN_EMIT,  72},
        { 26, HUFFMAN_EMIT,  72},
        { 27, HUFFMAN_EMIT,  72},
        { 28, HUFFMAN_EMIT,  72}
    },
    /* state 70 */ {
        { 25, HUFFMAN_EMIT,  73},
        { 26, HUFFMAN_EMIT,  73},
        { 27, HUFFMAN_EMIT,  73},
        { 28, HUFFMAN_EMIT,  73},
        { 25, HUFFMAN_EMIT,  74},
        { 26, HUFFMAN_EMIT,  74},
        { 27, HUFFMAN_EMIT,  74},
        { 28, HUFFMAN_EMIT,  74},
        { 25, HUFFMAN_EMIT,  75},
        { 26, HUFFMAN_EMIT,  75},
        { 27, HUFFMAN_EMIT,  75},
        { 28, HUFFMAN_EMIT,  75},
        { 25, HUFFMAN_EMIT,  76},
        { 26, HUFFMAN_EMIT,  76},
        { 27, HUFFMAN_EMIT,  76},
        { 28, HUFFMAN_EMIT,  76}
    },
    /* state 71 */ {
        { 25, HUFFMAN_EMIT,  77},
        { 26, HUFFMAN_EMIT,  77},
        { 27, HUFFMAN_EMIT,  77},
        { 28, HUFFMAN_EMIT,  77},
        { 25, HUFFMAN_EMIT,  78},
        { 26, HUFFMAN_EMIT,  78},
        { 27, HUFFMAN_EMIT,  78},
        { 28, HUFFMAN_EMIT,  78},
        { 25, HUFFMAN_EMIT,  79},
        { 26, HUFFMAN_EMIT,  79},
        { 27, HUFFMAN_EMIT,  79},
        { 28, HUFFMAN_EMIT,  79},
        { 25, HUFFMAN_EMIT,  80},
        { 26, HUFFMAN_EMIT,  80},
        { 27, HUFFMAN_EMIT,  80},
        { 28, HUFFMAN_EMIT,  80}
    },
    /* state 72 */ {
        { 25, HUFFMAN_EMIT,  81},
        { 26, HUFFMAN_EMIT,  81},
        { 27, HUFFMAN_EMIT,  81},
        { 28, HUFFMAN_EMIT,  81},
        { 25, HUFFMAN_EMIT,  82},
        { 26, HUFFMAN_EMIT,  82},
        { 27, HUFFMAN_EMIT,  82},
        { 28, HUFFMAN_EMIT,  82},
        { 25, HUFFMAN_EMIT,  83},
        { 26, HUFFMAN_EMIT,  83},
        { 27, HUFFMAN_EMIT,  83},
        { 28, HUFFMAN_EMIT,  83},
        { 25, HUFFMAN_EMIT,  84},
        { 26, HUFFMAN_EMIT,  84},
        { 27, HUFFMAN_EMIT,  84},
        { 28, HUFFMAN_EMIT,  84}
    },
    /* state 73 */ {
        { 25, HUFFMAN_EMIT,  85},
        { 26, HUFFMAN_EMIT,  85},
        { 27, HUFFMAN_EMIT,  85},
        { 28, HUFFMAN_EMIT,  85},
        { 25, HUFFMAN_EMIT,  86},
        { 26, HUFFMAN_EMIT,  86},
        { 27, HUFFMAN_EMIT,  86},
        { 28, HUFFMAN_EMIT,  86},
        { 25, HUFFMAN_EMIT,  87},
        { 26, HUFFMAN_EMIT,  87},
        { 27, HUFFMAN_EMIT,  87},
        { 28, HUFFMAN_EMIT,  87},
        { 25, HUFFMAN_EMIT,  89},
        { 26, HUFFMAN_EMIT,  89},
        { 27, HUFFMAN_EMIT,  89},
        { 28, HUFFMAN_EMIT,  89}
    },
    /* state 74 */ {
        { 25, HUFFMAN_EMIT, 106},
        { 26, HUFFMAN_EMIT, 106},
        { 27, HUFFMAN_EMIT, 106},
        { 28, HUFFMAN_EMIT, 106},
        { 25, HUFFMAN_EMIT, 107},
        { 26, HUFFMAN_EMIT, 107},
        { 27, HUFFMAN_EMIT, 107},
        { 28, HUFFMAN_EMIT, 107},
        { 25, HUFFMAN_EMIT, 113},
        { 26, HUFFMAN_EMIT, 113},
        { 27, HUFFMAN_EMIT, 113},
        { 28, HUFFMAN_EMIT, 113},
        { 25, HUFFMAN_EMIT, 118},
        { 26, HUFFMAN_EMIT, 118},
        { 27, HUFFMAN_EMIT, 118},
        { 28, HUFFMAN_EMIT, 118}
    },
    /* state 75 */ {
        { 25, HUFFMAN_EMIT, 119},
        { 26, HUFFMAN_EMIT, 119},
        { 27, HUFFMAN_EMIT, 119},
        { 28, HUFFMAN_EMIT, 119},
        { 25, HUFFMAN_EMIT, 120},
        { 26, HUFFMAN_EMIT, 120},
        { 27, HUFFMAN_EMIT, 120},
        { 28, HUFFMAN_EMIT, 120},
        { 25, HUFFMAN_EMIT, 121},
        { 26, HUFFMAN_EMIT, 121},
        { 27, HUFFMAN_EMIT, 121},
        { 28, HUFFMAN_EMIT, 121},
        { 25, HUFFMAN_EMIT, 122},
        { 26, HUFFMAN_EMIT, 122},
        { 27, HUFFMAN_EMIT, 122},
        { 28, HUFFMAN_EMIT, 122}
    },
    /* state 76 */ {
        { 29, HUFFMAN_EMIT,  38},
        { 30, HUFFMAN_EMIT,  38},
        { 29, HUFFMAN_EMIT,  42},
        { 30, HUFFMAN_EMIT,  42},
        { 29, HUFFMAN_EMIT,  44},
        { 30, HUFFMAN_EMIT,  44},
        { 29, HUFFMAN_EMIT,  59},
        { 30, HUFFMAN_EMIT,  59},
        { 29, HUFFMAN_EMIT,  88},
        { 30, HUFFMAN_EMIT,  88},
        { 29, HUFFMAN_EMIT,  90},
        { 30, HUFFMAN_EMIT,  90},
        { 87, HUFFMAN_NONE,   0},
        { 88, HUFFMAN_NONE,   0},
        { 89, HUFFMAN_NONE,   0},
        { 90, HUFFMAN_NONE,   0}
    },
    /* state 77 */ {
        { 17, HUFFMAN_EMIT,   0},
        { 18, HUFFMAN_EMIT,   0},
        { 19, HUFFMAN_EMIT,   0},
        { 20, HUFFMAN_EMIT,   0},
        { 21, HUFFMAN_EMIT,   0},
        { 22, HUFFMAN_EMIT,   0},
        { 23, HUFFMAN_EMIT,   0},
        { 24, HUFFMAN_EMIT,   0},
        { 17, HUFFMAN_EMIT,  36},
        { 18, HUFFMAN_EMIT,  36},
        { 19, HUFFMAN_EMIT,  36},
        { 20, HUFFMAN_EMIT,  36},
        { 21, HUFFMAN_EMIT,  36},
        { 22, HUFFMAN_EMIT,  36},
        { 23, HUFFMAN_EMIT,  36},
        { 24, HUFFMAN_EMIT,  36}
    },
    /* state 78 */ {
        { 17, HUFFMAN_EMIT,  64},
        { 18, HUFFMAN_EMIT,  64},
        { 19, HUFFMAN_EMIT,  64},
        { 20, HUFFMAN_EMIT,  64},
        { 21, HUFFMAN_EMIT,  64},
        { 22, HUFFMAN_EMIT,  64},
        { 23, HUFFMAN_EMIT,  64},
        { 24, HUFFMAN_EMIT,  64},
        { 17, HUFFMAN_EMIT,  91},
        { 18, HUFFMAN_EMIT,  91},
        { 19, HUFFMAN_EMIT,  91},
        { 20, HUFFMAN_EMIT,  91},
        { 21, HUFFMAN_EMIT,  91},
        { 22, HUFFMAN_EMIT,  91},
        { 23, HUFFMAN_EMIT,  91},
        { 24, HUFFMAN_EMIT,  91}
    },
    /* state 79 */ {
        { 17, HUFFMAN_EMIT,  93},
        { 18, HUFFMAN_EMIT,  93},
        { 19, HUFFMAN_EMIT,  93},
        { 20, HUFFMAN_EMIT,  93},
        { 21, HUFFMAN_EMIT,  93},
        { 22, HUFFMAN_EMIT,  93},
        { 23, HUFFMAN_EMIT,  93},
        { 24, HUFFMAN_EMIT,  93},
        { 17, HUFFMAN_EMIT, 126},
        { 18, HUFFMAN_EMIT, 126},
        { 19, HUFFMAN_EMIT, 126},
        { 20, HUFFMAN_EMIT, 126},
        { 21, HUFFMAN_EMIT, 126},
        { 22, HUFFMAN_EMIT, 126},
        { 23, HUFFMAN_EMIT, 126},
        { 24, HUFFMAN_EMIT, 126}
    },
    /* state 80 */ {
        { 25, HUFFMAN_EMIT,  94},
        { 26, HUFFMAN_EMIT,  94},
        { 27, HUFFMAN_EMIT,  94},
        { 28, HUFFMAN_EMIT,  94},
        { 25, HUFFMAN_EMIT, 125},
        { 26, HUFFMAN_EMIT, 125},
        { 27, HUFFMAN_EMIT, 125},
        { 28, HUFFMAN_EMIT, 125},
        { 29, HUFFMAN_EMIT,  60},
        { 30, HUFFMAN_EMIT,  60},
        { 29, HUFFMAN_EMIT,  96},
        { 30, HUFFMAN_EMIT,  96},
        { 29, HUFFMAN_EMIT, 123},
        { 30, HUFFMAN_EMIT, 123},
        { 91, HUFFMAN_NONE,   0},
        { 92, HUFFMAN_NONE,   0}
    },
    /* state 81 */ {
        { 17, HUFFMAN_EMIT,  35},
        { 18, HUFFMAN_EMIT,  35},
        { 19, HUFFMAN_EMIT,  35},
        { 20, HUFFMAN_EMIT,  35},
        { 21, HUFFMAN_EMIT,  35},
        { 22, HUFFMAN_EMIT,  35},
        { 23, HUFFMAN_EMIT,  35},
        { 24, HUFFMAN_EMIT,  35},
        { 17, HUFFMAN_EMIT,  62},
        { 18, HUFFMAN_EMIT,  62},
        { 19, HUFFMAN_EMIT,  62},
        { 20, HUFFMAN_EMIT,  62},
        { 21, HUFFMAN_EMIT,  62},
        { 22, HUFFMAN_EMIT,  62},
        { 23, HUFFMAN_EMIT,  62},
        { 24, HUFFMAN_EMIT,  62}
    },
    /* state 82 */ {
        { 25, HUFFMAN_EMIT,   0},
        { 26, HUFFMAN_EMIT,   0},
        { 27, HUFFMAN_EMIT,   0},
        { 28, HUFFMAN_EMIT,   0},
        { 25, HUFFMAN_EMIT,  36},
        { 26, HUFFMAN_EMIT,  36},
        { 27, HUFFMAN_EMIT,  36},
        { 28, HUFFMAN_EMIT,  36},
        { 25, HUFFMAN_EMIT,  64},
        { 26, HUFFMAN_EMIT,  64},
        { 27, HUFFMAN_EMIT,  64},
        { 28, HUFFMAN_EMIT,  64},
        { 25, HUFFMAN_EMIT,  91},
        { 26, HUFFMAN_EMIT,  91},
        { 27, HUFFMAN_EMIT,  91},
        { 28, HUFFMAN_EMIT,  91}
    },
    /* state 83 */ {
        { 25, HUFFMAN_EMIT,  93},
        { 26, HUFFMAN_EMIT,  93},
        { 27, HUFFMAN_EMIT,  93},
        { 28, HUFFMAN_EMIT,  93},
        { 25, HUFFMAN_EMIT, 126},
        { 26, HUFFMAN_EMIT, 126},
        { 27, HUFFMAN_EMIT, 126},
        { 28, HUFFMAN_EMIT, 126},
        { 29, HUFFMAN_EMIT,  94},
        { 30, HUFFMAN_EMIT,  94},
        { 29, HUFFMAN_EMIT, 125},
        { 30, HUFFMAN_EMIT, 125},
        {  0, HUFFMAN_EMIT,  60},
        {  0, HUFFMAN_EMIT,  96},
        {  0, HUFFMAN_EMIT, 123},
        { 93, HUFFMAN_NONE,   0}
    },
    /* state 84 */ {
        { 17, HUFFMAN_EMIT,  39},
        { 18, HUFFMAN_EMIT,  39},
        { 19, HUFFMAN_EMIT,  39},
        { 20, HUFFMAN_EMIT,  39},
        { 21, HUFFMAN_EMIT,  39},
        { 22, HUFFMAN_EMIT,  39},
        { 23, HUFFMAN_EMIT,  39},
        { 24, HUFFMAN_EMIT,  39},
        { 17, HUFFMAN_EMIT,  43},
        { 18, HUFFMAN_EMIT,  43},
        { 19, HUFFMAN_EMIT,  43},
        { 20, HUFFMAN_EMIT,  43},
        { 21, HUFFMAN_EMIT,  43},
        { 22, HUFFMAN_EMIT,  43},
        { 23, HUFFMAN_EMIT,  43},
        { 24, HUFFMAN_EMIT,  43}
    },
    /* state 85 */ {
        { 17, HUFFMAN_EMIT, 124},
        { 18, HUFFMAN_EMIT, 124},
        { 19, HUFFMAN_EMIT, 124},
        { 20, HUFFMAN_EMIT, 124},
        { 21, HUFFMAN_EMIT, 124},
        { 22, HUFFMAN_EMIT, 124},
        { 23, HUFFMAN_EMIT, 124},
        { 24, HUFFMAN_EMIT, 124},
        { 25, HUFFMAN_EMIT,  35},
        { 26, HUFFMAN_EMIT,  35},
        { 27, HUFFMAN_EMIT,  35},
        { 28, HUFFMAN_EMIT,  35},
        { 25, HUFFMAN_EMIT,  62},
        { 26, HUFFMAN_EMIT,  62},
        { 27, HUFFMAN_EMIT,  62},
        { 28, HUFFMAN_EMIT,  62}
    },
    /* state 86 */ {
        { 29, HUFFMAN_EMIT,   0},
        { 30, HUFFMAN_EMIT,   0},
        { 29, HUFFMAN_EMIT,  36},
        { 30, HUFFMAN_EMIT,  36},
        { 29, HUFFMAN_EMIT,  64},
        { 30, HUFFMAN_EMIT,  64},
        { 29, HUFFMAN_EMIT,  91},
        { 30, HUFFMAN_EMIT,  91},
        { 29, HUFFMAN_EMIT,  93},
        { 30, HUFFMAN_EMIT,  93},
        { 29, HUFFMAN_EMIT, 126},
        { 30, HUFFMAN_EMIT, 126},
        {  0, HUFFMAN_EMIT,  94},
        {  0, HUFFMAN_EMIT, 125},
        { 94, HUFFMAN_NONE,   0},
        { 95, HUFFMAN_NONE,   0}
    },
    /* state 87 */ {
        { 17, HUFFMAN_EMIT,  33},
        { 18, HUFFMAN_EMIT,  33},
        { 19, HUFFMAN_EMIT,  33},
        { 20, HUFFMAN_EMIT,  33},
        { 21, HUFFMAN_EMIT,  33},
        { 22, HUFFMAN_EMIT,  33},
        { 23, HUFFMAN_EMIT,  33},
        { 24, HUFFMAN_EMIT,  33},
        { 17, HUFFMAN_EMIT,  34},
        { 18, HUFFMAN_EMIT,  34},
        { 19, HUFFMAN_EMIT,  34},
        { 20, HUFFMAN_EMIT,  34},
        { 21, HUFFMAN_EMIT,  34},
        { 22, HUFFMAN_EMIT,  34},
        { 23, HUFFMAN_EMIT,  34},
        { 24, HUFFMAN_EMIT,  34}
    },
    /* state 88 */ {
        { 17, HUFFMAN_EMIT,  40},
        { 18, HUFFMAN_EMIT,  40},
        { 19, HUFFMAN_EMIT,  40},
        { 20, HUFFMAN_EMIT,  40},
        { 21, HUFFMAN_EMIT,  40},
        { 22, HUFFMAN_EMIT,  40},
        { 23, HUFFMAN_EMIT,  40},
        { 24, HUFFMAN_EMIT,  40},
        { 17, HUFFMAN_EMIT,  41},
        { 18, HUFFMAN_EMIT,  41},
        { 19, HUFFMAN_EMIT,  41},
        { 20, HUFFMAN_EMIT,  41},
        { 21, HUFFMAN_EMIT,  41},
        { 22, HUFFMAN_EMIT,  41},
        { 23, HUFFMAN_EMIT,  41},
        { 24, HUFFMAN_EMIT,  41}
    },
    /* state 89 */ {
        { 17, HUFFMAN_EMIT,  63},
        { 18, HUFFMAN_EMIT,  63},
        { 19, HUFFMAN_EMIT,  63},
        { 20, HUFFMAN_EMIT,  63},
        { 21, HUFFMAN_EMIT,  63},
        { 22, HUFFMAN_EMIT,  63},
        { 23, HUFFMAN_EMIT,  63},
        { 24, HUFFMAN_EMIT,  63},
        { 25, HUFFMAN_EMIT,  39},
        { 26, HUFFMAN_EMIT,  39},
        { 27, HUFFMAN_EMIT,  39},
        { 28, HUFFMAN_EMIT,  39},
        { 25, HUFFMAN_EMIT,  43},
        { 26, HUFFMAN_EMIT,  43},
        { 27, HUFFMAN_EMIT,  43},
        { 28, HUFFMAN_EMIT,  43}
    },
    /* state 90 */ {
        { 25, HUFFMAN_EMIT, 124},
        { 26, HUFFMAN_EMIT, 124},
        { 27, HUFFMAN_EMIT, 124},
        { 28, HUFFMAN_EMIT, 124},
        { 29, HUFFMAN_EMIT,  35},
        { 30, HUFFMAN_EMIT,  35},
        { 29, HUFFMAN_EMIT,  62},
        { 30, HUFFMAN_EMIT,  62},
        {  0, HUFFMAN_EMIT,   0},
        {  0, HUFFMAN_EMIT,  36},
        {  0, HUFFMAN_EMIT,  64},
        {  0, HUFFMAN_EMIT,  91},
        {  0, HUFFMAN_EMIT,  93},
        {  0, HUFFMAN_EMIT, 126},
        { 96, HUFFMAN_NONE,   0},
        { 97, HUFFMAN_NONE,   0}
    },
    /* state 91 */ {
        { 29, HUFFMAN_EMIT,  92},
        { 30, HUFFMAN_EMIT,  92},
        { 29, HUFFMAN_EMIT, 195},
        { 30, HUFFMAN_EMIT, 195},
        { 29, HUFFMAN_EMIT, 208},
        { 30, HUFFMAN_EMIT, 208},
        {  0, HUFFMAN_EMIT, 128},
        {  0, HUFFMAN_EMIT, 130},
        {  0, HUFFMAN_EMIT, 131},
        {  0, HUFFMAN_EMIT, 162},
        {  0, HUFFMAN_EMIT, 184},
        {  0, HUFFMAN_EMIT, 194},
        {  0, HUFFMAN_EMIT, 224},
        {  0, HUFFMAN_EMIT, 226},
        { 98, HUFFMAN_NONE,   0},
        { 99, HUFFMAN_NONE,   0}
    },
    /* state 92 */ {
        {100, HUFFMAN_NONE,   0},
        {101, HUFFMAN_NONE,   0},
        {102, HUFFMAN_NONE,   0},
        {103, HUFFMAN_NONE,   0},
        {104, HUFFMAN_NONE,   0},
        {105, HUFFMAN_NONE,   0},
        {106, HUFFMAN_NONE,   0},
        {107, HUFFMAN_NONE,   0},
        {108, HUFFMAN_NONE,   0},
        {109, HUFFMAN_NONE,   0},
        {110, HUFFMAN_NONE,   0},
        {111, HUFFMAN_NONE,   0},
        {112, HUFFMAN_NONE,   0},
        {113, HUFFMAN_NONE,   0},
        {114, HUFFMAN_NONE,   0},
        {115, HUFFMAN_NONE,   0}
    },
    /* state 93 */ {
        {  0, HUFFMAN_EMIT,  92},
        {  0, HUFFMAN_EMIT, 195},
        {  0, HUFFMAN_EMIT, 208},
        {116, HUFFMAN_NONE,   0},
        {117, HUFFMAN_NONE,   0},
        {118, HUFFMAN_NONE,   0},
        {119, HUFFMAN_NONE,   0},
        {120, HUFFMAN_NONE,   0},
        {121, HUFFMAN_NONE,   0},
        {122, HUFFMAN_NONE,   0},
        {123, HUFFMAN_NONE,   0},
        {124, HUFFMAN_NONE,   0},
        {125, HUFFMAN_NONE,   0},
        {126, HUFFMAN_NONE,   0},
        {127, HUFFMAN_NONE,   0},
        {128, HUFFMAN_NONE,   0}
    },
    /* state 94 */ {
        { 17, HUFFMAN_EMIT,  60},
        { 18, HUFFMAN_EMIT,  60},
        { 19, HUFFMAN_EMIT,  60},
        { 20, HUFFMAN_EMIT,  60},
        { 21, HUFFMAN_EMIT,  60},
        { 22, HUFFMAN_EMIT,  60},
        { 23, HUFFMAN_EMIT,  60},
        { 24, HUFFMAN_EMIT,  60},
        { 17, HUFFMAN_EMIT,  96},
        { 18, HUFFMAN_EMIT,  96},
        { 19, HUFFMAN_EMIT,  96},
        { 20, HUFFMAN_EMIT,  96},
        { 21, HUFFMAN_EMIT,  96},
        { 22, HUFFMAN_EMIT,  96},
        { 23, HUFFMAN_EMIT,  96},
        { 24, HUFFMAN_EMIT,  96}
    },
    /* state 95 */ {
        { 17, HUFFMAN_EMIT, 123},
        { 18, HUFFMAN_EMIT, 123},
        { 19, HUFFMAN_EMIT, 123},
        { 20, HUFFMAN_EMIT, 123},
        { 21, HUFFMAN_EMIT, 123},
        { 22, HUFFMAN_EMIT, 123},
        { 23, HUFFMAN_EMIT, 123},
        { 24, HUFFMAN_EMIT, 123},
        {129, HUFFMAN_NONE,   0},
        {130, HUFFMAN_NONE,   0},
        {131, HUFFMAN_NONE,   0},
        {132, HUFFMAN_NONE,   0},
        {133, HUFFMAN_NONE,   0},
        {134, HUFFMAN_NONE,   0},
        {135, HUFFMAN_NONE,   0},
        {136, HUFFMAN_NONE,   0}
    },
    /* state 96 */ {
        { 17, HUFFMAN_EMIT,  94},
        { 18, HUFFMAN_EMIT,  94},
        { 19, HUFFMAN_EMIT,  94},
        { 20, HUFFMAN_EMIT,  94},
        { 21, HUFFMAN_EMIT,  94},
        { 22, HUFFMAN_EMIT,  94},
        { 23, HUFFMAN_EMIT,  94},
        { 24, HUFFMAN_EMIT,  94},
        { 17, HUFFMAN_EMIT, 125},
        { 18, HUFFMAN_EMIT, 125},
        { 19, HUFFMAN_EMIT, 125},
        { 20, HUFFMAN_EMIT, 125},
        { 21, HUFFMAN_EMIT, 125},
        { 22, HUFFMAN_EMIT, 125},
        { 23, HUFFMAN_EMIT, 125},
        { 24, HUFFMAN_EMIT, 125}
    },
    /* state 97 */ {
        { 25, HUFFMAN_EMIT,  60},
        { 26, HUFFMAN_EMIT,  60},
        { 27, HUFFMAN_EMIT,  60},
        { 28, HUFFMAN_EMIT,  60},
        { 25, HUFFMAN_EMIT,  96},
        { 26, HUFFMAN_EMIT,  96},
        { 27, HUFFMAN_EMIT,  96},
        { 28, HUFFMAN_EMIT,  96},
        { 25, HUFFMAN_EMIT, 123},
        { 26, HUFFMAN_EMIT, 123},
        { 27, HUFFMAN_EMIT, 123},
        { 28, HUFFMAN_EMIT, 123},
        {137, HUFFMAN_NONE,   0},
        {138, HUFFMAN_NONE,   0},
        {139, HUFFMAN_NONE,   0},
        {140, HUFFMAN_NONE,   0}
    },
    /* state 98 */ {
        { 17, HUFFMAN_EMIT, 153},
        { 18, HUFFMAN_EMIT, 153},
        { 19, HUFFMAN_EMIT, 153},
        { 20, HUFFMAN_EMIT, 153},
        { 21, HUFFMAN_EMIT, 153},
        { 22, HUFFMAN_EMIT, 153},
        { 23, HUFFMAN_EMIT, 153},
        { 24, HUFFMAN_EMIT, 153},
        { 17, HUFFMAN_EMIT, 161},
        { 18, HUFFMAN_EMIT, 161},
        { 19, HUFFMAN_EMIT, 161},
        { 20, HUFFMAN_EMIT, 161},
        { 21, HUFFMAN_EMIT, 161},
        { 22, HUFFMAN_EMIT, 161},
        { 23, HUFFMAN_EMIT, 161},
        { 24, HUFFMAN_EMIT, 161}
    },
    /* state 99 */ {
        { 17, HUFFMAN_EMIT, 167},
        { 18, HUFFMAN_EMIT, 167},
        { 19, HUFFMAN_EMIT, 167},
        { 20, HUFFMAN_EMIT, 167},
        { 21, HUFFMAN_EMIT, 167},
        { 22, HUFFMAN_EMIT, 167},
        { 23, HUFFMAN_EMIT, 167},
        { 24, HUFFMAN_EMIT, 167},
        { 17, HUFFMAN_EMIT, 172},
        { 18, HUFFMAN_EMIT, 172},
        { 19, HUFFMAN_EMIT, 172},
        { 20, HUFFMAN_EMIT, 172},
        { 21, HUFFMAN_EMIT, 172},
        { 22, HUFFMAN_EMIT, 172},
        { 23, HUFFMAN_EMIT, 172},
        { 24, HUFFMAN_EMIT, 172}
    },
    /* state 100 */ {
        { 17, HUFFMAN_EMIT, 176},
        { 18, HUFFMAN_EMIT, 176},
        { 19, HUFFMAN_EMIT, 176},
        { 20, HUFFMAN_EMIT, 176},
        { 21, HUFFMAN_EMIT, 176},
        { 22, HUFFMAN_EMIT, 176},
        { 23, HUFFMAN_EMIT, 176},
        { 24, HUFFMAN_EMIT, 176},
        { 17, HUFFMAN_EMIT, 177},
        { 18, HUFFMAN_EMIT, 177},
        { 19, HUFFMAN_EMIT, 177},
        { 20, HUFFMAN_EMIT, 177},
        { 21, HUFFMAN_EMIT, 177},
        { 22, HUFFMAN_EMIT, 177},
        { 23, HUFFMAN_EMIT, 177},
        { 24, HUFFMAN_EMIT, 177}
    },
    /* state 101 */ {
        { 17, HUFFMAN_EMIT, 179},
        { 18, HUFFMAN_EMIT, 179},
        { 19, HUFFMAN_EMIT, 179},
        { 20, HUFFMAN_EMIT, 179},
        { 21, HUFFMAN_EMIT, 179},
        { 22, HUFFMAN_EMIT, 179},
        { 23, HUFFMAN_EMIT, 179},
        { 24, HUFFMAN_EMIT, 179},
        { 17, HUFFMAN_EMIT, 209},
        { 18, HUFFMAN_EMIT, 209},
        { 19, HUFFMAN_EMIT, 209},
        { 20, HUFFMAN_EMIT, 209},
        { 21, HUFFMAN_EMIT, 209},
        { 22, HUFFMAN_EMIT, 209},
        { 23, HUFFMAN_EMIT, 209},
        { 24, HUFFMAN_EMIT, 209}
    },
    /* state 102 */ {
        { 17, HUFFMAN_EMIT, 216},
        { 18, HUFFMAN_EMIT, 216},
        { 19, HUFFMAN_EMIT, 216},
        { 20, HUFFMAN_EMIT, 216},
        { 21, HUFFMAN_EMIT, 216},
        { 22, HUFFMAN_EMIT, 216},
        { 23, HUFFMAN_EMIT, 216},
        { 24, HUFFMAN_EMIT, 216},
        { 17, HUFFMAN_EMIT, 217},
        { 18, HUFFMAN_EMIT, 217},
        { 19, HUFFMAN_EMIT, 217},
        { 20, HUFFMAN_EMIT, 217},
        { 21, HUFFMAN_EMIT, 217},
        { 22, HUFFMAN_EMIT, 217},
        { 23, HUFFMAN_EMIT, 217},
        { 24, HUFFMAN_EMIT, 217}
    },
    /* state 103 */ {
        { 17, HUFFMAN_EMIT, 227},
        { 18, HUFFMAN_EMIT, 227},
        { 19, HUFFMAN_EMIT, 227},
        { 20, HUFFMAN_EMIT, 227},
        { 21, HUFFMAN_EMIT, 227},
        { 22, HUFFMAN_EMIT, 227},
        { 23, HUFFMAN_EMIT, 227},
        { 24, HUFFMAN_EMIT, 227},
        { 17, HUFFMAN_EMIT, 229},
        { 18, HUFFMAN_EMIT, 229},
        { 19, HUFFMAN_EMIT, 229},
        { 20, HUFFMAN_EMIT, 229},
        { 21, HUFFMAN_EMIT, 229},
        { 22, HUFFMAN_EMIT, 229},
        { 23, HUFFMAN_EMIT, 229},
        { 24, HUFFMAN_EMIT, 229}
    },
    /* state 104 */ {
        { 17, HUFFMAN_EMIT, 230},
        { 18, HUFFMAN_EMIT, 230},
        { 19, HUFFMAN_EMIT, 230},
        { 20, HUFFMAN_EMIT, 230},
        { 21, HUFFMAN_EMIT, 230},
        { 22, HUFFMAN_EMIT, 230},
        { 23, HUFFMAN_EMIT, 230},
        { 24, HUFFMAN_EMIT, 230},
        { 25, HUFFMAN_EMIT, 129},
        { 26, HUFFMAN_EMIT, 129},
        { 27, HUFFMAN_EMIT, 129},
        { 28, HUFFMAN_EMIT, 129},
        { 25, HUFFMAN_EMIT, 132},
        { 26, HUFFMAN_EMIT, 132},
        { 27, HUFFMAN_EMIT, 132},
        { 28, HUFFMAN_EMIT, 132}
    },
    /* state 105 */ {
        { 25, HUFFMAN_EMIT, 133},
        { 26, HUFFMAN_EMIT, 133},
        { 27, HUFFMAN_EMIT, 133},
        { 28, HUFFMAN_EMIT, 133},
        { 25, HUFFMAN_EMIT, 134},
        { 26, HUFFMAN_EMIT, 134},
        { 27, HUFFMAN_EMIT, 134},
        { 28, HUFFMAN_EMIT, 134},
        { 25, HUFFMAN_EMIT, 136},
        { 26, HUFFMAN_EMIT, 136},
        { 27, HUFFMAN_EMIT, 136},
        { 28, HUFFMAN_EMIT, 136},
        { 25, HUFFMAN_EMIT, 146},
        { 26, HUFFMAN_EMIT, 146},
        { 27, HUFFMAN_EMIT, 146},
        { 28, HUFFMAN_EMIT, 146}
    },
    /* state 106 */ {
        { 25, HUFFMAN_EMIT, 154},
        { 26, HUFFMAN_EMIT, 154},
        { 27, HUFFMAN_EMIT, 154},
        { 28, HUFFMAN_EMIT, 154},
        { 25, HUFFMAN_EMIT, 156},
        { 26, HUFFMAN_EMIT, 156},
        { 27, HUFFMAN_EMIT, 156},
        { 28, HUFFMAN_EMIT, 156},
        { 25, HUFFMAN_EMIT, 160},
        { 26, HUFFMAN_EMIT, 160},
        { 27, HUFFMAN_EMIT, 160},
        { 28, HUFFMAN_EMIT, 160},
        { 25, HUFFMAN_EMIT, 163},
        { 26, HUFFMAN_EMIT, 163},
        { 27, HUFFMAN_EMIT, 163},
        { 28, HUFFMAN_EMIT, 163}
    },
    /* state 107 */ {
        { 25, HUFFMAN_EMIT, 164},
        { 26, HUFFMAN_EMIT, 164},
        { 27, HUFFMAN_EMIT, 164},
        { 28, HUFFMAN_EMIT, 164},
        { 25, HUFFMAN_EMIT, 169},
        { 26, HUFFMAN_EMIT, 169},
        { 27, HUFFMAN_EMIT, 169},
        { 28, HUFFMAN_EMIT, 169},
        { 25, HUFFMAN_EMIT, 170},
        { 26, HUFFMAN_EMIT, 170},
        { 27, HUFFMAN_EMIT, 170},
        { 28, HUFFMAN_EMIT, 170},
        { 25, HUFFMAN_EMIT, 173},
        { 26, HUFFMAN_EMIT, 173},
        { 27, HUFFMAN_EMIT, 173},
        { 28, HUFFMAN_EMIT, 173}
    },
    /* state 108 */ {
        { 25, HUFFMAN_EMIT, 178},
        { 26, HUFFMAN_EMIT, 178},
        { 27, HUFFMAN_EMIT, 178},
        { 28, HUFFMAN_EMIT, 178},
        { 25, HUFFMAN_EMIT, 181},
        { 26, HUFFMAN_EMIT, 181},
        { 27, HUFFMAN_EMIT, 181},
        { 28, HUFFMAN_EMIT, 181},
        { 25, HUFFMAN_EMIT, 185},
        { 26, HUFFMAN_EMIT, 185},
        { 27, HUFFMAN_EMIT, 185},
        { 28, HUFFMAN_EMIT, 185},
        { 25, HUFFMAN_EMIT, 186},
        { 26, HUFFMAN_EMIT, 186},
        { 27, HUFFMAN_EMIT, 186},
        { 28, HUFFMAN_EMIT, 186}
    },
    /* state 109 */ {
        { 25, HUFFMAN_EMIT, 187},
        { 26, HUFFMAN_EMIT, 187},
        { 27, HUFFMAN_EMIT, 187},
        { 28, HUFFMAN_EMIT, 187},
        { 25, HUFFMAN_EMIT, 189},
        { 26, HUFFMAN_EMIT, 189},
        { 27, HUFFMAN_EMIT, 189},
        { 28, HUFFMAN_EMIT, 189},
        { 25, HUFFMAN_EMIT, 190},
        { 26, HUFFMAN_EMIT, 190},
        { 27, HUFFMAN_EMIT, 190},
        { 28, HUFFMAN_EMIT, 190},
        { 25, HUFFMAN_EMIT, 196},
        { 26, HUFFMAN_EMIT, 196},
        { 27, HUFFMAN_EMIT, 196},
        { 28, HUFFMAN_EMIT, 196}
    },
    /* state 110 */ {
        { 25, HUFFMAN_EMIT, 198},
        { 26, HUFFMAN_EMIT, 198},
        { 27, HUFFMAN_EMIT, 198},
        { 28, HUFFMAN_EMIT, 198},
        { 25, HUFFMAN_EMIT, 228},
        { 26, HUFFMAN_EMIT, 228},
        { 27, HUFFMAN_EMIT, 228},
        { 28, HUFFMAN_EMIT, 228},
        { 25, HUFFMAN_EMIT, 232},
        { 26, HUFFMAN_EMIT, 232},
        { 27, HUFFMAN_EMIT, 232},
        { 28, HUFFMAN_EMIT, 232},
        { 25, HUFFMAN_EMIT, 233},
        { 26, HUFFMAN_EMIT, 233},
        { 27, HUFFMAN_EMIT, 233},
        { 28, HUFFMAN_EMIT, 233}
    },
    /* state 111 */ {
        { 29, HUFFMAN_EMIT,   1},
        { 30, HUFFMAN_EMIT,   1},
        { 29, HUFFMAN_EMIT, 135},
        { 30, HUFFMAN_EMIT, 135},
        { 29, HUFFMAN_EMIT, 137},
        { 30, HUFFMAN_EMIT, 137},
        { 29, HUFFMAN_EMIT, 138},
        { 30, HUFFMAN_EMIT, 138},
        { 29, HUFFMAN_EMIT, 139},
        { 30, HUFFMAN_EMIT, 139},
        { 29, HUFFMAN_EMIT, 140},
        { 30, HUFFMAN_EMIT, 140},
        { 29, HUFFMAN_EMIT, 141},
        { 30, HUFFMAN_EMIT, 141},
        { 29, HUFFMAN_EMIT, 143},
        { 30, HUFFMAN_EMIT, 143}
    },
    /* state 112 */ {
        { 29, HUFFMAN_EMIT, 147},
        { 30, HUFFMAN_EMIT, 147},
        { 29, HUFFMAN_EMIT, 149},
        { 30, HUFFMAN_EMIT, 149},
        { 29, HUFFMAN_EMIT, 150},
        { 30, HUFFMAN_EMIT, 150},
        { 29, HUFFMAN_EMIT, 151},
        { 30, HUFFMAN_EMIT, 151},
        { 29, HUFFMAN_EMIT, 152},
        { 30, HUFFMAN_EMIT, 152},
        { 29, HUFFMAN_EMIT, 155},
        { 30, HUFFMAN_EMIT, 155},
        { 29, HUFFMAN_EMIT, 157},
        { 30, HUFFMAN_EMIT, 157},
        { 29, HUFFMAN_EMIT, 158},
        { 30, HUFFMAN_EMIT, 158}
    },
    /* state 113 */ {
        { 29, HUFFMAN_EMIT, 165},
        { 30, HUFFMAN_EMIT, 165},
        { 29, HUFFMAN_EMIT, 166},
        { 30, HUFFMAN_EMIT, 166},
        { 29, HUFFMAN_EMIT, 168},
        { 30, HUFFMAN_EMIT, 168},
        { 29, HUFFMAN_EMIT, 174},
        { 30, HUFFMAN_EMIT, 174},
        { 29, HUFFMAN_EMIT, 175},
        { 30, HUFFMAN_EMIT, 175},
        { 29, HUFFMAN_EMIT, 180},
        { 30, HUFFMAN_EMIT, 180},
        { 29, HUFFMAN_EMIT, 182},
        { 30, HUFFMAN_EMIT, 182},
        { 29, HUFFMAN_EMIT, 183},
        { 30, HUFFMAN_EMIT, 183}
    },
    /* state 114 */ {
        { 29, HUFFMAN_EMIT, 188},
        { 30, HUFFMAN_EMIT, 188},
        { 29, HUFFMAN_EMIT, 191},
        { 30, HUFFMAN_EMIT, 191},
        { 29, HUFFMAN_EMIT, 197},
        { 30, HUFFMAN_EMIT, 197},
        { 29, HUFFMAN_EMIT, 231},
        { 30, HUFFMAN_EMIT, 231},
        { 29, HUFFMAN_EMIT, 239},
        { 30, HUFFMAN_EMIT, 239},
        {  0, HUFFMAN_EMIT,   9},
        {  0, HUFFMAN_EMIT, 142},
        {  0, HUFFMAN_EMIT, 144},
        {  0, HUFFMAN_EMIT, 145},
        {  0, HUFFMAN_EMIT, 148},
        {  0, HUFFMAN_EMIT, 159}
    },
    /* state 115 */ {
        {  0, HUFFMAN_EMIT, 171},
        {  0, HUFFMAN_EMIT, 206},
        {  0, HUFFMAN_EMIT, 215},
        {  0, HUFFMAN_EMIT, 225},
        {  0, HUFFMAN_EMIT, 236},
        {  0, HUFFMAN_EMIT, 237},
        {141, HUFFMAN_NONE,   0},
        {142, HUFFMAN_NONE,   0},
        {143, HUFFMAN_NONE,   0},
        {144, HUFFMAN_NONE,   0},
        {145, HUFFMAN_NONE,   0},
        {146, HUFFMAN_NONE,   0},
        {147, HUFFMAN_NONE,   0},
        {148, HUFFMAN_NONE,   0},
        {149, HUFFMAN_NONE,   0},
        {150, HUFFMAN_NONE,   0}
    },
    /* state 116 */ {
        { 17, HUFFMAN_EMIT, 128},
        { 18, HUFFMAN_EMIT, 128},
        { 19, HUFFMAN_EMIT, 128},
        { 20, HUFFMAN_EMIT, 128},
        { 21, HUFFMAN_EMIT, 128},
        { 22, HUFFMAN_EMIT, 128},
        { 23, HUFFMAN_EMIT, 128},
        { 24, HUFFMAN_EMIT, 128},
        { 17, HUFFMAN_EMIT, 130},
        { 18, HUFFMAN_EMIT, 130},
        { 19, HUFFMAN_EMIT, 130},
        { 20, HUFFMAN_EMIT, 130},
        { 21, HUFFMAN_EMIT, 130},
        { 22, HUFFMAN_EMIT, 130},
        { 23, HUFFMAN_EMIT, 130},
        { 24, HUFFMAN_EMIT, 130}
    },
    /* state 117 */ {
        { 17, HUFFMAN_EMIT, 131},
        { 18, HUFFMAN_EMIT, 131},
        { 19, HUFFMAN_EMIT, 131},
        { 20, HUFFMAN_EMIT, 131},
        { 21, HUFFMAN_EMIT, 131},
        { 22, HUFFMAN_EMIT, 131},
        { 23, HUFFMAN_EMIT, 131},
        { 24, HUFFMAN_EMIT, 131},
        { 17, HUFFMAN_EMIT, 162},
        { 18, HUFFMAN_EMIT, 162},
        { 19, HUFFMAN_EMIT, 162},
        { 20, HUFFMAN_EMIT, 162},
        { 21, HUFFMAN_EMIT, 162},
        { 22, HUFFMAN_EMIT, 162},
        { 23, HUFFMAN_EMIT, 162},
        { 24, HUFFMAN_EMIT, 162}
    },
    /* state 118 */ {
        { 17, HUFFMAN_EMIT, 184},
        { 18, HUFFMAN_EMIT, 184},
        { 19, HUFFMAN_EMIT, 184},
        { 20, HUFFMAN_EMIT, 184},
        { 21, HUFFMAN_EMIT, 184},
        { 22, HUFFMAN_EMIT, 184},
        { 23, HUFFMAN_EMIT, 184},
        { 24, HUFFMAN_EMIT, 184},
        { 17, HUFFMAN_EMIT, 194},
        { 18, HUFFMAN_EMIT, 194},
        { 19, HUFFMAN_EMIT, 194},
        { 20, HUFFMAN_EMIT, 194},
        { 21, HUFFMAN_EMIT, 194},
        { 22, HUFFMAN_EMIT, 194},
        { 23, HUFFMAN_EMIT, 194},
        { 24, HUFFMAN_EMIT, 194}
    },
    /* state 119 */ {
        { 17, HUFFMAN_EMIT, 224},
        { 18, HUFFMAN_EMIT, 224},
        { 19, HUFFMAN_EMIT, 224},
        { 20, HUFFMAN_EMIT, 224},
        { 21, HUFFMAN_EMIT, 224},
        { 22, HUFFMAN_EMIT, 224},
        { 23, HUFFMAN_EMIT, 224},
        { 24, HUFFMAN_EMIT, 224},
        { 17, HUFFMAN_EMIT, 226},
        { 18, HUFFMAN_EMIT, 226},
        { 19, HUFFMAN_EMIT, 226},
        { 20, HUFFMAN_EMIT, 226},
        { 21, HUFFMAN_EMIT, 226},
        { 22, HUFFMAN_EMIT, 226},
        { 23, HUFFMAN_EMIT, 226},
        { 24, HUFFMAN_EMIT, 226}
    },
    /* state 120 */ {
        { 25, HUFFMAN_EMIT, 153},
        { 26, HUFFMAN_EMIT, 153},
        { 27, HUFFMAN_EMIT, 153},
        { 28, HUFFMAN_EMIT, 153},
        { 25, HUFFMAN_EMIT, 161},
        { 26, HUFFMAN_EMIT, 161},
        { 27, HUFFMAN_EMIT, 161},
        { 28, HUFFMAN_EMIT, 161},
        { 25, HUFFMAN_EMIT, 167},
        { 26, HUFFMAN_EMIT, 167},
        { 27, HUFFMAN_EMIT, 167},
        { 28, HUFFMAN_EMIT, 167},
        { 25, HUFFMAN_EMIT, 172},
        { 26, HUFFMAN_EMIT, 172},
        { 27, HUFFMAN_EMIT, 172},
        { 28, HUFFMAN_EMIT, 172}
    },
    /* state 121 */ {
        { 25, HUFFMAN_EMIT, 176},
        { 26, HUFFMAN_EMIT, 176},
        { 27, HUFFMAN_EMIT, 176},
        { 28, HUFFMAN_EMIT, 176},
        { 25, HUFFMAN_EMIT, 177},
        { 26, HUFFMAN_EMIT, 177},
        { 27, HUFFMAN_EMIT, 177},
        { 28, HUFFMAN_EMIT, 177},
        { 25, HUFFMAN_EMIT, 179},
        { 26, HUFFMAN_EMIT, 179},
        { 27, HUFFMAN_EMIT, 179},
        { 28, HUFFMAN_EMIT, 179},
        { 25, HUFFMAN_EMIT, 209},
        { 26, HUFFMAN_EMIT, 209},
        { 27, HUFFMAN_EMIT, 209},
        { 28, HUFFMAN_EMIT, 209}
    },
    /* state 122 */ {
        { 25, HUFFMAN_EMIT, 216},
        { 26, HUFFMAN_EMIT, 216},
        { 27, HUFFMAN_EMIT, 216},
        { 28, HUFFMAN_EMIT, 216},
        { 25, HUFFMAN_EMIT, 217},
        { 26, HUFFMAN_EMIT, 217},
        { 27, HUFFMAN_EMIT, 217},
        { 28, HUFFMAN_EMIT, 217},
        { 25, HUFFMAN_EMIT, 227},
        { 26, HUFFMAN_EMIT, 227},
        { 27, HUFFMAN_EMIT, 227},
        { 28, HUFFMAN_EMIT, 227},
        { 25, HUFFMAN_EMIT, 229},
        { 26, HUFFMAN_EMIT, 229},
        { 27, HUFFMAN_EMIT, 229},
        { 28, HUFFMAN_EMIT, 229}
    },
    /* state 123 */ {
        { 25, HUFFMAN_EMIT, 230},
        { 26, HUFFMAN_EMIT, 230},
        { 27, HUFFMAN_EMIT, 230},
        { 28, HUFFMAN_EMIT, 230},
        { 29, HUFFMAN_EMIT, 129},
        { 30, HUFFMAN_EMIT, 129},
        { 29, HUFFMAN_EMIT, 132},
        { 30, HUFFMAN_EMIT, 132},
        { 29, HUFFMAN_EMIT, 133},
        { 30, HUFFMAN_EMIT, 133},
        { 29, HUFFMAN_EMIT, 134},
        { 30, HUFFMAN_EMIT, 134},
        { 29, HUFFMAN_EMIT, 136},
        { 30, HUFFMAN_EMIT, 136},
        { 29, HUFFMAN_EMIT, 146},
        { 30, HUFFMAN_EMIT, 146}
    },
    /* state 124 */ {
        { 29, HUFFMAN_EMIT, 154},
        { 30, HUFFMAN_EMIT, 154},
        { 29, HUFFMAN_EMIT, 156},
        { 30, HUFFMAN_EMIT, 156},
        { 29, HUFFMAN_EMIT, 160},
        { 30, HUFFMAN_EMIT, 160},
        { 29, HUFFMAN_EMIT, 163},
        { 30, HUFFMAN_EMIT, 163},
        { 29, HUFFMAN_EMIT, 164},
        { 30, HUFFMAN_EMIT, 164},
        { 29, HUFFMAN_EMIT, 169},
        { 30, HUFFMAN_EMIT, 169},
        { 29, HUFFMAN_EMIT, 170},
        { 30, HUFFMAN_EMIT, 170},
        { 29, HUFFMAN_EMIT, 173},
        { 30, HUFFMAN_EMIT, 173}
    },
    /* state 125 */ {
        { 29, HUFFMAN_EMIT, 178},
        { 30, HUFFMAN_EMIT, 178},
        { 29, HUFFMAN_EMIT, 181},
        { 30, HUFFMAN_EMIT, 181},
        { 29, HUFFMAN_EMIT, 185},
        { 30, HUFFMAN_EMIT, 185},
        { 29, HUFFMAN_EMIT, 186},
        { 30, HUFFMAN_EMIT, 186},
        { 29, HUFFMAN_EMIT, 187},
        { 30, HUFFMAN_EMIT, 187},
        { 29, HUFFMAN_EMIT, 189},
        { 30, HUFFMAN_EMIT, 189},
        { 29, HUFFMAN_EMIT, 190},
        { 30, HUFFMAN_EMIT, 190},
        { 29, HUFFMAN_EMIT, 196},
        { 30, HUFFMAN_EMIT, 196}
    },
    /* state 126 */ {
        { 29, HUFFMAN_EMIT, 198},
        { 30, HUFFMAN_EMIT, 198},
        { 29, HUFFMAN_EMIT, 228},
        { 30, HUFFMAN_EMIT, 228},
        { 29, HUFFMAN_EMIT, 232},
        { 30, HUFFMAN_EMIT, 232},
        { 29, HUFFMAN_EMIT, 233},
        { 30, HUFFMAN_EMIT, 233},
        {  0, HUFFMAN_EMIT,   1},
        {  0, HUFFMAN_EMIT, 135},
        {  0, HUFFMAN_EMIT, 137},
        {  0, HUFFMAN_EMIT, 138},
        {  0, HUFFMAN_EMIT, 139},
        {  0, HUFFMAN_EMIT, 140},
        {  0, HUFFMAN_EMIT, 141},
        {  0, HUFFMAN_EMIT, 143}
    },
    /* state 127 */ {
        {  0, HUFFMAN_EMIT, 147},
        {  0, HUFFMAN_EMIT, 149},
        {  0, HUFFMAN_EMIT, 150},
        {  0, HUFFMAN_EMIT, 151},
        {  0, HUFFMAN_EMIT, 152},
        {  0, HUFFMAN_EMIT, 155},
        {  0, HUFFMAN_EMIT, 157},
        {  0, HUFFMAN_EMIT, 158},
        {  0, HUFFMAN_EMIT, 165},
        {  0, HUFFMAN_EMIT, 166},
        {  0, HUFFMAN_EMIT, 168},
        {  0, HUFFMAN_EMIT, 174},
        {  0, HUFFMAN_EMIT, 175},
        {  0, HUFFMAN_EMIT, 180},
        {  0, HUFFMAN_EMIT, 182},
        {  0, HUFFMAN_EMIT, 183}
    },
    /* state 128 */ {
        {  0, HUFFMAN_EMIT, 188},
        {  0, HUFFMAN_EMIT, 191},
        {  0, HUFFMAN_EMIT, 197},
        {  0, HUFFMAN_EMIT, 231},
        {  0, HUFFMAN_EMIT, 239},
        {151, HUFFMAN_NONE,   0},
        {152, HUFFMAN_NONE,   0},
        {153, HUFFMAN_NONE,   0},
        {154, HUFFMAN_NONE,   0},
        {155, HUFFMAN_NONE,   0},
        {156, HUFFMAN_NONE,   0},
        {157, HUFFMAN_NONE,   0},
        {158, HUFFMAN_NONE,   0},
        {159, HUFFMAN_NONE,   0},
        {160, HUFFMAN_NONE,   0},
        {161, HUFFMAN_NONE,   0}
    },
    /* state 129 */ {
        { 17, HUFFMAN_EMIT,  92},
        { 18, HUFFMAN_EMIT,  92},
        { 19, HUFFMAN_EMIT,  92},
        { 20, HUFFMAN_EMIT,  92},
        { 21, HUFFMAN_EMIT,  92},
        { 22, HUFFMAN_EMIT,  92},
        { 23, HUFFMAN_EMIT,  92},
        { 24, HUFFMAN_EMIT,  92},
        { 17, HUFFMAN_EMIT, 195},
        { 18, HUFFMAN_EMIT, 195},
        { 19, HUFFMAN_EMIT, 195},
        { 20, HUFFMAN_EMIT, 195},
        { 21, HUFFMAN_EMIT, 195},
        { 22, HUFFMAN_EMIT, 195},
        { 23, HUFFMAN_EMIT, 195},
        { 24, HUFFMAN_EMIT, 195}
    },
    /* state 130 */ {
        { 17, HUFFMAN_EMIT, 208},
        { 18, HUFFMAN_EMIT, 208},
        { 19, HUFFMAN_EMIT, 208},
        { 20, HUFFMAN_EMIT, 208},
        { 21, HUFFMAN_EMIT, 208},
        { 22, HUFFMAN_EMIT, 208},
        { 23, HUFFMAN_EMIT, 208},
        { 24, HUFFMAN_EMIT, 208},
        { 25, HUFFMAN_EMIT, 128},
        { 26, HUFFMAN_EMIT, 128},
        { 27, HUFFMAN_EMIT, 128},
        { 28, HUFFMAN_EMIT, 128},
        { 25, HUFFMAN_EMIT, 130},
        { 26, HUFFMAN_EMIT, 130},
        { 27, HUFFMAN_EMIT, 130},
        { 28, HUFFMAN_EMIT, 130}
    },
    /* state 131 */ {
        { 25, HUFFMAN_EMIT, 131},
        { 26, HUFFMAN_EMIT, 131},
        { 27, HUFFMAN_EMIT, 131},
        { 28, HUFFMAN_EMIT, 131},
        { 25, HUFFMAN_EMIT, 162},
        { 26, HUFFMAN_EMIT, 162},
        { 27, HUFFMAN_EMIT, 162},
        { 28, HUFFMAN_EMIT, 162},
        { 25, HUFFMAN_EMIT, 184},
        { 26, HUFFMAN_EMIT, 184},
        { 27, HUFFMAN_EMIT, 184},
        { 28, HUFFMAN_EMIT, 184},
        { 25, HUFFMAN_EMIT, 194},
        { 26, HUFFMAN_EMIT, 194},
        { 27, HUFFMAN_EMIT, 194},
        { 28, HUFFMAN_EMIT, 194}
    },
    /* state 132 */ {
        { 25, HUFFMAN_EMIT, 224},
        { 26, HUFFMAN_EMIT, 224},
        { 27, HUFFMAN_EMIT, 224},
        { 28, HUFFMAN_EMIT, 224},
        { 25, HUFFMAN_EMIT, 226},
        { 26, HUFFMAN_EMIT, 226},
        { 27, HUFFMAN_EMIT, 226},
        { 28, HUFFMAN_EMIT, 226},
        { 29, HUFFMAN_EMIT, 153},
        { 30, HUFFMAN_EMIT, 153},
        { 29, HUFFMAN_EMIT, 161},
        { 30, HUFFMAN_EMIT, 161},
        { 29, HUFFMAN_EMIT, 167},
        { 30, HUFFMAN_EMIT, 167},
        { 29, HUFFMAN_EMIT, 172},
        { 30, HUFFMAN_EMIT, 172}
    },
    /* state 133 */ {
        { 29, HUFFMAN_EMIT, 176},
        { 30, HUFFMAN_EMIT, 176},
        { 29, HUFFMAN_EMIT, 177},
        { 30, HUFFMAN_EMIT, 177},
        { 29, HUFFMAN_EMIT, 179},
        { 30, HUFFMAN_EMIT, 179},
        { 29, HUFFMAN_EMIT, 209},
        { 30, HUFFMAN_EMIT, 209},
        { 29, HUFFMAN_EMIT, 216},
        { 30, HUFFMAN_EMIT, 216},
        { 29, HUFFMAN_EMIT, 217},
        { 30, HUFFMAN_EMIT, 217},
        { 29, HUFFMAN_EMIT, 227},
        { 30, HUFFMAN_EMIT, 227},
        { 29, HUFFMAN_EMIT, 229},
        { 30, HUFFMAN_EMIT, 229}
    },
    /* state 134 */ {
        { 29, HUFFMAN_EMIT, 230},
        { 30, HUFFMAN_EMIT, 230},
        {  0, HUFFMAN_EMIT, 129},
        {  0, HUFFMAN_EMIT, 132},
        {  0, HUFFMAN_EMIT, 133},
        {  0, HUFFMAN_EMIT, 134},
        {  0, HUFFMAN_EMIT, 136},
        {  0, HUFFMAN_EMIT, 146},
        {  0, HUFFMAN_EMIT, 154},
        {  0, HUFFMAN_EMIT, 156},
        {  0, HUFFMAN_EMIT, 160},
        {  0, HUFFMAN_EMIT, 163},
        {  0, HUFFMAN_EMIT, 164},
        {  0, HUFFMAN_EMIT, 169},
        {  0, HUFFMAN_EMIT, 170},
        {  0, HUFFMAN_EMIT, 173}
    },
    /* state 135 */ {
        {  0, HUFFMAN_EMIT, 178},
        {  0, HUFFMAN_EMIT, 181},
        {  0, HUFFMAN_EMIT, 185},
        {  0, HUFFMAN_EMIT, 186},
        {  0, HUFFMAN_EMIT, 187},
        {  0, HUFFMAN_EMIT, 189},
        {  0, HUFFMAN_EMIT, 190},
        {  0, HUFFMAN_EMIT, 196},
        {  0, HUFFMAN_EMIT, 198},
        {  0, HUFFMAN_EMIT, 228},
        {  0, HUFFMAN_EMIT, 232},
        {  0, HUFFMAN_EMIT, 233},
        {162, HUFFMAN_NONE,   0},
        {163, HUFFMAN_NONE,   0},
        {164, HUFFMAN_NONE,   0},
        {165, HUFFMAN_NONE,   0}
    },
    /* state 136 */ {
        {166, HUFFMAN_NONE,   0},
        {167, HUFFMAN_NONE,   0},
        {168, HUFFMAN_NONE,   0},
        {169, HUFFMAN_NONE,   0},
        {170, HUFFMAN_NONE,   0},
        {171, HUFFMAN_NONE,   0},
        {172, HUFFMAN_NONE,   0},
        {173, HUFFMAN_NONE,   0},
        {174, HUFFMAN_NONE,   0},
        {175, HUFFMAN_NONE,   0},
        {176, HUFFMAN_NONE,   0},
        {177, HUFFMAN_NONE,   0},
        {178, HUFFMAN_NONE,   0},
        {179, HUFFMAN_NONE,   0},
        {180, HUFFMAN_NONE,   0},
        {181, HUFFMAN_NONE,   0}
    },
    /* state 137 */ {
        { 25, HUFFMAN_EMIT,  92},
        { 26, HUFFMAN_EMIT,  92},
        { 27, HUFFMAN_EMIT,  92},
        { 28, HUFFMAN_EMIT,  92},
        { 25, HUFFMAN_EMIT, 195},
        { 26, HUFFMAN_EMIT, 195},
        { 27, HUFFMAN_EMIT, 195},
        { 28, HUFFMAN_EMIT, 195},
        { 25, HUFFMAN_EMIT, 208},
        { 26, HUFFMAN_EMIT, 208},
        { 27, HUFFMAN_EMIT, 208},
        { 28, HUFFMAN_EMIT, 208},
        { 29, HUFFMAN_EMIT, 128},
        { 30, HUFFMAN_EMIT, 128},
        { 29, HUFFMAN_EMIT, 130},
        { 30, HUFFMAN_EMIT, 130}
    },
    /* state 138 */ {
        { 29, HUFFMAN_EMIT, 131},
        { 30, HUFFMAN_EMIT, 131},
        { 29, HUFFMAN_EMIT, 162},
        { 30, HUFFMAN_EMIT, 162},
        { 29, HUFFMAN_EMIT, 184},
        { 30, HUFFMAN_EMIT, 184},
        { 29, HUFFMAN_EMIT, 194},
        { 30, HUFFMAN_EMIT, 194},
        { 29, HUFFMAN_EMIT, 224},
        { 30, HUFFMAN_EMIT, 224},
        { 29, HUFFMAN_EMIT, 226},
        { 30, HUFFMAN_EMIT, 226},
        {  0, HUFFMAN_EMIT, 153},
        {  0, HUFFMAN_EMIT, 161},
        {  0, HUFFMAN_EMIT, 167},
        {  0, HUFFMAN_EMIT, 172}
    },
    /* state 139 */ {
        {  0, HUFFMAN_EMIT, 176},
        {  0, HUFFMAN_EMIT, 177},
        {  0, HUFFMAN_EMIT, 179},
        {  0, HUFFMAN_EMIT, 209},
        {  0, HUFFMAN_EMIT, 216},
        {  0, HUFFMAN_EMIT, 217},
        {  0, HUFFMAN_EMIT, 227},
        {  0, HUFFMAN_EMIT, 229},
        {  0, HUFFMAN_EMIT, 230},
        {182, HUFFMAN_NONE,   0},
        {183, HUFFMAN_NONE,   0},
        {184, HUFFMAN_NONE,   0},
        {185, HUFFMAN_NONE,   0},
        {186, HUFFMAN_NONE,   0},
        {187, HUFFMAN_NONE,   0},
        {188, HUFFMAN_NONE,   0}
    },
    /* state 140 */ {
        {189, HUFFMAN_NONE,   0},
        {190, HUFFMAN_NONE,   0},
        {191, HUFFMAN_NONE,   0},
        {192, HUFFMAN_NONE,   0},
        {193, HUFFMAN_NONE,   0},
        {194, HUFFMAN_NONE,   0},
        {195, HUFFMAN_NONE,   0},
        {196, HUFFMAN_NONE,   0},
        {197, HUFFMAN_NONE,   0},
        {198, HUFFMAN_NONE,   0},
        {199, HUFFMAN_NONE,   0},
        {200, HUFFMAN_NONE,   0},
        {201, HUFFMAN_NONE,   0},
        {202, HUFFMAN_NONE,   0},
        {203, HUFFMAN_NONE,   0},
        {204, HUFFMAN_NONE,   0}
    },
    /* state 141 */ {
        { 17, HUFFMAN_EMIT, 199},
        { 18, HUFFMAN_EMIT, 199},
        { 19, HUFFMAN_EMIT, 199},
        { 20, HUFFMAN_EMIT, 199},
        { 21, HUFFMAN_EMIT, 199},
        { 22, HUFFMAN_EMIT, 199},
        { 23, HUFFMAN_EMIT, 199},
        { 24, HUFFMAN_EMIT, 199},
        { 17, HUFFMAN_EMIT, 207},
        { 18, HUFFMAN_EMIT, 207},
        { 19, HUFFMAN_EMIT, 207},
        { 20, HUFFMAN_EMIT, 207},
        { 21, HUFFMAN_EMIT, 207},
        { 22, HUFFMAN_EMIT, 207},
        { 23, HUFFMAN_EMIT, 207},
        { 24, HUFFMAN_EMIT, 207}
    },
    /* state 142 */ {
        { 17, HUFFMAN_EMIT, 234},
        { 18, HUFFMAN_EMIT, 234},
        { 19, HUFFMAN_EMIT, 234},
        { 20, HUFFMAN_EMIT, 234},
        { 21, HUFFMAN_EMIT, 234},
        { 22, HUFFMAN_EMIT, 234},
        { 23, HUFFMAN_EMIT, 234},
        { 24, HUFFMAN_EMIT, 234},
        { 17, HUFFMAN_EMIT, 235},
        { 18, HUFFMAN_EMIT, 235},
        { 19, HUFFMAN_EMIT, 235},
        { 20, HUFFMAN_EMIT, 235},
        { 21, HUFFMAN_EMIT, 235},
        { 22, HUFFMAN_EMIT, 235},
        { 23, HUFFMAN_EMIT, 235},
        { 24, HUFFMAN_EMIT, 235}
    },
    /* state 143 */ {
        { 25, HUFFMAN_EMIT, 192},
        { 26, HUFFMAN_EMIT, 192},
        { 27, HUFFMAN_EMIT, 192},
        { 28, HUFFMAN_EMIT, 192},
        { 25, HUFFMAN_EMIT, 193},
        { 26, HUFFMAN_EMIT, 193},
        { 27, HUFFMAN_EMIT, 193},
        { 28, HUFFMAN_EMIT, 193},
        { 25, HUFFMAN_EMIT, 200},
        { 26, HUFFMAN_EMIT, 200},
        { 27, HUFFMAN_EMIT, 200},
        { 28, HUFFMAN_EMIT, 200},
        { 25, HUFFMAN_EMIT, 201},
        { 26, HUFFMAN_EMIT, 201},
        { 27, HUFFMAN_EMIT, 201},
        { 28, HUFFMAN_EMIT, 201}
    },
    /* state 144 */ {
        { 25, HUFFMAN_EMIT, 202},
        { 26, HUFFMAN_EMIT, 202},
        { 27, HUFFMAN_EMIT, 202},
        { 28, HUFFMAN_EMIT, 202},
        { 25, HUFFMAN_EMIT, 205},
        { 26, HUFFMAN_EMIT, 205},
        { 27, HUFFMAN_EMIT, 205},
        { 28, HUFFMAN_EMIT, 205},
        { 25, HUFFMAN_EMIT, 210},
        { 26, HUFFMAN_EMIT, 210},
        { 27, HUFFMAN_EMIT, 210},
        { 28, HUFFMAN_EMIT, 210},
        { 25, HUFFMAN_EMIT, 213},
        { 26, HUFFMAN_EMIT, 213},
        { 27, HUFFMAN_EMIT, 213},
        { 28, HUFFMAN_EMIT, 213}
    },
    /* state 145 */ {
        { 25, HUFFMAN_EMIT, 218},
        { 26, HUFFMAN_EMIT, 218},
        { 27, HUFFMAN_EMIT, 218},
        { 28, HUFFMAN_EMIT, 218},
        { 25, HUFFMAN_EMIT, 219},
        { 26, HUFFMAN_EMIT, 219},
        { 27, HUFFMAN_EMIT, 219},
        { 28, HUFFMAN_EMIT, 219},
        { 25, HUFFMAN_EMIT, 238},
        { 26, HUFFMAN_EMIT, 238},
        { 27, HUFFMAN_EMIT, 238},
        { 28, HUFFMAN_EMIT, 238},
        { 25, HUFFMAN_EMIT, 240},
        { 26, HUFFMAN_EMIT, 240},
        { 27, HUFFMAN_EMIT, 240},
        { 28, HUFFMAN_EMIT, 240}
    },
    /* state 146 */ {
        { 25, HUFFMAN_EMIT, 242},
        { 26, HUFFMAN_EMIT, 242},
        { 27, HUFFMAN_EMIT, 242},
        { 28, HUFFMAN_EMIT, 242},
        { 25, HUFFMAN_EMIT, 243},
        { 26, HUFFMAN_EMIT, 243},
        { 27, HUFFMAN_EMIT, 243},
        { 28, HUFFMAN_EMIT, 243},
        { 25, HUFFMAN_EMIT, 255},
        { 26, HUFFMAN_EMIT, 255},
        { 27, HUFFMAN_EMIT, 255},
        { 28, HUFFMAN_EMIT, 255},
        { 29, HUFFMAN_EMIT, 203},
        { 30, HUFFMAN_EMIT, 203},
        { 29, HUFFMAN_EMIT, 204},
        { 30, HUFFMAN_EMIT, 204}
    },
    /* state 147 */ {
        { 29, HUFFMAN_EMIT, 211},
        { 30, HUFFMAN_EMIT, 211},
        { 29, HUFFMAN_EMIT, 212},
        { 30, HUFFMAN_EMIT, 212},
        { 29, HUFFMAN_EMIT, 214},
        { 30, HUFFMAN_EMIT, 214},
        { 29, HUFFMAN_EMIT, 221},
        { 30, HUFFMAN_EMIT, 221},
        { 29, HUFFMAN_EMIT, 222},
        { 30, HUFFMAN_EMIT, 222},
        { 29, HUFFMAN_EMIT, 223},
        { 30, HUFFMAN_EMIT, 223},
        { 29, HUFFMAN_EMIT, 241},
        { 30, HUFFMAN_EMIT, 241},
        { 29, HUFFMAN_EMIT, 244},
        { 30, HUFFMAN_EMIT, 244}
    },
    /* state 148 */ {
        { 29, HUFFMAN_EMIT, 245},
        { 30, HUFFMAN_EMIT, 245},
        { 29, HUFFMAN_EMIT, 246},
        { 30, HUFFMAN_EMIT, 246},
        { 29, HUFFMAN_EMIT, 247},
        { 30, HUFFMAN_EMIT, 247},
        { 29, HUFFMAN_EMIT, 248},
        { 30, HUFFMAN_EMIT, 248},
        { 29, HUFFMAN_EMIT, 250},
        { 30, HUFFMAN_EMIT, 250},
        { 29, HUFFMAN_EMIT, 251},
        { 30, HUFFMAN_EMIT, 251},
        { 29, HUFFMAN_EMIT, 252},
        { 30, HUFFMAN_EMIT, 252},
        { 29, HUFFMAN_EMIT, 253},
        { 30, HUFFMAN_EMIT, 253}
    },
    /* state 149 */ {
        { 29, HUFFMAN_EMIT, 254},
        { 30, HUFFMAN_EMIT, 254},
        {  0, HUFFMAN_EMIT,   2},
        {  0, HUFFMAN_EMIT,   3},
        {  0, HUFFMAN_EMIT,   4},
        {  0, HUFFMAN_EMIT,   5},
        {  0, HUFFMAN_EMIT,   6},
        {  0, HUFFMAN_EMIT,   7},
        {  0, HUFFMAN_EMIT,   8},
        {  0, HUFFMAN_EMIT,  11},
        {  0, HUFFMAN_EMIT,  12},
        {  0, HUFFMAN_EMIT,  14},
        {  0, HUFFMAN_EMIT,  15},
        {  0, HUFFMAN_EMIT,  16},
        {  0, HUFFMAN_EMIT,  17},
        {  0, HUFFMAN_EMIT,  18}
    },
    /* state 150 */ {
        {  0, HUFFMAN_EMIT,  19},
        {  0, HUFFMAN_EMIT,  20},
        {  0, HUFFMAN_EMIT,  21},
        {  0, HUFFMAN_EMIT,  23},
        {  0, HUFFMAN_EMIT,  24},
        {  0, HUFFMAN_EMIT,  25},
        {  0, HUFFMAN_EMIT,  26},
        {  0, HUFFMAN_EMIT,  27},
        {  0, HUFFMAN_EMIT,  28},
        {  0, HUFFMAN_EMIT,  29},
        {  0, HUFFMAN_EMIT,  30},
        {  0, HUFFMAN_EMIT,  31},
        {  0, HUFFMAN_EMIT, 127},
        {  0, HUFFMAN_EMIT, 220},
        {  0, HUFFMAN_EMIT, 249},
        {205, HUFFMAN_NONE,   0}
    },
    /* state 151 */ {
        { 17, HUFFMAN_EMIT,   9},
        { 18, HUFFMAN_EMIT,   9},
        { 19, HUFFMAN_EMIT,   9},
        { 20, HUFFMAN_EMIT,   9},
        { 21, HUFFMAN_EMIT,   9},
        { 22, HUFFMAN_EMIT,   9},
        { 23, HUFFMAN_EMIT,   9},
        { 24, HUFFMAN_EMIT,   9},
        { 17, HUFFMAN_EMIT, 142},
        { 18, HUFFMAN_EMIT, 142},
        { 19, HUFFMAN_EMIT, 142},
        { 20, HUFFMAN_EMIT, 142},
        { 21, HUFFMAN_EMIT, 142},
        { 22, HUFFMAN_EMIT, 142},
        { 23, HUFFMAN_EMIT, 142},
        { 24, HUFFMAN_EMIT, 142}
    },
    /* state 152 */ {
        { 17, HUFFMAN_EMIT, 144},
        { 18, HUFFMAN_EMIT, 144},
        { 19, HUFFMAN_EMIT, 144},
        { 20, HUFFMAN_EMIT, 144},
        { 21, HUFFMAN_EMIT, 144},
        { 22, HUFFMAN_EMIT, 144},
        { 23, HUFFMAN_EMIT, 144},
        { 24, HUFFMAN_EMIT, 144},
        { 17, HUFFMAN_EMIT, 145},
        { 18, HUFFMAN_EMIT, 145},
        { 19, HUFFMAN_EMIT, 145},
        { 20, HUFFMAN_EMIT, 145},
        { 21, HUFFMAN_EMIT, 145},
        { 22, HUFFMAN_EMIT, 145},
        { 23, HUFFMAN_EMIT, 145},
        { 24, HUFFMAN_EMIT, 145}
    },
    /* state 153 */ {
        { 17, HUFFMAN_EMIT, 148},
        { 18, HUFFMAN_EMIT, 148},
        { 19, HUFFMAN_EMIT, 148},
        { 20, HUFFMAN_EMIT, 148},
        { 21, HUFFMAN_EMIT, 148},
        { 22, HUFFMAN_EMIT, 148},
        { 23, HUFFMAN_EMIT, 148},
        { 24, HUFFMAN_EMIT, 148},
        { 17, HUFFMAN_EMIT, 159},
        { 18, HUFFMAN_EMIT, 159},
        { 19, HUFFMAN_EMIT, 159},
        { 20, HUFFMAN_EMIT, 159},
        { 21, HUFFMAN_EMIT, 159},
        { 22, HUFFMAN_EMIT, 159},
        { 23, HUFFMAN_EMIT, 159},
        { 24, HUFFMAN_EMIT, 159}
    },
    /* state 154 */ {
        { 17, HUFFMAN_EMIT, 171},
        { 18, HUFFMAN_EMIT, 171},
        { 19, HUFFMAN_EMIT, 171},
        { 20, HUFFMAN_EMIT, 171},
        { 21, HUFFMAN_EMIT, 171},
        { 22, HUFFMAN_EMIT, 171},
        { 23, HUFFMAN_EMIT, 171},
        { 24, HUFFMAN_EMIT, 171},
        { 17, HUFFMAN_EMIT, 206},
        { 18, HUFFMAN_EMIT, 206},
        { 19, HUFFMAN_EMIT, 206},
        { 20, HUFFMAN_EMIT, 206},
        { 21, HUFFMAN_EMIT, 206},
        { 22, HUFFMAN_EMIT, 206},
        { 23, HUFFMAN_EMIT, 206},
        { 24, HUFFMAN_EMIT, 206}
    },
    /* state 155 */ {
        { 17, HUFFMAN_EMIT, 215},
        { 18, HUFFMAN_EMIT, 215},
        { 19, HUFFMAN_EMIT, 215},
        { 20, HUFFMAN_EMIT, 215},
        { 21, HUFFMAN_EMIT, 215},
        { 22, HUFFMAN_EMIT, 215},
        { 23, HUFFMAN_EMIT, 215},
        { 24, HUFFMAN_EMIT, 215},
        { 17, HUFFMAN_EMIT, 225},
        { 18, HUFFMAN_EMIT, 225},
        { 19, HUFFMAN_EMIT, 225},
        { 20, HUFFMAN_EMIT, 225},
        { 21, HUFFMAN_EMIT, 225},
        { 22, HUFFMAN_EMIT, 225},
        { 23, HUFFMAN_EMIT, 225},
        { 24, HUFFMAN_EMIT, 225}
    },
    /* state 156 */ {
        { 17, HUFFMAN_EMIT, 236},
        { 18, HUFFMAN_EMIT, 236},
        { 19, HUFFMAN_EMIT, 236},
        { 20, HUFFMAN_EMIT, 236},
        { 21, HUFFMAN_EMIT, 236},
        { 22, HUFFMAN_EMIT, 236},
        { 23, HUFFMAN_EMIT, 236},
        { 24, HUFFMAN_EMIT, 236},
        { 17, HUFFMAN_EMIT, 237},
        { 18, HUFFMAN_EMIT, 237},
        { 19, HUFFMAN_EMIT, 237},
        { 20, HUFFMAN_EMIT, 237},
        { 21, HUFFMAN_EMIT, 237},
        { 22, HUFFMAN_EMIT, 237},
        { 23, HUFFMAN_EMIT, 237},
        { 24, HUFFMAN_EMIT, 237}
    },
    /* state 157 */ {
        { 25, HUFFMAN_EMIT, 199},
        { 26, HUFFMAN_EMIT, 199},
        { 27, HUFFMAN_EMIT, 199},
        { 28, HUFFMAN_EMIT, 199},
        { 25, HUFFMAN_EMIT, 207},
        { 26, HUFFMAN_EMIT, 207},
        { 27, HUFFMAN_EMIT, 207},
        { 28, HUFFMAN_EMIT, 207},
        { 25, HUFFMAN_EMIT, 234},
        { 26, HUFFMAN_EMIT, 234},
        { 27, HUFFMAN_EMIT, 234},
        { 28, HUFFMAN_EMIT, 234},
        { 25, HUFFMAN_EMIT, 235},
        { 26, HUFFMAN_EMIT, 235},
        { 27, HUFFMAN_EMIT, 235},
        { 28, HUFFMAN_EMIT, 235}
    },
    /* state 158 */ {
        { 29, HUFFMAN_EMIT, 192},
        { 30, HUFFMAN_EMIT, 192},
        { 29, HUFFMAN_EMIT, 193},
        { 30, HUFFMAN_EMIT, 193},
        { 29, HUFFMAN_EMIT, 200},
        { 30, HUFFMAN_EMIT, 200},
        { 29, HUFFMAN_EMIT, 201},
        { 30, HUFFMAN_EMIT, 201},
        { 29, HUFFMAN_EMIT, 202},
        { 30, HUFFMAN_EMIT, 202},
        { 29, HUFFMAN_EMIT, 205},
        { 30, HUFFMAN_EMIT, 205},
        { 29, HUFFMAN_EMIT, 210},
        { 30, HUFFMAN_EMIT, 210},
        { 29, HUFFMAN_EMIT, 213},
        { 30, HUFFMAN_EMIT, 213}
    },
    /* state 159 */ {
        { 29, HUFFMAN_EMIT, 218},
        { 30, HUFFMAN_EMIT, 218},
        { 29, HUFFMAN_EMIT, 219},
        { 30, HUFFMAN_EMIT, 219},
        { 29, HUFFMAN_EMIT, 238},
        { 30, HUFFMAN_EMIT, 238},
        { 29, HUFFMAN_EMIT, 240},
        { 30, HUFFMAN_EMIT, 240},
        { 29, HUFFMAN_EMIT, 242},
        { 30, HUFFMAN_EMIT, 242},
        { 29, HUFFMAN_EMIT, 243},
        { 30, HUFFMAN_EMIT, 243},
        { 29, HUFFMAN_EMIT, 255},
        { 30, HUFFMAN_EMIT, 255},
        {  0, HUFFMAN_EMIT, 203},
        {  0, HUFFMAN_EMIT, 204}
    },
    /* state 160 */ {
        {  0, HUFFMAN_EMIT, 211},
        {  0, HUFFMAN_EMIT, 212},
        {  0, HUFFMAN_EMIT, 214},
        {  0, HUFFMAN_EMIT, 221},
        {  0, HUFFMAN_EMIT, 222},
        {  0, HUFFMAN_EMIT, 223},
        {  0, HUFFMAN_EMIT, 241},
        {  0, HUFFMAN_EMIT, 244},
        {  0, HUFFMAN_EMIT, 245},
        {  0, HUFFMAN_EMIT, 246},
        {  0, HUFFMAN_EMIT, 247},
        {  0, HUFFMAN_EMIT, 248},
        {  0, HUFFMAN_EMIT, 250},
        {  0, HUFFMAN_EMIT, 251},
        {  0, HUFFMAN_EMIT, 252},
        {  0, HUFFMAN_EMIT, 253}
    },
    /* state 161 */ {
        {  0, HUFFMAN_EMIT, 254},
        {206, HUFFMAN_NONE,   0},
        {207, HUFFMAN_NONE,   0},
        {208, HUFFMAN_NONE,   0},
        {209, HUFFMAN_NONE,   0},
        {210, HUFFMAN_NONE,   0},
        {211, HUFFMAN_NONE,   0},
        {212, HUFFMAN_NONE,   0},
        {213, HUFFMAN_NONE,   0},
        {214, HUFFMAN_NONE,   0},
        {215, HUFFMAN_NONE,   0},
        {216, HUFFMAN_NONE,   0},
        {217, HUFFMAN_NONE,   0},
        {218, HUFFMAN_NONE,   0},
        {219, HUFFMAN_NONE,   0},
        {220, HUFFMAN_NONE,   0}
    },
    /* state 162 */ {
        { 17, HUFFMAN_EMIT,   1},
        { 18, HUFFMAN_EMIT,   1},
        { 19, HUFFMAN_EMIT,   1},
        { 20, HUFFMAN_EMIT,   1},
        { 21, HUFFMAN_EMIT,   1},
        { 22, HUFFMAN_EMIT,   1},
        { 23, HUFFMAN_EMIT,   1},
        { 24, HUFFMAN_EMIT,   1},
        { 17, HUFFMAN_EMIT, 135},
        { 18, HUFFMAN_EMIT, 135},
        { 19, HUFFMAN_EMIT, 135},
        { 20, HUFFMAN_EMIT, 135},
        { 21, HUFFMAN_EMIT, 135},
        { 22, HUFFMAN_EMIT, 135},
        { 23, HUFFMAN_EMIT, 135},
        { 24, HUFFMAN_EMIT, 135}
    },
    /* state 163 */ {
        { 17, HUFFMAN_EMIT, 137},
        { 18, HUFFMAN_EMIT, 137},
        { 19, HUFFMAN_EMIT, 137},
        { 20, HUFFMAN_EMIT, 137},
        { 21, HUFFMAN_EMIT, 137},
        { 22, HUFFMAN_EMIT, 137},
        { 23, HUFFMAN_EMIT, 137},
        { 24, HUFFMAN_EMIT, 137},
        { 17, HUFFMAN_EMIT, 138},
        { 18, HUFFMAN_EMIT, 138},
        { 19, HUFFMAN_EMIT, 138},
        { 20, HUFFMAN_EMIT, 138},
        { 21, HUFFMAN_EMIT, 138},
        { 22, HUFFMAN_EMIT, 138},
        { 23, HUFFMAN_EMIT, 138},
        { 24, HUFFMAN_EMIT, 138}
    },
    /* state 164 */ {
        { 17, HUFFMAN_EMIT, 139},
        { 18, HUFFMAN_EMIT, 139},
        { 19, HUFFMAN_EMIT, 139},
        { 20, HUFFMAN_EMIT, 139},
        { 21, HUFFMAN_EMIT, 139},
        { 22, HUFFMAN_EMIT, 139},
        { 23, HUFFMAN_EMIT, 139},
        { 24, HUFFMAN_EMIT, 139},
        { 17, HUFFMAN_EMIT, 140},
        { 18, HUFFMAN_EMIT, 140},
        { 19, HUFFMAN_EMIT, 140},
        { 20, HUFFMAN_EMIT, 140},
        { 21, HUFFMAN_EMIT, 140},
        { 22, HUFFMAN_EMIT, 140},
        { 23, HUFFMAN_EMIT, 140},
        { 24, HUFFMAN_EMIT, 140}
    },
    /* state 165 */ {
        { 17, HUFFMAN_EMIT, 141},
        { 18, HUFFMAN_EMIT, 141},
        { 19, HUFFMAN_EMIT, 141},
        { 20, HUFFMAN_EMIT, 141},
        { 21, HUFFMAN_EMIT, 141},
        { 22, HUFFMAN_EMIT, 141},
        { 23, HUFFMAN_EMIT, 141},
        { 24, HUFFMAN_EMIT, 141},
        { 17, HUFFMAN_EMIT, 143},
        { 18, HUFFMAN_EMIT, 143},
        { 19, HUFFMAN_EMIT, 143},
        { 20, HUFFMAN_EMIT, 143},
        { 21, HUFFMAN_EMIT, 143},
        { 22, HUFFMAN_EMIT, 143},
        { 23, HUFFMAN_EMIT, 143},
        { 24, HUFFMAN_EMIT, 143}
    },
    /* state 166 */ {
        { 17, HUFFMAN_EMIT, 147},
        { 18, HUFFMAN_EMIT, 147},
        { 19, HUFFMAN_EMIT, 147},
        { 20, HUFFMAN_EMIT, 147},
        { 21, HUFFMAN_EMIT, 147},
        { 22, HUFFMAN_EMIT, 147},
        { 23, HUFFMAN_EMIT, 147},
        { 24, HUFFMAN_EMIT, 147},
        { 17, HUFFMAN_EMIT, 149},
        { 18, HUFFMAN_EMIT, 149},
        { 19, HUFFMAN_EMIT, 149},
        { 20, HUFFMAN_EMIT, 149},
        { 21, HUFFMAN_EMIT, 149},
        { 22, HUFFMAN_EMIT, 149},
        { 23, HUFFMAN_EMIT, 149},
        { 24, HUFFMAN_EMIT, 149}
    },
    /* state 167 */ {
        { 17, HUFFMAN_EMIT, 150},
        { 18, HUFFMAN_EMIT, 150},
        { 19, HUFFMAN_EMIT, 150},
        { 20, HUFFMAN_EMIT, 150},
        { 21, HUFFMAN_EMIT, 150},
        { 22, HUFFMAN_EMIT, 150},
        { 23, HUFFMAN_EMIT, 150},
        { 24, HUFFMAN_EMIT, 150},
        { 17, HUFFMAN_EMIT, 151},
        { 18, HUFFMAN_EMIT, 151},
        { 19, HUFFMAN_EMIT, 151},
        { 20, HUFFMAN_EMIT, 151},
        { 21, HUFFMAN_EMIT, 151},
        { 22, HUFFMAN_EMIT, 151},
        { 23, HUFFMAN_EMIT, 151},
        { 24, HUFFMAN_EMIT, 151}
    },
    /* state 168 */ {
        { 17, HUFFMAN_EMIT, 152},
        { 18, HUFFMAN_EMIT, 152},
        { 19, HUFFMAN_EMIT, 152},
        { 20, HUFFMAN_EMIT, 152},
        { 21, HUFFMAN_EMIT, 152},
        { 22, HUFFMAN_EMIT, 152},
        { 23, HUFFMAN_EMIT, 152},
        { 24, HUFFMAN_EMIT, 152},
        { 17, HUFFMAN_EMIT, 155},
        { 18, HUFFMAN_EMIT, 155},
        { 19, HUFFMAN_EMIT, 155},
        { 20, HUFFMAN_EMIT, 155},
        { 21, HUFFMAN_EMIT, 155},
        { 22, HUFFMAN_EMIT, 155},
        { 23, HUFFMAN_EMIT, 155},
        { 24, HUFFMAN_EMIT, 155}
    },
    /* state 169 */ {
        { 17, HUFFMAN_EMIT, 157},
        { 18, HUFFMAN_EMIT, 157},
        { 19, HUFFMAN_EMIT, 157},
        { 20, HUFFMAN_EMIT, 157},
        { 21, HUFFMAN_EMIT, 157},
        { 22, HUFFMAN_EMIT, 157},
        { 23, HUFFMAN_EMIT, 157},
        { 24, HUFFMAN_EMIT, 157},
        { 17, HUFFMAN_EMIT, 158},
        { 18, HUFFMAN_EMIT, 158},
        { 19, HUFFMAN_EMIT, 158},
        { 20, HUFFMAN_EMIT, 158},
        { 21, HUFFMAN_EMIT, 158},
        { 22, HUFFMAN_EMIT, 158},
        { 23, HUFFMAN_EMIT, 158},
        { 24, HUFFMAN_EMIT, 158}
    },
    /* state 170 */ {
        { 17, HUFFMAN_EMIT, 165},
        { 18, HUFFMAN_EMIT, 165},
        { 19, HUFFMAN_EMIT, 165},
        { 20, HUFFMAN_EMIT, 165},
        { 21, HUFFMAN_EMIT, 165},
        { 22, HUFFMAN_EMIT, 165},
        { 23, HUFFMAN_EMIT, 165},
        { 24, HUFFMAN_EMIT, 165},
        { 17, HUFFMAN_EMIT, 166},
        { 18, HUFFMAN_EMIT, 166},
        { 19, HUFFMAN_EMIT, 166},
        { 20, HUFFMAN_EMIT, 166},
        { 21, HUFFMAN_EMIT, 166},
        { 22, HUFFMAN_EMIT, 166},
        { 23, HUFFMAN_EMIT, 166},
        { 24, HUFFMAN_EMIT, 166}
    },
    /* state 171 */ {
        { 17, HUFFMAN_EMIT, 168},
        { 18, HUFFMAN_EMIT, 168},
        { 19, HUFFMAN_EMIT, 168},
        { 20, HUFFMAN_EMIT, 168},
        { 21, HUFFMAN_EMIT, 168},
        { 22, HUFFMAN_EMIT, 168},
        { 23, HUFFMAN_EMIT, 168},
        { 24, HUFFMAN_EMIT, 168},
        { 17, HUFFMAN_EMIT, 174},
        { 18, HUFFMAN_EMIT, 174},
        { 19, HUFFMAN_EMIT, 174},
        { 20, HUFFMAN_EMIT, 174},
        { 21, HUFFMAN_EMIT, 174},
        { 22, HUFFMAN_EMIT, 174},
        { 23, HUFFMAN_EMIT, 174},
        { 24, HUFFMAN_EMIT, 174}
    },
    /* state 172 */ {
        { 17, HUFFMAN_EMIT, 175},
        { 18, HUFFMAN_EMIT, 175},
        { 19, HUFFMAN_EMIT, 175},
        { 20, HUFFMAN_EMIT, 175},
        { 21, HUFFMAN_EMIT, 175},
        { 22, HUFFMAN_EMIT, 175},
        { 23, HUFFMAN_EMIT, 175},
        { 24, HUFFMAN_EMIT, 175},
        { 17, HUFFMAN_EMIT, 180},
        { 18, HUFFMAN_EMIT, 180},
        { 19, HUFFMAN_EMIT, 180},
        { 20, HUFFMAN_EMIT, 180},
        { 21, HUFFMAN_EMIT, 180},
        { 22, HUFFMAN_EMIT, 180},
        { 23, HUFFMAN_EMIT, 180},
        { 24, HUFFMAN_EMIT, 180}
    },
    /* state 173 */ {
        { 17, HUFFMAN_EMIT, 182},
        { 18, HUFFMAN_EMIT, 182},
        { 19, HUFFMAN_EMIT, 182},
        { 20, HUFFMAN_EMIT, 182},
        { 21, HUFFMAN_EMIT, 182},
        { 22, HUFFMAN_EMIT, 182},
        { 23, HUFFMAN_EMIT, 182},
        { 24, HUFFMAN_EMIT, 182},
        { 17, HUFFMAN_EMIT, 183},
        { 18, HUFFMAN_EMIT, 183},
        { 19, HUFFMAN_EMIT, 183},
        { 20, HUFFMAN_EMIT, 183},
        { 21, HUFFMAN_EMIT, 183},
        { 22, HUFFMAN_EMIT, 183},
        { 23, HUFFMAN_EMIT, 183},
        { 24, HUFFMAN_EMIT, 183}
    },
    /* state 174 */ {
        { 17, HUFFMAN_EMIT, 188},
        { 18, HUFFMAN_EMIT, 188},
        { 19, HUFFMAN_EMIT, 188},
        { 20, HUFFMAN_EMIT, 188},
        { 21, HUFFMAN_EMIT, 188},
        { 22, HUFFMAN_EMIT, 188},
        { 23, HUFFMAN_EMIT, 188},
        { 24, HUFFMAN_EMIT, 188},
        { 17, HUFFMAN_EMIT, 191},
        { 18, HUFFMAN_EMIT, 191},
        { 19, HUFFMAN_EMIT, 191},
        { 20, HUFFMAN_EMIT, 191},
        { 21, HUFFMAN_EMIT, 191},
        { 22, HUFFMAN_EMIT, 191},
        { 23, HUFFMAN_EMIT, 191},
        { 24, HUFFMAN_EMIT, 191}
    },
    /* state 175 */ {
        { 17, HUFFMAN_EMIT, 197},
        { 18, HUFFMAN_EMIT, 197},
        { 19, HUFFMAN_EMIT, 197},
        { 20, HUFFMAN_EMIT, 197},
        { 21, HUFFMAN_EMIT, 197},
        { 22, HUFFMAN_EMIT, 197},
        { 23, HUFFMAN_EMIT, 197},
        { 24, HUFFMAN_EMIT, 197},
        { 17, HUFFMAN_EMIT, 231},
        { 18, HUFFMAN_EMIT, 231},
        { 19, HUFFMAN_EMIT, 231},
        { 20, HUFFMAN_EMIT, 231},
        { 21, HUFFMAN_EMIT, 231},
        { 22, HUFFMAN_EMIT, 231},
        { 23, HUFFMAN_EMIT, 231},
        { 24, HUFFMAN_EMIT, 231}
    },
    /* state 176 */ {
        { 17, HUFFMAN_EMIT, 239},
        { 18, HUFFMAN_EMIT, 239},
        { 19, HUFFMAN_EMIT, 239},
        { 20, HUFFMAN_EMIT, 239},
        { 21, HUFFMAN_EMIT, 239},
        { 22, HUFFMAN_EMIT, 239},
        { 23, HUFFMAN_EMIT, 239},
        { 24, HUFFMAN_EMIT, 239},
        { 25, HUFFMAN_EMIT,   9},
        { 26, HUFFMAN_EMIT,   9},
        { 27, HUFFMAN_EMIT,   9},
        { 28, HUFFMAN_EMIT,   9},
        { 25, HUFFMAN_EMIT, 142},
        { 26, HUFFMAN_EMIT, 142},
        { 27, HUFFMAN_EMIT, 142},
        { 28, HUFFMAN_EMIT, 142}
    },
    /* state 177 */ {
        { 25, HUFFMAN_EMIT, 144},
        { 26, HUFFMAN_EMIT, 144},
        { 27, HUFFMAN_EMIT, 144},
        { 28, HUFFMAN_EMIT, 144},
        { 25, HUFFMAN_EMIT, 145},
        { 26, HUFFMAN_EMIT, 145},
        { 27, HUFFMAN_EMIT, 145},
        { 28, HUFFMAN_EMIT, 145},
        { 25, HUFFMAN_EMIT, 148},
        { 26, HUFFMAN_EMIT, 148},
        { 27, HUFFMAN_EMIT, 148},
        { 28, HUFFMAN_EMIT, 148},
        { 25, HUFFMAN_EMIT, 159},
        { 26, HUFFMAN_EMIT, 159},
        { 27, HUFFMAN_EMIT, 159},
        { 28, HUFFMAN_EMIT, 159}
    },
    /* state 178 */ {
        { 25, HUFFMAN_EMIT, 171},
        { 26, HUFFMAN_EMIT, 171},
        { 27, HUFFMAN_EMIT, 171},
        { 28, HUFFMAN_EMIT, 171},
        { 25, HUFFMAN_EMIT, 206},
        { 26, HUFFMAN_EMIT, 206},
        { 27, HUFFMAN_EMIT, 206},
        { 28, HUFFMAN_EMIT, 206},
        { 25, HUFFMAN_EMIT, 215},
        { 26, HUFFMAN_EMIT, 215},
        { 27, HUFFMAN_EMIT, 215},
        { 28, HUFFMAN_EMIT, 215},
        { 25, HUFFMAN_EMIT, 225},
        { 26, HUFFMAN_EMIT, 225},
        { 27, HUFFMAN_EMIT, 225},
        { 28, HUFFMAN_EMIT, 225}
    },
    /* state 179 */ {
        { 25, HUFFMAN_EMIT, 236},
        { 26, HUFFMAN_EMIT, 236},
        { 27, HUFFMAN_EMIT, 236},
        { 28, HUFFMAN_EMIT, 236},
        { 25, HUFFMAN_EMIT, 237},
        { 26, HUFFMAN_EMIT, 237},
        { 27, HUFFMAN_EMIT, 237},
        { 28, HUFFMAN_EMIT, 237},
        { 29, HUFFMAN_EMIT, 199},
        { 30, HUFFMAN_EMIT, 199},
        { 29, HUFFMAN_EMIT, 207},
        { 30, HUFFMAN_EMIT, 207},
        { 29, HUFFMAN_EMIT, 234},
        { 30, HUFFMAN_EMIT, 234},
        { 29, HUFFMAN_EMIT, 235},
        { 30, HUFFMAN_EMIT, 235}
    },
    /* state 180 */ {
        {  0, HUFFMAN_EMIT, 192},
        {  0, HUFFMAN_EMIT, 193},
        {  0, HUFFMAN_EMIT, 200},
        {  0, HUFFMAN_EMIT, 201},
        {  0, HUFFMAN_EMIT, 202},
        {  0, HUFFMAN_EMIT, 205},
        {  0, HUFFMAN_EMIT, 210},
        {  0, HUFFMAN_EMIT, 213},
        {  0, HUFFMAN_EMIT, 218},
        {  0, HUFFMAN_EMIT, 219},
        {  0, HUFFMAN_EMIT, 238},
        {  0, HUFFMAN_EMIT, 240},
        {  0, HUFFMAN_EMIT, 242},
        {  0, HUFFMAN_EMIT, 243},
        {  0, HUFFMAN_EMIT, 255},
        {221, HUFFMAN_NONE,   0}
    },
    /* state 181 */ {
        {222, HUFFMAN_NONE,   0},
        {223, HUFFMAN_NONE,   0},
        {224, HUFFMAN_NONE,   0},
        {225, HUFFMAN_NONE,   0},
        {226, HUFFMAN_NONE,   0},
        {227, HUFFMAN_NONE,   0},
        {228, HUFFMAN_NONE,   0},
        {229, HUFFMAN_NONE,   0},
        {230, HUFFMAN_NONE,   0},
        {231, HUFFMAN_NONE,   0},
        {232, HUFFMAN_NONE,   0},
        {233, HUFFMAN_NONE,   0},
        {234, HUFFMAN_NONE,   0},
        {235, HUFFMAN_NONE,   0},
        {236, HUFFMAN_NONE,   0},
        {237, HUFFMAN_NONE,   0}
    },
    /* state 182 */ {
        { 17, HUFFMAN_EMIT, 129},
        { 18, HUFFMAN_EMIT, 129},
        { 19, HUFFMAN_EMIT, 129},
        { 20, HUFFMAN_EMIT, 129},
        { 21, HUFFMAN_EMIT, 129},
        { 22, HUFFMAN_EMIT, 129},
        { 23, HUFFMAN_EMIT, 129},
        { 24, HUFFMAN_EMIT, 129},
        { 17, HUFFMAN_EMIT, 132},
        { 18, HUFFMAN_EMIT, 132},
        { 19, HUFFMAN_EMIT, 132},
        { 20, HUFFMAN_EMIT, 132},
        { 21, HUFFMAN_EMIT, 132},
        { 22, HUFFMAN_EMIT, 132},
        { 23, HUFFMAN_EMIT, 132},
        { 24, HUFFMAN_EMIT, 132}
    },
    /* state 183 */ {
        { 17, HUFFMAN_EMIT, 133},
        { 18, HUFFMAN_EMIT, 133},
        { 19, HUFFMAN_EMIT, 133},
        { 20, HUFFMAN_EMIT, 133},
        { 21, HUFFMAN_EMIT, 133},
        { 22, HUFFMAN_EMIT, 133},
        { 23, HUFFMAN_EMIT, 133},
        { 24, HUFFMAN_EMIT, 133},
        { 17, HUFFMAN_EMIT, 134},
        { 18, HUFFMAN_EMIT, 134},
        { 19, HUFFMAN_EMIT, 134},
        { 20, HUFFMAN_EMIT, 134},
        { 21, HUFFMAN_EMIT, 134},
        { 22, HUFFMAN_EMIT, 134},
        { 23, HUFFMAN_EMIT, 134},
        { 24, HUFFMAN_EMIT, 134}
    },
    /* state 184 */ {
        { 17, HUFFMAN_EMIT, 136},
        { 18, HUFFMAN_EMIT, 136},
        { 19, HUFFMAN_EMIT, 136},
        { 20, HUFFMAN_EMIT, 136},
        { 21, HUFFMAN_EMIT, 136},
        { 22, HUFFMAN_EMIT, 136},
        { 23, HUFFMAN_EMIT, 136},
        { 24, HUFFMAN_EMIT, 136},
        { 17, HUFFMAN_EMIT, 146},
        { 18, HUFFMAN_EMIT, 146},
        { 19, HUFFMAN_EMIT, 146},
        { 20, HUFFMAN_EMIT, 146},
        { 21, HUFFMAN_EMIT, 146},
        { 22, HUFFMAN_EMIT, 146},
        { 23, HUFFMAN_EMIT, 146},
        { 24, HUFFMAN_EMIT, 146}
    },
    /* state 185 */ {
        { 17, HUFFMAN_EMIT, 154},
        { 18, HUFFMAN_EMIT, 154},
        { 19, HUFFMAN_EMIT, 154},
        { 20, HUFFMAN_EMIT, 154},
        { 21, HUFFMAN_EMIT, 154},
        { 22, HUFFMAN_EMIT, 154},
        { 23, HUFFMAN_EMIT, 154},
        { 24, HUFFMAN_EMIT, 154},
        { 17, HUFFMAN_EMIT, 156},
        { 18, HUFFMAN_EMIT, 156},
        { 19, HUFFMAN_EMIT, 156},
        { 20, HUFFMAN_EMIT, 156},
        { 21, HUFFMAN_EMIT, 156},
        { 22, HUFFMAN_EMIT, 156},
        { 23, HUFFMAN_EMIT, 156},
        { 24, HUFFMAN_EMIT, 156}
    },
    /* state 186 */ {
        { 17, HUFFMAN_EMIT, 160},
        { 18, HUFFMAN_EMIT, 160},
        { 19, HUFFMAN_EMIT, 160},
        { 20, HUFFMAN_EMIT, 160},
        { 21, HUFFMAN_EMIT, 160},
        { 22, HUFFMAN_EMIT, 160},
        { 23, HUFFMAN_EMIT, 160},
        { 24, HUFFMAN_EMIT, 160},
        { 17, HUFFMAN_EMIT, 163},
        { 18, HUFFMAN_EMIT, 163},
        { 19, HUFFMAN_EMIT, 163},
        { 20, HUFFMAN_EMIT, 163},
        { 21, HUFFMAN_EMIT, 163},
        { 22, HUFFMAN_EMIT, 163},
        { 23, HUFFMAN_EMIT, 163},
        { 24, HUFFMAN_EMIT, 163}
    },
    /* state 187 */ {
        { 17, HUFFMAN_EMIT, 164},
        { 18, HUFFMAN_EMIT, 164},
        { 19, HUFFMAN_EMIT, 164},
        { 20, HUFFMAN_EMIT, 164},
        { 21, HUFFMAN_EMIT, 164},
        { 22, HUFFMAN_EMIT, 164},
        { 23, HUFFMAN_EMIT, 164},
        { 24, HUFFMAN_EMIT, 164},
        { 17, HUFFMAN_EMIT, 169},
        { 18, HUFFMAN_EMIT, 169},
        { 19, HUFFMAN_EMIT, 169},
        { 20, HUFFMAN_EMIT, 169},
        { 21, HUFFMAN_EMIT, 169},
        { 22, HUFFMAN_EMIT, 169},
        { 23, HUFFMAN_EMIT, 169},
        { 24, HUFFMAN_EMIT, 169}
    },
    /* state 188 */ {
        { 17, HUFFMAN_EMIT, 170},
        { 18, HUFFMAN_EMIT, 170},
        { 19, HUFFMAN_EMIT, 170},
        { 20, HUFFMAN_EMIT, 170},
        { 21, HUFFMAN_EMIT, 170},
        { 22, HUFFMAN_EMIT, 170},
        { 23, HUFFMAN_EMIT, 170},
        { 24, HUFFMAN_EMIT, 170},
        { 17, HUFFMAN_EMIT, 173},
        { 18, HUFFMAN_EMIT, 173},
        { 19, HUFFMAN_EMIT, 173},
        { 20, HUFFMAN_EMIT, 173},
        { 21, HUFFMAN_EMIT, 173},
        { 22, HUFFMAN_EMIT, 173},
        { 23, HUFFMAN_EMIT, 173},
        { 24, HUFFMAN_EMIT, 173}
    },
    /* state 189 */ {
        { 17, HUFFMAN_EMIT, 178},
        { 18, HUFFMAN_EMIT, 178},
        { 19, HUFFMAN_EMIT, 178},
        { 20, HUFFMAN_EMIT, 178},
        { 21, HUFFMAN_EMIT, 178},
        { 22, HUFFMAN_EMIT, 178},
        { 23, HUFFMAN_EMIT, 178},
        { 24, HUFFMAN_EMIT, 178},
        { 17, HUFFMAN_EMIT, 181},
        { 18, HUFFMAN_EMIT, 181},
        { 19, HUFFMAN_EMIT, 181},
        { 20, HUFFMAN_EMIT, 181},
        { 21, HUFFMAN_EMIT, 181},
        { 22, HUFFMAN_EMIT, 181},
        { 23, HUFFMAN_EMIT, 181},
        { 24, HUFFMAN_EMIT, 181}
    },
    /* state 190 */ {
        { 17, HUFFMAN_EMIT, 185},
        { 18, HUFFMAN_EMIT, 185},
        { 19, HUFFMAN_EMIT, 185},
        { 20, HUFFMAN_EMIT, 185},
        { 21, HUFFMAN_EMIT, 185},
        { 22, HUFFMAN_EMIT, 185},
        { 23, HUFFMAN_EMIT, 185},
        { 24, HUFFMAN_EMIT, 185},
        { 17, HUFFMAN_EMIT, 186},
        { 18, HUFFMAN_EMIT, 186},
        { 19, HUFFMAN_EMIT, 186},
        { 20, HUFFMAN_EMIT, 186},
        { 21, HUFFMAN_EMIT, 186},
        { 22, HUFFMAN_EMIT, 186},
        { 23, HUFFMAN_EMIT, 186},
        { 24, HUFFMAN_EMIT, 186}
    },
    /* state 191 */ {
        { 17, HUFFMAN_EMIT, 187},
        { 18, HUFFMAN_EMIT, 187},
        { 19, HUFFMAN_EMIT, 187},
        { 20, HUFFMAN_EMIT, 187},
        { 21, HUFFMAN_EMIT, 187},
        { 22, HUFFMAN_EMIT, 187},
        { 23, HUFFMAN_EMIT, 187},
        { 24, HUFFMAN_EMIT, 187},
        { 17, HUFFMAN_EMIT, 189},
        { 18, HUFFMAN_EMIT, 189},
        { 19, HUFFMAN_EMIT, 189},
        { 20, HUFFMAN_EMIT, 189},
        { 21, HUFFMAN_EMIT, 189},
        { 22, HUFFMAN_EMIT, 189},
        { 23, HUFFMAN_EMIT, 189},
        { 24, HUFFMAN_EMIT, 189}
    },
    /* state 192 */ {
        { 17, HUFFMAN_EMIT, 190},
        { 18, HUFFMAN_EMIT, 190},
        { 19, HUFFMAN_EMIT, 190},
        { 20, HUFFMAN_EMIT, 190},
        { 21, HUFFMAN_EMIT, 190},
        { 22, HUFFMAN_EMIT, 190},
        { 23, HUFFMAN_EMIT, 190},
        { 24, HUFFMAN_EMIT, 190},
        { 17, HUFFMAN_EMIT, 196},
        { 18, HUFFMAN_EMIT, 196},
        { 19, HUFFMAN_EMIT, 196},
        { 20, HUFFMAN_EMIT, 196},
        { 21, HUFFMAN_EMIT, 196},
        { 22, HUFFMAN_EMIT, 196},
        { 23, HUFFMAN_EMIT, 196},
        { 24, HUFFMAN_EMIT, 196}
    },
    /* state 193 */ {
        { 17, HUFFMAN_EMIT, 198},
        { 18, HUFFMAN_EMIT, 198},
        { 19, HUFFMAN_EMIT, 198},
        { 20, HUFFMAN_EMIT, 198},
        { 21, HUFFMAN_EMIT, 198},
        { 22, HUFFMAN_EMIT, 198},
        { 23, HUFFMAN_EMIT, 198},
        { 24, HUFFMAN_EMIT, 198},
        { 17, HUFFMAN_EMIT, 228},
        { 18, HUFFMAN_EMIT, 228},
        { 19, HUFFMAN_EMIT, 228},
        { 20, HUFFMAN_EMIT, 228},
        { 21, HUFFMAN_EMIT, 228},
        { 22, HUFFMAN_EMIT, 228},
        { 23, HUFFMAN_EMIT, 228},
        { 24, HUFFMAN_EMIT, 228}
    },
    /* state 194 */ {
        { 17, HUFFMAN_EMIT, 232},
        { 18, HUFFMAN_EMIT, 232},
        { 19, HUFFMAN_EMIT, 232},
        { 20, HUFFMAN_EMIT, 232},
        { 21, HUFFMAN_EMIT, 232},
        { 22, HUFFMAN_EMIT, 232},
        { 23, HUFFMAN_EMIT, 232},
        { 24, HUFFMAN_EMIT, 232},
        { 17, HUFFMAN_EMIT, 233},
        { 18, HUFFMAN_EMIT, 233},
        { 19, HUFFMAN_EMIT, 233},
        { 20, HUFFMAN_EMIT, 233},
        { 21, HUFFMAN_EMIT, 233},
        { 22, HUFFMAN_EMIT, 233},
        { 23, HUFFMAN_EMIT, 233},
        { 24, HUFFMAN_EMIT, 233}
    },
    /* state 195 */ {
        { 25, HUFFMAN_EMIT,   1},
        { 26, HUFFMAN_EMIT,   1},
        { 27, HUFFMAN_EMIT,   1},
        { 28, HUFFMAN_EMIT,   1},
        { 25, HUFFMAN_EMIT, 135},
        { 26, HUFFMAN_EMIT, 135},
        { 27, HUFFMAN_EMIT, 135},
        { 28, HUFFMAN_EMIT, 135},
        { 25, HUFFMAN_EMIT, 137},
        { 26, HUFFMAN_EMIT, 137},
        { 27, HUFFMAN_EMIT, 137},
        { 28, HUFFMAN_EMIT, 137},
        { 25, HUFFMAN_EMIT, 138},
        { 26, HUFFMAN_EMIT, 138},
        { 27, HUFFMAN_EMIT, 138},
        { 28, HUFFMAN_EMIT, 138}
    },
    /* state 196 */ {
        { 25, HUFFMAN_EMIT, 139},
        { 26, HUFFMAN_EMIT, 139},
        { 27, HUFFMAN_EMIT, 139},
        { 28, HUFFMAN_EMIT, 139},
        { 25, HUFFMAN_EMIT, 140},
        { 26, HUFFMAN_EMIT, 140},
        { 27, HUFFMAN_EMIT, 140},
        { 28, HUFFMAN_EMIT, 140},
        { 25, HUFFMAN_EMIT, 141},
        { 26, HUFFMAN_EMIT, 141},
        { 27, HUFFMAN_EMIT, 141},
        { 28, HUFFMAN_EMIT, 141},
        { 25, HUFFMAN_EMIT, 143},
        { 26, HUFFMAN_EMIT, 143},
        { 27, HUFFMAN_EMIT, 143},
        { 28, HUFFMAN_EMIT, 143}
    },
    /* state 197 */ {
        { 25, HUFFMAN_EMIT, 147},
        { 26, HUFFMAN_EMIT, 147},
        { 27, HUFFMAN_EMIT, 147},
        { 28, HUFFMAN_EMIT, 147},
        { 25, HUFFMAN_EMIT, 149},
        { 26, HUFFMAN_EMIT, 149},
        { 27, HUFFMAN_EMIT, 149},
        { 28, HUFFMAN_EMIT, 149},
        { 25, HUFFMAN_EMIT, 150},
        { 26, HUFFMAN_EMIT, 150},
        { 27, HUFFMAN_EMIT, 150},
        { 28, HUFFMAN_EMIT, 150},
        { 25, HUFFMAN_EMIT, 151},
        { 26, HUFFMAN_EMIT, 151},
        { 27, HUFFMAN_EMIT, 151},
        { 28, HUFFMAN_EMIT, 151}
    },
    /* state 198 */ {
        { 25, HUFFMAN_EMIT, 152},
        { 26, HUFFMAN_EMIT, 152},
        { 27, HUFFMAN_EMIT, 152},
        { 28, HUFFMAN_EMIT, 152},
        { 25, HUFFMAN_EMIT, 155},
        { 26, HUFFMAN_EMIT, 155},
        { 27, HUFFMAN_EMIT, 155},
        { 28, HUFFMAN_EMIT, 155},
        { 25, HUFFMAN_EMIT, 157},
        { 26, HUFFMAN_EMIT, 157},
        { 27, HUFFMAN_EMIT, 157},
        { 28, HUFFMAN_EMIT, 157},
        { 25, HUFFMAN_EMIT, 158},
        { 26, HUFFMAN_EMIT, 158},
        { 27, HUFFMAN_EMIT, 158},
        { 28, HUFFMAN_EMIT, 158}
    },
    /* state 199 */ {
        { 25, HUFFMAN_EMIT, 165},
        { 26, HUFFMAN_EMIT, 165},
        { 27, HUFFMAN_EMIT, 165},
        { 28, HUFFMAN_EMIT, 165},
        { 25, HUFFMAN_EMIT, 166},
        { 26, HUFFMAN_EMIT, 166},
        { 27, HUFFMAN_EMIT, 166},
        { 28, HUFFMAN_EMIT, 166},
        { 25, HUFFMAN_EMIT, 168},
        { 26, HUFFMAN_EMIT, 168},
        { 27, HUFFMAN_EMIT, 168},
        { 28, HUFFMAN_EMIT, 168},
        { 25, HUFFMAN_EMIT, 174},
        { 26, HUFFMAN_EMIT, 174},
        { 27, HUFFMAN_EMIT, 174},
        { 28, HUFFMAN_EMIT, 174}
    },
    /* state 200 */ {
        { 25, HUFFMAN_EMIT, 175},
        { 26, HUFFMAN_EMIT, 175},
        { 27, HUFFMAN_EMIT, 175},
        { 28, HUFFMAN_EMIT, 175},
        { 25, HUFFMAN_EMIT, 180},
        { 26, HUFFMAN_EMIT, 180},
        { 27, HUFFMAN_EMIT, 180},
        { 28, HUFFMAN_EMIT, 180},
        { 25, HUFFMAN_EMIT, 182},
        { 26, HUFFMAN_EMIT, 182},
        { 27, HUFFMAN_EMIT, 182},
        { 28, HUFFMAN_EMIT, 182},
        { 25, HUFFMAN_EMIT, 183},
        { 26, HUFFMAN_EMIT, 183},
        { 27, HUFFMAN_EMIT, 183},
        { 28, HUFFMAN_EMIT, 183}
    },
    /* state 201 */ {
        { 25, HUFFMAN_EMIT, 188},
        { 26, HUFFMAN_EMIT, 188},
        { 27, HUFFMAN_EMIT, 188},
        { 28, HUFFMAN_EMIT, 188},
        { 25, HUFFMAN_EMIT, 191},
        { 26, HUFFMAN_EMIT, 191},
        { 27, HUFFMAN_EMIT, 191},
        { 28, HUFFMAN_EMIT, 191},
        { 25, HUFFMAN_EMIT, 197},
        { 26, HUFFMAN_EMIT, 197},
        { 27, HUFFMAN_EMIT, 197},
        { 28, HUFFMAN_EMIT, 197},
        { 25, HUFFMAN_EMIT, 231},
        { 26, HUFFMAN_EMIT, 231},
        { 27, HUFFMAN_EMIT, 231},
        { 28, HUFFMAN_EMIT, 231}
    },
    /* state 202 */ {
        { 25, HUFFMAN_EMIT, 239},
        { 26, HUFFMAN_EMIT, 239},
        { 27, HUFFMAN_EMIT, 239},
        { 28, HUFFMAN_EMIT, 239},
        { 29, HUFFMAN_EMIT,   9},
        { 30, HUFFMAN_EMIT,   9},
        { 29, HUFFMAN_EMIT, 142},
        { 30, HUFFMAN_EMIT, 142},
        { 29, HUFFMAN_EMIT, 144},
        { 30, HUFFMAN_EMIT, 144},
        { 29, HUFFMAN_EMIT, 145},
        { 30, HUFFMAN_EMIT, 145},
        { 29, HUFFMAN_EMIT, 148},
        { 30, HUFFMAN_EMIT, 148},
        { 29, HUFFMAN_EMIT, 159},
        { 30, HUFFMAN_EMIT, 159}
    },
    /* state 203 */ {
        { 29, HUFFMAN_EMIT, 171},
        { 30, HUFFMAN_EMIT, 171},
        { 29, HUFFMAN_EMIT, 206},
        { 30, HUFFMAN_EMIT, 206},
        { 29, HUFFMAN_EMIT, 215},
        { 30, HUFFMAN_EMIT, 215},
        { 29, HUFFMAN_EMIT, 225},
        { 30, HUFFMAN_EMIT, 225},
        { 29, HUFFMAN_EMIT, 236},
        { 30, HUFFMAN_EMIT, 236},
        { 29, HUFFMAN_EMIT, 237},
        { 30, HUFFMAN_EMIT, 237},
        {  0, HUFFMAN_EMIT, 199},
        {  0, HUFFMAN_EMIT, 207},
        {  0, HUFFMAN_EMIT, 234},
        {  0, HUFFMAN_EMIT, 235}
    },
    /* state 204 */ {
        {238, HUFFMAN_NONE,   0},
        {239, HUFFMAN_NONE,   0},
        {240, HUFFMAN_NONE,   0},
        {241, HUFFMAN_NONE,   0},
        {242, HUFFMAN_NONE,   0},
        {243, HUFFMAN_NONE,   0},
        {244, HUFFMAN_NONE,   0},
        {245, HUFFMAN_NONE,   0},
        {246, HUFFMAN_NONE,   0},
        {247, HUFFMAN_NONE,   0},
        {248, HUFFMAN_NONE,   0},
        {249, HUFFMAN_NONE,   0},
        {250, HUFFMAN_NONE,   0},
        {251, HUFFMAN_NONE,   0},
        {252, HUFFMAN_NONE,   0},
        {253, HUFFMAN_NONE,   0}
    },
    /* state 205 */ {
        { 25, HUFFMAN_EMIT,  10},
        { 26, HUFFMAN_EMIT,  10},
        { 27, HUFFMAN_EMIT,  10},
        { 28, HUFFMAN_EMIT,  10},
        { 25, HUFFMAN_EMIT,  13},
        { 26, HUFFMAN_EMIT,  13},
        { 27, HUFFMAN_EMIT,  13},
        { 28, HUFFMAN_EMIT,  13},
        { 25, HUFFMAN_EMIT,  22},
        { 26, HUFFMAN_EMIT,  22},
        { 27, HUFFMAN_EMIT,  22},
        { 28, HUFFMAN_EMIT,  22},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0}
    },
    /* state 206 */ {
        { 17, HUFFMAN_EMIT,   2},
        { 18, HUFFMAN_EMIT,   2},
        { 19, HUFFMAN_EMIT,   2},
        { 20, HUFFMAN_EMIT,   2},
        { 21, HUFFMAN_EMIT,   2},
        { 22, HUFFMAN_EMIT,   2},
        { 23, HUFFMAN_EMIT,   2},
        { 24, HUFFMAN_EMIT,   2},
        { 17, HUFFMAN_EMIT,   3},
        { 18, HUFFMAN_EMIT,   3},
        { 19, HUFFMAN_EMIT,   3},
        { 20, HUFFMAN_EMIT,   3},
        { 21, HUFFMAN_EMIT,   3},
        { 22, HUFFMAN_EMIT,   3},
        { 23, HUFFMAN_EMIT,   3},
        { 24, HUFFMAN_EMIT,   3}
    },
    /* state 207 */ {
        { 17, HUFFMAN_EMIT,   4},
        { 18, HUFFMAN_EMIT,   4},
        { 19, HUFFMAN_EMIT,   4},
        { 20, HUFFMAN_EMIT,   4},
        { 21, HUFFMAN_EMIT,   4},
        { 22, HUFFMAN_EMIT,   4},
        { 23, HUFFMAN_EMIT,   4},
        { 24, HUFFMAN_EMIT,   4},
        { 17, HUFFMAN_EMIT,   5},
        { 18, HUFFMAN_EMIT,   5},
        { 19, HUFFMAN_EMIT,   5},
        { 20, HUFFMAN_EMIT,   5},
        { 21, HUFFMAN_EMIT,   5},
        { 22, HUFFMAN_EMIT,   5},
        { 23, HUFFMAN_EMIT,   5},
        { 24, HUFFMAN_EMIT,   5}
    },
    /* state 208 */ {
        { 17, HUFFMAN_EMIT,   6},
        { 18, HUFFMAN_EMIT,   6},
        { 19, HUFFMAN_EMIT,   6},
        { 20, HUFFMAN_EMIT,   6},
        { 21, HUFFMAN_EMIT,   6},
        { 22, HUFFMAN_EMIT,   6},
        { 23, HUFFMAN_EMIT,   6},
        { 24, HUFFMAN_EMIT,   6},
        { 17, HUFFMAN_EMIT,   7},
        { 18, HUFFMAN_EMIT,   7},
        { 19, HUFFMAN_EMIT,   7},
        { 20, HUFFMAN_EMIT,   7},
        { 21, HUFFMAN_EMIT,   7},
        { 22, HUFFMAN_EMIT,   7},
        { 23, HUFFMAN_EMIT,   7},
        { 24, HUFFMAN_EMIT,   7}
    },
    /* state 209 */ {
        { 17, HUFFMAN_EMIT,   8},
        { 18, HUFFMAN_EMIT,   8},
        { 19, HUFFMAN_EMIT,   8},
        { 20, HUFFMAN_EMIT,   8},
        { 21, HUFFMAN_EMIT,   8},
        { 22, HUFFMAN_EMIT,   8},
        { 23, HUFFMAN_EMIT,   8},
        { 24, HUFFMAN_EMIT,   8},
        { 17, HUFFMAN_EMIT,  11},
        { 18, HUFFMAN_EMIT,  11},
        { 19, HUFFMAN_EMIT,  11},
        { 20, HUFFMAN_EMIT,  11},
        { 21, HUFFMAN_EMIT,  11},
        { 22, HUFFMAN_EMIT,  11},
        { 23, HUFFMAN_EMIT,  11},
        { 24, HUFFMAN_EMIT,  11}
    },
    /* state 210 */ {
        { 17, HUFFMAN_EMIT,  12},
        { 18, HUFFMAN_EMIT,  12},
        { 19, HUFFMAN_EMIT,  12},
        { 20, HUFFMAN_EMIT,  12},
        { 21, HUFFMAN_EMIT,  12},
        { 22, HUFFMAN_EMIT,  12},
        { 23, HUFFMAN_EMIT,  12},
        { 24, HUFFMAN_EMIT,  12},
        { 17, HUFFMAN_EMIT,  14},
        { 18, HUFFMAN_EMIT,  14},
        { 19, HUFFMAN_EMIT,  14},
        { 20, HUFFMAN_EMIT,  14},
        { 21, HUFFMAN_EMIT,  14},
        { 22, HUFFMAN_EMIT,  14},
        { 23, HUFFMAN_EMIT,  14},
        { 24, HUFFMAN_EMIT,  14}
    },
    /* state 211 */ {
        { 17, HUFFMAN_EMIT,  15},
        { 18, HUFFMAN_EMIT,  15},
        { 19, HUFFMAN_EMIT,  15},
        { 20, HUFFMAN_EMIT,  15},
        { 21, HUFFMAN_EMIT,  15},
        { 22, HUFFMAN_EMIT,  15},
        { 23, HUFFMAN_EMIT,  15},
        { 24, HUFFMAN_EMIT,  15},
        { 17, HUFFMAN_EMIT,  16},
        { 18, HUFFMAN_EMIT,  16},
        { 19, HUFFMAN_EMIT,  16},
        { 20, HUFFMAN_EMIT,  16},
        { 21, HUFFMAN_EMIT,  16},
        { 22, HUFFMAN_EMIT,  16},
        { 23, HUFFMAN_EMIT,  16},
        { 24, HUFFMAN_EMIT,  16}
    },
    /* state 212 */ {
        { 17, HUFFMAN_EMIT,  17},
        { 18, HUFFMAN_EMIT,  17},
        { 19, HUFFMAN_EMIT,  17},
        { 20, HUFFMAN_EMIT,  17},
        { 21, HUFFMAN_EMIT,  17},
        { 22, HUFFMAN_EMIT,  17},
        { 23, HUFFMAN_EMIT,  17},
        { 24, HUFFMAN_EMIT,  17},
        { 17, HUFFMAN_EMIT,  18},
        { 18, HUFFMAN_EMIT,  18},
        { 19, HUFFMAN_EMIT,  18},
        { 20, HUFFMAN_EMIT,  18},
        { 21, HUFFMAN_EMIT,  18},
        { 22, HUFFMAN_EMIT,  18},
        { 23, HUFFMAN_EMIT,  18},
        { 24, HUFFMAN_EMIT,  18}
    },
    /* state 213 */ {
        { 17, HUFFMAN_EMIT,  19},
        { 18, HUFFMAN_EMIT,  19},
        { 19, HUFFMAN_EMIT,  19},
        { 20, HUFFMAN_EMIT,  19},
        { 21, HUFFMAN_EMIT,  19},
        { 22, HUFFMAN_EMIT,  19},
        { 23, HUFFMAN_EMIT,  19},
        { 24, HUFFMAN_EMIT,  19},
        { 17, HUFFMAN_EMIT,  20},
        { 18, HUFFMAN_EMIT,  20},
        { 19, HUFFMAN_EMIT,  20},
        { 20, HUFFMAN_EMIT,  20},
        { 21, HUFFMAN_EMIT,  20},
        { 22, HUFFMAN_EMIT,  20},
        { 23, HUFFMAN_EMIT,  20},
        { 24, HUFFMAN_EMIT,  20}
    },
    /* state 214 */ {
        { 17, HUFFMAN_EMIT,  21},
        { 18, HUFFMAN_EMIT,  21},
        { 19, HUFFMAN_EMIT,  21},
        { 20, HUFFMAN_EMIT,  21},
        { 21, HUFFMAN_EMIT,  21},
        { 22, HUFFMAN_EMIT,  21},
        { 23, HUFFMAN_EMIT,  21},
        { 24, HUFFMAN_EMIT,  21},
        { 17, HUFFMAN_EMIT,  23},
        { 18, HUFFMAN_EMIT,  23},
        { 19, HUFFMAN_EMIT,  23},
        { 20, HUFFMAN_EMIT,  23},
        { 21, HUFFMAN_EMIT,  23},
        { 22, HUFFMAN_EMIT,  23},
        { 23, HUFFMAN_EMIT,  23},
        { 24, HUFFMAN_EMIT,  23}
    },
    /* state 215 */ {
        { 17, HUFFMAN_EMIT,  24},
        { 18, HUFFMAN_EMIT,  24},
        { 19, HUFFMAN_EMIT,  24},
        { 20, HUFFMAN_EMIT,  24},
        { 21, HUFFMAN_EMIT,  24},
        { 22, HUFFMAN_EMIT,  24},
        { 23, HUFFMAN_EMIT,  24},
        { 24, HUFFMAN_EMIT,  24},
        { 17, HUFFMAN_EMIT,  25},
        { 18, HUFFMAN_EMIT,  25},
        { 19, HUFFMAN_EMIT,  25},
        { 20, HUFFMAN_EMIT,  25},
        { 21, HUFFMAN_EMIT,  25},
        { 22, HUFFMAN_EMIT,  25},
        { 23, HUFFMAN_EMIT,  25},
        { 24, HUFFMAN_EMIT,  25}
    },
    /* state 216 */ {
        { 17, HUFFMAN_EMIT,  26},
        { 18, HUFFMAN_EMIT,  26},
        { 19, HUFFMAN_EMIT,  26},
        { 20, HUFFMAN_EMIT,  26},
        { 21, HUFFMAN_EMIT,  26},
        { 22, HUFFMAN_EMIT,  26},
        { 23, HUFFMAN_EMIT,  26},
        { 24, HUFFMAN_EMIT,  26},
        { 17, HUFFMAN_EMIT,  27},
        { 18, HUFFMAN_EMIT,  27},
        { 19, HUFFMAN_EMIT,  27},
        { 20, HUFFMAN_EMIT,  27},
        { 21, HUFFMAN_EMIT,  27},
        { 22, HUFFMAN_EMIT,  27},
        { 23, HUFFMAN_EMIT,  27},
        { 24, HUFFMAN_EMIT,  27}
    },
    /* state 217 */ {
        { 17, HUFFMAN_EMIT,  28},
        { 18, HUFFMAN_EMIT,  28},
        { 19, HUFFMAN_EMIT,  28},
        { 20, HUFFMAN_EMIT,  28},
        { 21, HUFFMAN_EMIT,  28},
        { 22, HUFFMAN_EMIT,  28},
        { 23, HUFFMAN_EMIT,  28},
        { 24, HUFFMAN_EMIT,  28},
        { 17, HUFFMAN_EMIT,  29},
        { 18, HUFFMAN_EMIT,  29},
        { 19, HUFFMAN_EMIT,  29},
        { 20, HUFFMAN_EMIT,  29},
        { 21, HUFFMAN_EMIT,  29},
        { 22, HUFFMAN_EMIT,  29},
        { 23, HUFFMAN_EMIT,  29},
        { 24, HUFFMAN_EMIT,  29}
    },
    /* state 218 */ {
        { 17, HUFFMAN_EMIT,  30},
        { 18, HUFFMAN_EMIT,  30},
        { 19, HUFFMAN_EMIT,  30},
        { 20, HUFFMAN_EMIT,  30},
        { 21, HUFFMAN_EMIT,  30},
        { 22, HUFFMAN_EMIT,  30},
        { 23, HUFFMAN_EMIT,  30},
        { 24, HUFFMAN_EMIT,  30},
        { 17, HUFFMAN_EMIT,  31},
        { 18, HUFFMAN_EMIT,  31},
        { 19, HUFFMAN_EMIT,  31},
        { 20, HUFFMAN_EMIT,  31},
        { 21, HUFFMAN_EMIT,  31},
        { 22, HUFFMAN_EMIT,  31},
        { 23, HUFFMAN_EMIT,  31},
        { 24, HUFFMAN_EMIT,  31}
    },
    /* state 219 */ {
        { 17, HUFFMAN_EMIT, 127},
        { 18, HUFFMAN_EMIT, 127},
        { 19, HUFFMAN_EMIT, 127},
        { 20, HUFFMAN_EMIT, 127},
        { 21, HUFFMAN_EMIT, 127},
        { 22, HUFFMAN_EMIT, 127},
        { 23, HUFFMAN_EMIT, 127},
        { 24, HUFFMAN_EMIT, 127},
        { 17, HUFFMAN_EMIT, 220},
        { 18, HUFFMAN_EMIT, 220},
        { 19, HUFFMAN_EMIT, 220},
        { 20, HUFFMAN_EMIT, 220},
        { 21, HUFFMAN_EMIT, 220},
        { 22, HUFFMAN_EMIT, 220},
        { 23, HUFFMAN_EMIT, 220},
        { 24, HUFFMAN_EMIT, 220}
    },
    /* state 220 */ {
        { 17, HUFFMAN_EMIT, 249},
        { 18, HUFFMAN_EMIT, 249},
        { 19, HUFFMAN_EMIT, 249},
        { 20, HUFFMAN_EMIT, 249},
        { 21, HUFFMAN_EMIT, 249},
        { 22, HUFFMAN_EMIT, 249},
        { 23, HUFFMAN_EMIT, 249},
        { 24, HUFFMAN_EMIT, 249},
        { 29, HUFFMAN_EMIT,  10},
        { 30, HUFFMAN_EMIT,  10},
        { 29, HUFFMAN_EMIT,  13},
        { 30, HUFFMAN_EMIT,  13},
        { 29, HUFFMAN_EMIT,  22},
        { 30, HUFFMAN_EMIT,  22},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0}
    },
    /* state 221 */ {
        { 17, HUFFMAN_EMIT, 203},
        { 18, HUFFMAN_EMIT, 203},
        { 19, HUFFMAN_EMIT, 203},
        { 20, HUFFMAN_EMIT, 203},
        { 21, HUFFMAN_EMIT, 203},
        { 22, HUFFMAN_EMIT, 203},
        { 23, HUFFMAN_EMIT, 203},
        { 24, HUFFMAN_EMIT, 203},
        { 17, HUFFMAN_EMIT, 204},
        { 18, HUFFMAN_EMIT, 204},
        { 19, HUFFMAN_EMIT, 204},
        { 20, HUFFMAN_EMIT, 204},
        { 21, HUFFMAN_EMIT, 204},
        { 22, HUFFMAN_EMIT, 204},
        { 23, HUFFMAN_EMIT, 204},
        { 24, HUFFMAN_EMIT, 204}
    },
    /* state 222 */ {
        { 17, HUFFMAN_EMIT, 211},
        { 18, HUFFMAN_EMIT, 211},
        { 19, HUFFMAN_EMIT, 211},
        { 20, HUFFMAN_EMIT, 211},
        { 21, HUFFMAN_EMIT, 211},
        { 22, HUFFMAN_EMIT, 211},
        { 23, HUFFMAN_EMIT, 211},
        { 24, HUFFMAN_EMIT, 211},
        { 17, HUFFMAN_EMIT, 212},
        { 18, HUFFMAN_EMIT, 212},
        { 19, HUFFMAN_EMIT, 212},
        { 20, HUFFMAN_EMIT, 212},
        { 21, HUFFMAN_EMIT, 212},
        { 22, HUFFMAN_EMIT, 212},
        { 23, HUFFMAN_EMIT, 212},
        { 24, HUFFMAN_EMIT, 212}
    },
    /* state 223 */ {
        { 17, HUFFMAN_EMIT, 214},
        { 18, HUFFMAN_EMIT, 214},
        { 19, HUFFMAN_EMIT, 214},
        { 20, HUFFMAN_EMIT, 214},
        { 21, HUFFMAN_EMIT, 214},
        { 22, HUFFMAN_EMIT, 214},
        { 23, HUFFMAN_EMIT, 214},
        { 24, HUFFMAN_EMIT, 214},
        { 17, HUFFMAN_EMIT, 221},
        { 18, HUFFMAN_EMIT, 221},
        { 19, HUFFMAN_EMIT, 221},
        { 20, HUFFMAN_EMIT, 221},
        { 21, HUFFMAN_EMIT, 221},
        { 22, HUFFMAN_EMIT, 221},
        { 23, HUFFMAN_EMIT, 221},
        { 24, HUFFMAN_EMIT, 221}
    },
    /* state 224 */ {
        { 17, HUFFMAN_EMIT, 222},
        { 18, HUFFMAN_EMIT, 222},
        { 19, HUFFMAN_EMIT, 222},
        { 20, HUFFMAN_EMIT, 222},
        { 21, HUFFMAN_EMIT, 222},
        { 22, HUFFMAN_EMIT, 222},
        { 23, HUFFMAN_EMIT, 222},
        { 24, HUFFMAN_EMIT, 222},
        { 17, HUFFMAN_EMIT, 223},
        { 18, HUFFMAN_EMIT, 223},
        { 19, HUFFMAN_EMIT, 223},
        { 20, HUFFMAN_EMIT, 223},
        { 21, HUFFMAN_EMIT, 223},
        { 22, HUFFMAN_EMIT, 223},
        { 23, HUFFMAN_EMIT, 223},
        { 24, HUFFMAN_EMIT, 223}
    },
    /* state 225 */ {
        { 17, HUFFMAN_EMIT, 241},
        { 18, HUFFMAN_EMIT, 241},
        { 19, HUFFMAN_EMIT, 241},
        { 20, HUFFMAN_EMIT, 241},
        { 21, HUFFMAN_EMIT, 241},
        { 22, HUFFMAN_EMIT, 241},
        { 23, HUFFMAN_EMIT, 241},
        { 24, HUFFMAN_EMIT, 241},
        { 17, HUFFMAN_EMIT, 244},
        { 18, HUFFMAN_EMIT, 244},
        { 19, HUFFMAN_EMIT, 244},
        { 20, HUFFMAN_EMIT, 244},
        { 21, HUFFMAN_EMIT, 244},
        { 22, HUFFMAN_EMIT, 244},
        { 23, HUFFMAN_EMIT, 244},
        { 24, HUFFMAN_EMIT, 244}
    },
    /* state 226 */ {
        { 17, HUFFMAN_EMIT, 245},
        { 18, HUFFMAN_EMIT, 245},
        { 19, HUFFMAN_EMIT, 245},
        { 20, HUFFMAN_EMIT, 245},
        { 21, HUFFMAN_EMIT, 245},
        { 22, HUFFMAN_EMIT, 245},
        { 23, HUFFMAN_EMIT, 245},
        { 24, HUFFMAN_EMIT, 245},
        { 17, HUFFMAN_EMIT, 246},
        { 18, HUFFMAN_EMIT, 246},
        { 19, HUFFMAN_EMIT, 246},
        { 20, HUFFMAN_EMIT, 246},
        { 21, HUFFMAN_EMIT, 246},
        { 22, HUFFMAN_EMIT, 246},
        { 23, HUFFMAN_EMIT, 246},
        { 24, HUFFMAN_EMIT, 246}
    },
    /* state 227 */ {
        { 17, HUFFMAN_EMIT, 247},
        { 18, HUFFMAN_EMIT, 247},
        { 19, HUFFMAN_EMIT, 247},
        { 20, HUFFMAN_EMIT, 247},
        { 21, HUFFMAN_EMIT, 247},
        { 22, HUFFMAN_EMIT, 247},
        { 23, HUFFMAN_EMIT, 247},
        { 24, HUFFMAN_EMIT, 247},
        { 17, HUFFMAN_EMIT, 248},
        { 18, HUFFMAN_EMIT, 248},
        { 19, HUFFMAN_EMIT, 248},
        { 20, HUFFMAN_EMIT, 248},
        { 21, HUFFMAN_EMIT, 248},
        { 22, HUFFMAN_EMIT, 248},
        { 23, HUFFMAN_EMIT, 248},
        { 24, HUFFMAN_EMIT, 248}
    },
    /* state 228 */ {
        { 17, HUFFMAN_EMIT, 250},
        { 18, HUFFMAN_EMIT, 250},
        { 19, HUFFMAN_EMIT, 250},
        { 20, HUFFMAN_EMIT, 250},
        { 21, HUFFMAN_EMIT, 250},
        { 22, HUFFMAN_EMIT, 250},
        { 23, HUFFMAN_EMIT, 250},
        { 24, HUFFMAN_EMIT, 250},
        { 17, HUFFMAN_EMIT, 251},
        { 18, HUFFMAN_EMIT, 251},
        { 19, HUFFMAN_EMIT, 251},
        { 20, HUFFMAN_EMIT, 251},
        { 21, HUFFMAN_EMIT, 251},
        { 22, HUFFMAN_EMIT, 251},
        { 23, HUFFMAN_EMIT, 251},
        { 24, HUFFMAN_EMIT, 251}
    },
    /* state 229 */ {
        { 17, HUFFMAN_EMIT, 252},
        { 18, HUFFMAN_EMIT, 252},
        { 19, HUFFMAN_EMIT, 252},
        { 20, HUFFMAN_EMIT, 252},
        { 21, HUFFMAN_EMIT, 252},
        { 22, HUFFMAN_EMIT, 252},
        { 23, HUFFMAN_EMIT, 252},
        { 24, HUFFMAN_EMIT, 252},
        { 17, HUFFMAN_EMIT, 253},
        { 18, HUFFMAN_EMIT, 253},
        { 19, HUFFMAN_EMIT, 253},
        { 20, HUFFMAN_EMIT, 253},
        { 21, HUFFMAN_EMIT, 253},
        { 22, HUFFMAN_EMIT, 253},
        { 23, HUFFMAN_EMIT, 253},
        { 24, HUFFMAN_EMIT, 253}
    },
    /* state 230 */ {
        { 17, HUFFMAN_EMIT, 254},
        { 18, HUFFMAN_EMIT, 254},
        { 19, HUFFMAN_EMIT, 254},
        { 20, HUFFMAN_EMIT, 254},
        { 21, HUFFMAN_EMIT, 254},
        { 22, HUFFMAN_EMIT, 254},
        { 23, HUFFMAN_EMIT, 254},
        { 24, HUFFMAN_EMIT, 254},
        { 25, HUFFMAN_EMIT,   2},
        { 26, HUFFMAN_EMIT,   2},
        { 27, HUFFMAN_EMIT,   2},
        { 28, HUFFMAN_EMIT,   2},
        { 25, HUFFMAN_EMIT,   3},
        { 26, HUFFMAN_EMIT,   3},
        { 27, HUFFMAN_EMIT,   3},
        { 28, HUFFMAN_EMIT,   3}
    },
    /* state 231 */ {
        { 25, HUFFMAN_EMIT,   4},
        { 26, HUFFMAN_EMIT,   4},
        { 27, HUFFMAN_EMIT,   4},
        { 28, HUFFMAN_EMIT,   4},
        { 25, HUFFMAN_EMIT,   5},
        { 26, HUFFMAN_EMIT,   5},
        { 27, HUFFMAN_EMIT,   5},
        { 28, HUFFMAN_EMIT,   5},
        { 25, HUFFMAN_EMIT,   6},
        { 26, HUFFMAN_EMIT,   6},
        { 27, HUFFMAN_EMIT,   6},
        { 28, HUFFMAN_EMIT,   6},
        { 25, HUFFMAN_EMIT,   7},
        { 26, HUFFMAN_EMIT,   7},
        { 27, HUFFMAN_EMIT,   7},
        { 28, HUFFMAN_EMIT,   7}
    },
    /* state 232 */ {
        { 25, HUFFMAN_EMIT,   8},
        { 26, HUFFMAN_EMIT,   8},
        { 27, HUFFMAN_EMIT,   8},
        { 28, HUFFMAN_EMIT,   8},
        { 25, HUFFMAN_EMIT,  11},
        { 26, HUFFMAN_EMIT,  11},
        { 27, HUFFMAN_EMIT,  11},
        { 28, HUFFMAN_EMIT,  11},
        { 25, HUFFMAN_EMIT,  12},
        { 26, HUFFMAN_EMIT,  12},
        { 27, HUFFMAN_EMIT,  12},
        { 28, HUFFMAN_EMIT,  12},
        { 25, HUFFMAN_EMIT,  14},
        { 26, HUFFMAN_EMIT,  14},
        { 27, HUFFMAN_EMIT,  14},
        { 28, HUFFMAN_EMIT,  14}
    },
    /* state 233 */ {
        { 25, HUFFMAN_EMIT,  15},
        { 26, HUFFMAN_EMIT,  15},
        { 27, HUFFMAN_EMIT,  15},
        { 28, HUFFMAN_EMIT,  15},
        { 25, HUFFMAN_EMIT,  16},
        { 26, HUFFMAN_EMIT,  16},
        { 27, HUFFMAN_EMIT,  16},
        { 28, HUFFMAN_EMIT,  16},
        { 25, HUFFMAN_EMIT,  17},
        { 26, HUFFMAN_EMIT,  17},
        { 27, HUFFMAN_EMIT,  17},
        { 28, HUFFMAN_EMIT,  17},
        { 25, HUFFMAN_EMIT,  18},
        { 26, HUFFMAN_EMIT,  18},
        { 27, HUFFMAN_EMIT,  18},
        { 28, HUFFMAN_EMIT,  18}
    },
    /* state 234 */ {
        { 25, HUFFMAN_EMIT,  19},
        { 26, HUFFMAN_EMIT,  19},
        { 27, HUFFMAN_EMIT,  19},
        { 28, HUFFMAN_EMIT,  19},
        { 25, HUFFMAN_EMIT,  20},
        { 26, HUFFMAN_EMIT,  20},
        { 27, HUFFMAN_EMIT,  20},
        { 28, HUFFMAN_EMIT,  20},
        { 25, HUFFMAN_EMIT,  21},
        { 26, HUFFMAN_EMIT,  21},
        { 27, HUFFMAN_EMIT,  21},
        { 28, HUFFMAN_EMIT,  21},
        { 25, HUFFMAN_EMIT,  23},
        { 26, HUFFMAN_EMIT,  23},
        { 27, HUFFMAN_EMIT,  23},
        { 28, HUFFMAN_EMIT,  23}
    },
    /* state 235 */ {
        { 25, HUFFMAN_EMIT,  24},
        { 26, HUFFMAN_EMIT,  24},
        { 27, HUFFMAN_EMIT,  24},
        { 28, HUFFMAN_EMIT,  24},
        { 25, HUFFMAN_EMIT,  25},
        { 26, HUFFMAN_EMIT,  25},
        { 27, HUFFMAN_EMIT,  25},
        { 28, HUFFMAN_EMIT,  25},
        { 25, HUFFMAN_EMIT,  26},
        { 26, HUFFMAN_EMIT,  26},
        { 27, HUFFMAN_EMIT,  26},
        { 28, HUFFMAN_EMIT,  26},
        { 25, HUFFMAN_EMIT,  27},
        { 26, HUFFMAN_EMIT,  27},
        { 27, HUFFMAN_EMIT,  27},
        { 28, HUFFMAN_EMIT,  27}
    },
    /* state 236 */ {
        { 25, HUFFMAN_EMIT,  28},
        { 26, HUFFMAN_EMIT,  28},
        { 27, HUFFMAN_EMIT,  28},
        { 28, HUFFMAN_EMIT,  28},
        { 25, HUFFMAN_EMIT,  29},
        { 26, HUFFMAN_EMIT,  29},
        { 27, HUFFMAN_EMIT,  29},
        { 28, HUFFMAN_EMIT,  29},
        { 25, HUFFMAN_EMIT,  30},
        { 26, HUFFMAN_EMIT,  30},
        { 27, HUFFMAN_EMIT,  30},
        { 28, HUFFMAN_EMIT,  30},
        { 25, HUFFMAN_EMIT,  31},
        { 26, HUFFMAN_EMIT,  31},
        { 27, HUFFMAN_EMIT,  31},
        { 28, HUFFMAN_EMIT,  31}
    },
    /* state 237 */ {
        { 25, HUFFMAN_EMIT, 127},
        { 26, HUFFMAN_EMIT, 127},
        { 27, HUFFMAN_EMIT, 127},
        { 28, HUFFMAN_EMIT, 127},
        { 25, HUFFMAN_EMIT, 220},
        { 26, HUFFMAN_EMIT, 220},
        { 27, HUFFMAN_EMIT, 220},
        { 28, HUFFMAN_EMIT, 220},
        { 25, HUFFMAN_EMIT, 249},
        { 26, HUFFMAN_EMIT, 249},
        { 27, HUFFMAN_EMIT, 249},
        { 28, HUFFMAN_EMIT, 249},
        {  0, HUFFMAN_EMIT,  10},
        {  0, HUFFMAN_EMIT,  13},
        {  0, HUFFMAN_EMIT,  22},
        {  0, HUFFMAN_FAIL,   0}
    },
    /* state 238 */ {
        { 17, HUFFMAN_EMIT, 192},
        { 18, HUFFMAN_EMIT, 192},
        { 19, HUFFMAN_EMIT, 192},
        { 20, HUFFMAN_EMIT, 192},
        { 21, HUFFMAN_EMIT, 192},
        { 22, HUFFMAN_EMIT, 192},
        { 23, HUFFMAN_EMIT, 192},
        { 24, HUFFMAN_EMIT, 192},
        { 17, HUFFMAN_EMIT, 193},
        { 18, HUFFMAN_EMIT, 193},
        { 19, HUFFMAN_EMIT, 193},
        { 20, HUFFMAN_EMIT, 193},
        { 21, HUFFMAN_EMIT, 193},
        { 22, HUFFMAN_EMIT, 193},
        { 23, HUFFMAN_EMIT, 193},
        { 24, HUFFMAN_EMIT, 193}
    },
    /* state 239 */ {
        { 17, HUFFMAN_EMIT, 200},
        { 18, HUFFMAN_EMIT, 200},
        { 19, HUFFMAN_EMIT, 200},
        { 20, HUFFMAN_EMIT, 200},
        { 21, HUFFMAN_EMIT, 200},
        { 22, HUFFMAN_EMIT, 200},
        { 23, HUFFMAN_EMIT, 200},
        { 24, HUFFMAN_EMIT, 200},
        { 17, HUFFMAN_EMIT, 201},
        { 18, HUFFMAN_EMIT, 201},
        { 19, HUFFMAN_EMIT, 201},
        { 20, HUFFMAN_EMIT, 201},
        { 21, HUFFMAN_EMIT, 201},
        { 22, HUFFMAN_EMIT, 201},
        { 23, HUFFMAN_EMIT, 201},
        { 24, HUFFMAN_EMIT, 201}
    },
    /* state 240 */ {
        { 17, HUFFMAN_EMIT, 202},
        { 18, HUFFMAN_EMIT, 202},
        { 19, HUFFMAN_EMIT, 202},
        { 20, HUFFMAN_EMIT, 202},
        { 21, HUFFMAN_EMIT, 202},
        { 22, HUFFMAN_EMIT, 202},
        { 23, HUFFMAN_EMIT, 202},
        { 24, HUFFMAN_EMIT, 202},
        { 17, HUFFMAN_EMIT, 205},
        { 18, HUFFMAN_EMIT, 205},
        { 19, HUFFMAN_EMIT, 205},
        { 20, HUFFMAN_EMIT, 205},
        { 21, HUFFMAN_EMIT, 205},
        { 22, HUFFMAN_EMIT, 205},
        { 23, HUFFMAN_EMIT, 205},
        { 24, HUFFMAN_EMIT, 205}
    },
    /* state 241 */ {
        { 17, HUFFMAN_EMIT, 210},
        { 18, HUFFMAN_EMIT, 210},
        { 19, HUFFMAN_EMIT, 210},
        { 20, HUFFMAN_EMIT, 210},
        { 21, HUFFMAN_EMIT, 210},
        { 22, HUFFMAN_EMIT, 210},
        { 23, HUFFMAN_EMIT, 210},
        { 24, HUFFMAN_EMIT, 210},
        { 17, HUFFMAN_EMIT, 213},
        { 18, HUFFMAN_EMIT, 213},
        { 19, HUFFMAN_EMIT, 213},
        { 20, HUFFMAN_EMIT, 213},
        { 21, HUFFMAN_EMIT, 213},
        { 22, HUFFMAN_EMIT, 213},
        { 23, HUFFMAN_EMIT, 213},
        { 24, HUFFMAN_EMIT, 213}
    },
    /* state 242 */ {
        { 17, HUFFMAN_EMIT, 218},
        { 18, HUFFMAN_EMIT, 218},
        { 19, HUFFMAN_EMIT, 218},
        { 20, HUFFMAN_EMIT, 218},
        { 21, HUFFMAN_EMIT, 218},
        { 22, HUFFMAN_EMIT, 218},
        { 23, HUFFMAN_EMIT, 218},
        { 24, HUFFMAN_EMIT, 218},
        { 17, HUFFMAN_EMIT, 219},
        { 18, HUFFMAN_EMIT, 219},
        { 19, HUFFMAN_EMIT, 219},
        { 20, HUFFMAN_EMIT, 219},
        { 21, HUFFMAN_EMIT, 219},
        { 22, HUFFMAN_EMIT, 219},
        { 23, HUFFMAN_EMIT, 219},
        { 24, HUFFMAN_EMIT, 219}
    },
    /* state 243 */ {
        { 17, HUFFMAN_EMIT, 238},
        { 18, HUFFMAN_EMIT, 238},
        { 19, HUFFMAN_EMIT, 238},
        { 20, HUFFMAN_EMIT, 238},
        { 21, HUFFMAN_EMIT, 238},
        { 22, HUFFMAN_EMIT, 238},
        { 23, HUFFMAN_EMIT, 238},
        { 24, HUFFMAN_EMIT, 238},
        { 17, HUFFMAN_EMIT, 240},
        { 18, HUFFMAN_EMIT, 240},
        { 19, HUFFMAN_EMIT, 240},
        { 20, HUFFMAN_EMIT, 240},
        { 21, HUFFMAN_EMIT, 240},
        { 22, HUFFMAN_EMIT, 240},
        { 23, HUFFMAN_EMIT, 240},
        { 24, HUFFMAN_EMIT, 240}
    },
    /* state 244 */ {
        { 17, HUFFMAN_EMIT, 242},
        { 18, HUFFMAN_EMIT, 242},
        { 19, HUFFMAN_EMIT, 242},
        { 20, HUFFMAN_EMIT, 242},
        { 21, HUFFMAN_EMIT, 242},
        { 22, HUFFMAN_EMIT, 242},
        { 23, HUFFMAN_EMIT, 242},
        { 24, HUFFMAN_EMIT, 242},
        { 17, HUFFMAN_EMIT, 243},
        { 18, HUFFMAN_EMIT, 243},
        { 19, HUFFMAN_EMIT, 243},
        { 20, HUFFMAN_EMIT, 243},
        { 21, HUFFMAN_EMIT, 243},
        { 22, HUFFMAN_EMIT, 243},
        { 23, HUFFMAN_EMIT, 243},
        { 24, HUFFMAN_EMIT, 243}
    },
    /* state 245 */ {
        { 17, HUFFMAN_EMIT, 255},
        { 18, HUFFMAN_EMIT, 255},
        { 19, HUFFMAN_EMIT, 255},
        { 20, HUFFMAN_EMIT, 255},
        { 21, HUFFMAN_EMIT, 255},
        { 22, HUFFMAN_EMIT, 255},
        { 23, HUFFMAN_EMIT, 255},
        { 24, HUFFMAN_EMIT, 255},
        { 25, HUFFMAN_EMIT, 203},
        { 26, HUFFMAN_EMIT, 203},
        { 27, HUFFMAN_EMIT, 203},
        { 28, HUFFMAN_EMIT, 203},
        { 25, HUFFMAN_EMIT, 204},
        { 26, HUFFMAN_EMIT, 204},
        { 27, HUFFMAN_EMIT, 204},
        { 28, HUFFMAN_EMIT, 204}
    },
    /* state 246 */ {
        { 25, HUFFMAN_EMIT, 211},
        { 26, HUFFMAN_EMIT, 211},
        { 27, HUFFMAN_EMIT, 211},
        { 28, HUFFMAN_EMIT, 211},
        { 25, HUFFMAN_EMIT, 212},
        { 26, HUFFMAN_EMIT, 212},
        { 27, HUFFMAN_EMIT, 212},
        { 28, HUFFMAN_EMIT, 212},
        { 25, HUFFMAN_EMIT, 214},
        { 26, HUFFMAN_EMIT, 214},
        { 27, HUFFMAN_EMIT, 214},
        { 28, HUFFMAN_EMIT, 214},
        { 25, HUFFMAN_EMIT, 221},
        { 26, HUFFMAN_EMIT, 221},
        { 27, HUFFMAN_EMIT, 221},
        { 28, HUFFMAN_EMIT, 221}
    },
    /* state 247 */ {
        { 25, HUFFMAN_EMIT, 222},
        { 26, HUFFMAN_EMIT, 222},
        { 27, HUFFMAN_EMIT, 222},
        { 28, HUFFMAN_EMIT, 222},
        { 25, HUFFMAN_EMIT, 223},
        { 26, HUFFMAN_EMIT, 223},
        { 27, HUFFMAN_EMIT, 223},
        { 28, HUFFMAN_EMIT, 223},
        { 25, HUFFMAN_EMIT, 241},
        { 26, HUFFMAN_EMIT, 241},
        { 27, HUFFMAN_EMIT, 241},
        { 28, HUFFMAN_EMIT, 241},
        { 25, HUFFMAN_EMIT, 244},
        { 26, HUFFMAN_EMIT, 244},
        { 27, HUFFMAN_EMIT, 244},
        { 28, HUFFMAN_EMIT, 244}
    },
    /* state 248 */ {
        { 25, HUFFMAN_EMIT, 245},
        { 26, HUFFMAN_EMIT, 245},
        { 27, HUFFMAN_EMIT, 245},
        { 28, HUFFMAN_EMIT, 245},
        { 25, HUFFMAN_EMIT, 246},
        { 26, HUFFMAN_EMIT, 246},
        { 27, HUFFMAN_EMIT, 246},
        { 28, HUFFMAN_EMIT, 246},
        { 25, HUFFMAN_EMIT, 247},
        { 26, HUFFMAN_EMIT, 247},
        { 27, HUFFMAN_EMIT, 247},
        { 28, HUFFMAN_EMIT, 247},
        { 25, HUFFMAN_EMIT, 248},
        { 26, HUFFMAN_EMIT, 248},
        { 27, HUFFMAN_EMIT, 248},
        { 28, HUFFMAN_EMIT, 248}
    },
    /* state 249 */ {
        { 25, HUFFMAN_EMIT, 250},
        { 26, HUFFMAN_EMIT, 250},
        { 27, HUFFMAN_EMIT, 250},
        { 28, HUFFMAN_EMIT, 250},
        { 25, HUFFMAN_EMIT, 251},
        { 26, HUFFMAN_EMIT, 251},
        { 27, HUFFMAN_EMIT, 251},
        { 28, HUFFMAN_EMIT, 251},
        { 25, HUFFMAN_EMIT, 252},
        { 26, HUFFMAN_EMIT, 252},
        { 27, HUFFMAN_EMIT, 252},
        { 28, HUFFMAN_EMIT, 252},
        { 25, HUFFMAN_EMIT, 253},
        { 26, HUFFMAN_EMIT, 253},
        { 27, HUFFMAN_EMIT, 253},
        { 28, HUFFMAN_EMIT, 253}
    },
    /* state 250 */ {
        { 25, HUFFMAN_EMIT, 254},
        { 26, HUFFMAN_EMIT, 254},
        { 27, HUFFMAN_EMIT, 254},
        { 28, HUFFMAN_EMIT, 254},
        { 29, HUFFMAN_EMIT,   2},
        { 30, HUFFMAN_EMIT,   2},
        { 29, HUFFMAN_EMIT,   3},
        { 30, HUFFMAN_EMIT,   3},
        { 29, HUFFMAN_EMIT,   4},
        { 30, HUFFMAN_EMIT,   4},
        { 29, HUFFMAN_EMIT,   5},
        { 30, HUFFMAN_EMIT,   5},
        { 29, HUFFMAN_EMIT,   6},
        { 30, HUFFMAN_EMIT,   6},
        { 29, HUFFMAN_EMIT,   7},
        { 30, HUFFMAN_EMIT,   7}
    },
    /* state 251 */ {
        { 29, HUFFMAN_EMIT,   8},
        { 30, HUFFMAN_EMIT,   8},
        { 29, HUFFMAN_EMIT,  11},
        { 30, HUFFMAN_EMIT,  11},
        { 29, HUFFMAN_EMIT,  12},
        { 30, HUFFMAN_EMIT,  12},
        { 29, HUFFMAN_EMIT,  14},
        { 30, HUFFMAN_EMIT,  14},
        { 29, HUFFMAN_EMIT,  15},
        { 30, HUFFMAN_EMIT,  15},
        { 29, HUFFMAN_EMIT,  16},
        { 30, HUFFMAN_EMIT,  16},
        { 29, HUFFMAN_EMIT,  17},
        { 30, HUFFMAN_EMIT,  17},
        { 29, HUFFMAN_EMIT,  18},
        { 30, HUFFMAN_EMIT,  18}
    },
    /* state 252 */ {
        { 29, HUFFMAN_EMIT,  19},
        { 30, HUFFMAN_EMIT,  19},
        { 29, HUFFMAN_EMIT,  20},
        { 30, HUFFMAN_EMIT,  20},
        { 29, HUFFMAN_EMIT,  21},
        { 30, HUFFMAN_EMIT,  21},
        { 29, HUFFMAN_EMIT,  23},
        { 30, HUFFMAN_EMIT,  23},
        { 29, HUFFMAN_EMIT,  24},
        { 30, HUFFMAN_EMIT,  24},
        { 29, HUFFMAN_EMIT,  25},
        { 30, HUFFMAN_EMIT,  25},
        { 29, HUFFMAN_EMIT,  26},
        { 30, HUFFMAN_EMIT,  26},
        { 29, HUFFMAN_EMIT,  27},
        { 30, HUFFMAN_EMIT,  27}
    },
    /* state 253 */ {
        { 29, HUFFMAN_EMIT,  28},
        { 30, HUFFMAN_EMIT,  28},
        { 29, HUFFMAN_EMIT,  29},
        { 30, HUFFMAN_EMIT,  29},
        { 29, HUFFMAN_EMIT,  30},
        { 30, HUFFMAN_EMIT,  30},
        { 29, HUFFMAN_EMIT,  31},
        { 30, HUFFMAN_EMIT,  31},
        { 29, HUFFMAN_EMIT, 127},
        { 30, HUFFMAN_EMIT, 127},
        { 29, HUFFMAN_EMIT, 220},
        { 30, HUFFMAN_EMIT, 220},
        { 29, HUFFMAN_EMIT, 249},
        { 30, HUFFMAN_EMIT, 249},
        {254, HUFFMAN_NONE,   0},
        {255, HUFFMAN_NONE,   0}
    },
    /* state 254 */ {
        { 17, HUFFMAN_EMIT,  10},
        { 18, HUFFMAN_EMIT,  10},
        { 19, HUFFMAN_EMIT,  10},
        { 20, HUFFMAN_EMIT,  10},
        { 21, HUFFMAN_EMIT,  10},
        { 22, HUFFMAN_EMIT,  10},
        { 23, HUFFMAN_EMIT,  10},
        { 24, HUFFMAN_EMIT,  10},
        { 17, HUFFMAN_EMIT,  13},
        { 18, HUFFMAN_EMIT,  13},
        { 19, HUFFMAN_EMIT,  13},
        { 20, HUFFMAN_EMIT,  13},
        { 21, HUFFMAN_EMIT,  13},
        { 22, HUFFMAN_EMIT,  13},
        { 23, HUFFMAN_EMIT,  13},
        { 24, HUFFMAN_EMIT,  13}
    },
    /* state 255 */ {
        { 17, HUFFMAN_EMIT,  22},
        { 18, HUFFMAN_EMIT,  22},
        { 19, HUFFMAN_EMIT,  22},
        { 20, HUFFMAN_EMIT,  22},
        { 21, HUFFMAN_EMIT,  22},
        { 22, HUFFMAN_EMIT,  22},
        { 23, HUFFMAN_EMIT,  22},
        { 24, HUFFMAN_EMIT,  22},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0},
        {  0, HUFFMAN_FAIL,   0}
    },
};

static const bool kHuffmanDecodeAccepting[256] = {
    true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    true, false, false, false, false, false, false, false, true, false, false, false, true, false, true, false,
    true, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false,
    true, false, false, true, false, false, true, false, false, false, true, false, true, true, false, true,
    false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false,
    true, false, false, false, false, false, false, false, true, false, false, false, true, false, false, false,
    false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false,
    false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, true, true, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, true,
};

// clang-format on

// ==================== 静态表 (RFC 7541 Appendix A) ====================

static const Http2HeaderField kStaticTableEntries[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

constexpr size_t kStaticTableSize = sizeof(kStaticTableEntries) / sizeof(kStaticTableEntries[0]);

// ==================== HpackStaticTable ====================

HpackStaticTable::HpackStaticTable()
{
    m_table.reserve(kStaticTableSize);
    m_name_to_indices.reserve(kStaticTableSize);
    for (size_t i = 0; i < kStaticTableSize; ++i) {
        m_table.push_back(kStaticTableEntries[i]);
        // 建立 name -> [indices] 索引，避免热路径构造临时 pair<string,string>
        m_name_to_indices[kStaticTableEntries[i].name].push_back(i + 1);
    }
}

const HpackStaticTable& HpackStaticTable::instance()
{
    static HpackStaticTable instance;
    return instance;
}

const Http2HeaderField* HpackStaticTable::get(size_t index) const
{
    if (index == 0 || index > m_table.size()) {
        return nullptr;
    }
    return &m_table[index - 1];
}

std::pair<size_t, bool> HpackStaticTable::find(const std::string& name, const std::string& value) const
{
    auto nit = m_name_to_indices.find(name);
    if (nit == m_name_to_indices.end()) {
        return {0, false};  // 未找到
    }

    const auto& indices = nit->second;
    for (size_t index : indices) {
        if (m_table[index - 1].value == value) {
            return {index, false};  // 完全匹配
        }
    }
    return {indices.front(), true};  // 只匹配名称
}


// ==================== HpackDynamicTable ====================

HpackDynamicTable::HpackDynamicTable(size_t max_size)
    : m_max_size(max_size)
{
}

void HpackDynamicTable::add(const Http2HeaderField& field)
{
    size_t entry_size = field.size();

    // 如果条目大于最大大小，清空表
    if (entry_size > m_max_size) {
        clear();
        return;
    }

    // 驱逐条目直到有足够空间
    while (m_current_size + entry_size > m_max_size) {
        evict();
    }

    // 添加到环形缓冲区头部
    if (m_count < m_ring.size()) {
        // 缓冲区有空位，直接覆盖
        m_ring[m_head] = field;
    } else {
        // 缓冲区满了，需要扩展：线性化为逻辑顺序（最新→最旧），再追加新条目
        std::vector<Http2HeaderField> new_ring;
        size_t new_cap = m_count == 0 ? 8 : m_count * 2;
        new_ring.resize(new_cap);
        // slot 0 = 新条目
        new_ring[0] = field;
        // slot 1..m_count = 旧的逻辑顺序（get(0)..get(m_count-1)）
        for (size_t i = 0; i < m_count; ++i) {
            size_t old_idx = (m_head + m_ring.size() - 1 - i) % m_ring.size();
            new_ring[1 + i] = std::move(m_ring[old_idx]);
        }
        m_ring = std::move(new_ring);
        m_head = (m_count + 1) % m_ring.size();
        m_count++;
        m_current_size += entry_size;
        return;
    }
    m_head = (m_head + 1) % m_ring.size();
    m_count++;
    m_current_size += entry_size;
}

const Http2HeaderField* HpackDynamicTable::get(size_t index) const
{
    if (index >= m_count) {
        return nullptr;
    }
    // index 0 = 最新条目 = ring[(head - 1 + cap) % cap]
    // index i = ring[(head - 1 - i + cap) % cap]
    size_t cap = m_ring.size();
    size_t pos = (m_head + cap - 1 - index) % cap;
    return &m_ring[pos];
}

std::pair<size_t, bool> HpackDynamicTable::find(const std::string& name, const std::string& value) const
{
    size_t name_match = 0;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& entry = *get(i);
        if (entry.name == name) {
            if (entry.value == value) {
                return {i, false};  // 完全匹配
            }
            if (name_match == 0) {
                name_match = i + 1;  // 记录第一个名称匹配（+1 表示找到）
            }
        }
    }
    if (name_match > 0) {
        return {name_match - 1, true};  // 只匹配名称
    }
    return {SIZE_MAX, false};  // 未找到：用 SIZE_MAX 区分于索引 0 的完全匹配
}

void HpackDynamicTable::setMaxSize(size_t max_size)
{
    m_max_size = max_size;
    while (m_current_size > m_max_size) {
        evict();
    }
}

void HpackDynamicTable::clear()
{
    m_ring.clear();
    m_head = 0;
    m_count = 0;
    m_current_size = 0;
}

void HpackDynamicTable::evict()
{
    if (m_count == 0) {
        return;
    }
    // 最旧条目 = get(m_count - 1) = ring[(head - count + cap) % cap]
    size_t cap = m_ring.size();
    size_t oldest = (m_head + cap - m_count) % cap;
    m_current_size -= m_ring[oldest].size();
    m_ring[oldest] = Http2HeaderField{};  // 释放内存
    m_count--;
}


// ==================== HpackHuffman ====================

std::string HpackHuffman::encode(const std::string& input)
{
    std::string output;
    output.reserve(input.size());  // Huffman 编码后通常 ≤ 原始长度
    uint64_t buffer = 0;
    int bits_in_buffer = 0;
    
    for (unsigned char c : input) {
        const HuffmanCode& code = kHuffmanCodes[c];
        buffer = (buffer << code.bits) | code.code;
        bits_in_buffer += code.bits;
        
        while (bits_in_buffer >= 8) {
            bits_in_buffer -= 8;
            output.push_back(static_cast<char>((buffer >> bits_in_buffer) & 0xFF));
        }
    }
    
    // 填充 EOS 符号的前缀
    if (bits_in_buffer > 0) {
        buffer = (buffer << (8 - bits_in_buffer)) | (0xFF >> bits_in_buffer);
        output.push_back(static_cast<char>(buffer & 0xFF));
    }
    
    return output;
}

size_t HpackHuffman::encodedLength(const std::string& input)
{
    size_t bits = 0;
    for (unsigned char c : input) {
        bits += kHuffmanCodes[c].bits;
    }
    return (bits + 7) / 8;
}

std::expected<std::string, Http2ErrorCode> HpackHuffman::decode(const uint8_t* data, size_t length)
{
    std::string output;
    output.reserve(length * 2);  // Huffman 解码后通常更长

    uint8_t state = 0;

    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = data[i];

        // 高 4 位
        const auto& hi = kHuffmanDecodeTable[state][byte >> 4];
        if (hi.flags & HUFFMAN_FAIL) {
            return std::unexpected(Http2ErrorCode::CompressionError);
        }
        if (hi.flags & HUFFMAN_EMIT) {
            output.push_back(static_cast<char>(hi.symbol));
        }
        state = hi.state;

        // 低 4 位
        const auto& lo = kHuffmanDecodeTable[state][byte & 0x0F];
        if (lo.flags & HUFFMAN_FAIL) {
            return std::unexpected(Http2ErrorCode::CompressionError);
        }
        if (lo.flags & HUFFMAN_EMIT) {
            output.push_back(static_cast<char>(lo.symbol));
        }
        state = lo.state;
    }

    // 检查是否在合法的结束状态（EOS 填充）
    if (!kHuffmanDecodeAccepting[state]) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }

    return output;
}


// ==================== HpackEncoder ====================

HpackEncoder::HpackEncoder(size_t max_table_size)
    : m_dynamic_table(max_table_size)
{
}

void HpackEncoder::encodeInteger(uint32_t value, uint8_t prefix_bits, uint8_t prefix, std::string& output)
{
    uint8_t max_prefix = (1 << prefix_bits) - 1;
    
    if (value < max_prefix) {
        output.push_back(static_cast<char>(prefix | value));
    } else {
        output.push_back(static_cast<char>(prefix | max_prefix));
        value -= max_prefix;
        while (value >= 128) {
            output.push_back(static_cast<char>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        output.push_back(static_cast<char>(value));
    }
}

void HpackEncoder::encodeString(const std::string& str, bool use_huffman, std::string& output)
{
    if (use_huffman) {
        std::string encoded = HpackHuffman::encode(str);
        encodeInteger(encoded.size(), 7, 0x80, output);  // H=1
        output.append(encoded);
    } else {
        encodeInteger(str.size(), 7, 0x00, output);  // H=0
        output.append(str);
    }
}

void HpackEncoder::encodeIndexed(size_t index, std::string& output)
{
    // 索引头部字段: 1xxxxxxx
    encodeInteger(index, 7, 0x80, output);
}

void HpackEncoder::encodeLiteralIndexed(size_t name_index, const std::string& value, std::string& output)
{
    // 带索引的字面头部字段: 01xxxxxx
    encodeInteger(name_index, 6, 0x40, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralIndexed(const std::string& name, const std::string& value, std::string& output)
{
    // 带索引的字面头部字段，新名称: 01000000
    output.push_back(0x40);
    encodeString(name, m_use_huffman, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralWithoutIndexing(size_t name_index, const std::string& value, std::string& output)
{
    // 不带索引的字面头部字段: 0000xxxx
    encodeInteger(name_index, 4, 0x00, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralWithoutIndexing(const std::string& name, const std::string& value, std::string& output)
{
    // 不带索引的字面头部字段，新名称: 00000000
    output.push_back(0x00);
    encodeString(name, m_use_huffman, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralNeverIndexed(size_t name_index, const std::string& value, std::string& output)
{
    // 永不索引的字面头部字段: 0001xxxx
    encodeInteger(name_index, 4, 0x10, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralNeverIndexed(const std::string& name, const std::string& value, std::string& output)
{
    // 永不索引的字面头部字段，新名称: 00010000
    output.push_back(0x10);
    encodeString(name, m_use_huffman, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::setMaxTableSize(size_t size)
{
    m_dynamic_table.setMaxSize(size);
    m_table_size_update_pending = true;
    m_pending_table_size = size;
}


void HpackEncoder::encodeField(const Http2HeaderField& field, std::string& output)
{
    const auto& static_table = HpackStaticTable::instance();
    
    // 首先在静态表中查找
    auto [static_idx, static_name_only] = static_table.find(field.name, field.value);
    
    if (static_idx > 0 && !static_name_only) {
        // 静态表完全匹配
        encodeIndexed(static_idx, output);
        return;
    }
    
    // 在动态表中查找
    auto [dyn_idx, dyn_name_only] = m_dynamic_table.find(field.name, field.value);

    if (dyn_idx != SIZE_MAX && !dyn_name_only) {
        // 动态表完全匹配
        size_t index = static_table.size() + dyn_idx + 1;
        encodeIndexed(index, output);
        return;
    }

    // 需要字面编码
    // 敏感头部不索引
    bool sensitive = (field.name == "authorization" ||
                      field.name == "cookie" ||
                      field.name == "set-cookie" ||
                      field.name == "proxy-authorization");

    if (sensitive) {
        if (static_idx > 0) {
            encodeLiteralNeverIndexed(static_idx, field.value, output);
        } else {
            encodeLiteralNeverIndexed(field.name, field.value, output);
        }
    } else {
        // 添加到动态表
        if (static_idx > 0) {
            encodeLiteralIndexed(static_idx, field.value, output);
        } else if (dyn_idx != SIZE_MAX && dyn_name_only) {
            size_t index = static_table.size() + dyn_idx + 1;
            encodeLiteralIndexed(index, field.value, output);
        } else {
            encodeLiteralIndexed(field.name, field.value, output);
        }
        m_dynamic_table.add(field);
    }
}

void HpackEncoder::encodeFieldStateless(const Http2HeaderField& field, std::string& output)
{
    const auto& static_table = HpackStaticTable::instance();
    auto [static_idx, static_name_only] = static_table.find(field.name, field.value);

    if (static_idx > 0 && !static_name_only) {
        encodeIndexed(static_idx, output);
        return;
    }

    if (static_idx > 0) {
        encodeLiteralWithoutIndexing(static_idx, field.value, output);
        return;
    }

    encodeLiteralWithoutIndexing(field.name, field.value, output);
}

std::string HpackEncoder::encode(const std::vector<Http2HeaderField>& headers)
{
    std::string output;
    output.reserve(headers.size() * 32);  // 粗略估算每个头部字段 ~32 字节
    
    // 如果有待处理的表大小更新
    if (m_table_size_update_pending) {
        // 动态表大小更新: 001xxxxx
        encodeInteger(m_pending_table_size, 5, 0x20, output);
        m_table_size_update_pending = false;
    }
    
    for (const auto& field : headers) {
        encodeField(field, output);
    }
    
    return output;
}

std::string HpackEncoder::encodeStateless(const std::vector<Http2HeaderField>& headers)
{
    std::string output;
    output.reserve(headers.size() * 32);

    for (const auto& field : headers) {
        encodeFieldStateless(field, output);
    }

    return output;
}


// ==================== HpackDecoder ====================

HpackDecoder::HpackDecoder(size_t max_table_size)
    : m_dynamic_table(max_table_size)
    , m_max_table_size(max_table_size)
{
}

std::expected<uint32_t, Http2ErrorCode> HpackDecoder::decodeInteger(const uint8_t*& data, const uint8_t* end, uint8_t prefix_bits)
{
    if (data >= end) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }
    
    uint8_t max_prefix = (1 << prefix_bits) - 1;
    uint32_t value = (*data++) & max_prefix;
    
    if (value < max_prefix) {
        return value;
    }
    
    uint32_t m = 0;
    while (data < end) {
        uint8_t b = *data++;
        value += (b & 0x7F) << m;
        m += 7;
        
        if ((b & 0x80) == 0) {
            return value;
        }
        
        if (m > 28) {
            return std::unexpected(Http2ErrorCode::CompressionError);
        }
    }
    
    return std::unexpected(Http2ErrorCode::CompressionError);
}

std::expected<std::string, Http2ErrorCode> HpackDecoder::decodeString(const uint8_t*& data, const uint8_t* end)
{
    if (data >= end) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }
    
    bool huffman = (*data & 0x80) != 0;
    auto length_result = decodeInteger(data, end, 7);
    if (!length_result) {
        return std::unexpected(length_result.error());
    }
    
    uint32_t length = *length_result;
    if (data + length > end) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }
    
    std::string result;
    if (huffman) {
        auto decode_result = HpackHuffman::decode(data, length);
        if (!decode_result) {
            return std::unexpected(decode_result.error());
        }
        result = std::move(*decode_result);
    } else {
        result.assign(reinterpret_cast<const char*>(data), length);
    }
    
    data += length;
    return result;
}

const Http2HeaderField* HpackDecoder::getField(size_t index) const
{
    const auto& static_table = HpackStaticTable::instance();
    
    if (index == 0) {
        return nullptr;
    }
    
    if (index <= static_table.size()) {
        return static_table.get(index);
    }
    
    size_t dyn_index = index - static_table.size() - 1;
    return m_dynamic_table.get(dyn_index);
}

void HpackDecoder::setMaxTableSize(size_t size)
{
    m_max_table_size = size;
    m_dynamic_table.setMaxSize(size);
}

void HpackDecoder::setMaxHeaderListSize(size_t size)
{
    m_max_header_list_size = size;
}


std::expected<std::vector<Http2HeaderField>, Http2ErrorCode> HpackDecoder::decode(const uint8_t* data, size_t length)
{
    std::vector<Http2HeaderField> headers;
    // 每个 header block 通常包含多个小字段，预留容量减少 push_back 扩容开销。
    headers.reserve(std::min<size_t>(length / 16 + 4, 64));
    const uint8_t* end = data + length;
    size_t header_list_size = 0;
    
    while (data < end) {
        uint8_t byte = *data;
        
        if (byte & 0x80) {
            // 索引头部字段: 1xxxxxxx
            auto index_result = decodeInteger(data, end, 7);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }
            
            const Http2HeaderField* field = getField(*index_result);
            if (!field) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            header_list_size += field->size();
            if (header_list_size > m_max_header_list_size) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            
            headers.push_back(*field);
        }
        else if (byte & 0x40) {
            // 带索引的字面头部字段: 01xxxxxx
            auto index_result = decodeInteger(data, end, 6);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }
            
            std::string name;
            if (*index_result > 0) {
                const Http2HeaderField* field = getField(*index_result);
                if (!field) {
                    return std::unexpected(Http2ErrorCode::CompressionError);
                }
                name = field->name;
            } else {
                auto name_result = decodeString(data, end);
                if (!name_result) {
                    return std::unexpected(name_result.error());
                }
                name = std::move(*name_result);
            }
            
            auto value_result = decodeString(data, end);
            if (!value_result) {
                return std::unexpected(value_result.error());
            }
            
            Http2HeaderField field{std::move(name), std::move(*value_result)};
            header_list_size += field.size();
            if (header_list_size > m_max_header_list_size) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            m_dynamic_table.add(field);
            headers.push_back(std::move(field));
        }
        else if (byte & 0x20) {
            // 动态表大小更新: 001xxxxx
            auto size_result = decodeInteger(data, end, 5);
            if (!size_result) {
                return std::unexpected(size_result.error());
            }
            
            if (*size_result > m_max_table_size) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            
            m_dynamic_table.setMaxSize(*size_result);
        }
        else {
            // 不带索引或永不索引的字面头部字段: 0000xxxx 或 0001xxxx
            bool never_index = (byte & 0x10) != 0;
            (void)never_index;  // 解码时不需要区分
            
            auto index_result = decodeInteger(data, end, 4);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }
            
            std::string name;
            if (*index_result > 0) {
                const Http2HeaderField* field = getField(*index_result);
                if (!field) {
                    return std::unexpected(Http2ErrorCode::CompressionError);
                }
                name = field->name;
            } else {
                auto name_result = decodeString(data, end);
                if (!name_result) {
                    return std::unexpected(name_result.error());
                }
                name = std::move(*name_result);
            }
            
            auto value_result = decodeString(data, end);
            if (!value_result) {
                return std::unexpected(value_result.error());
            }

            Http2HeaderField field{std::move(name), std::move(*value_result)};
            header_list_size += field.size();
            if (header_list_size > m_max_header_list_size) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            headers.push_back(std::move(field));
            // 不添加到动态表
        }
    }
    
    return headers;
}

} // namespace galay::http2
