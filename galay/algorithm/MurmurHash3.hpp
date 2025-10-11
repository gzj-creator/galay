#ifndef GALAY_MURMURHASH3_HPP
#define GALAY_MURMURHASH3_HPP


#include <stdint.h>


#if defined(_MSC_VER) && (_MSC_VER < 1600)

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;

// Other compilers

#else	// defined(_MSC_VER)

#endif // !defined(_MSC_VER)

#if defined(_MSC_VER)

#define FORCE_INLINE	__forceinline

#include <stdlib.h>

#define ROTL32(x,y)	_rotl(x,y)
#define ROTL64(x,y)	_rotl64(x,y)

#define BIG_CONSTANT(x) (x)

// Other compilers

#else	// defined(_MSC_VER)

#define	FORCE_INLINE inline __attribute__((always_inline))

inline uint32_t rotl32 ( uint32_t x, int8_t r )
{
  return (x << r) | (x >> (32 - r));
}

inline uint64_t rotl64 ( uint64_t x, int8_t r )
{
  return (x << r) | (x >> (64 - r));
}

#define	ROTL32(x,y)	rotl32(x,y)
#define ROTL64(x,y)	rotl64(x,y)

#define BIG_CONSTANT(x) (x##LLU)

#endif // !defined(_MSC_VER)


namespace galay::algorithm
{

/**
 * @brief MurmurHash3 x86 32位哈希函数
 * @details 适用于32位系统的快速非加密哈希算法，常用于哈希表、布隆过滤器等
 * @param key 待哈希的数据指针
 * @param len 数据长度（字节）
 * @param seed 哈希种子值，用于生成不同的哈希序列
 * @param out 输出缓冲区指针，需要至少4字节空间（存储uint32_t）
 */
void murmurHash3_x86_32  ( const void * key, int len, uint32_t seed, void * out );

/**
 * @brief MurmurHash3 x86 128位哈希函数
 * @details 适用于32位系统的128位哈希算法，提供更低的碰撞率
 * @param key 待哈希的数据指针
 * @param len 数据长度（字节）
 * @param seed 哈希种子值
 * @param out 输出缓冲区指针，需要至少16字节空间（存储4个uint32_t）
 */
void murmurHash3_x86_128 ( const void * key, int len, uint32_t seed, void * out );

/**
 * @brief MurmurHash3 x64 128位哈希函数
 * @details 适用于64位系统的128位哈希算法，性能优于x86版本
 * @param key 待哈希的数据指针
 * @param len 数据长度（字节）
 * @param seed 哈希种子值
 * @param out 输出缓冲区指针，需要至少16字节空间（存储2个uint64_t）
 */
void murmurHash3_x64_128 ( const void * key, int len, uint32_t seed, void * out );



}

#endif