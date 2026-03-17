#ifndef __SYLAR_ENDIAN_H__
#define __SYLAR_ENDIAN_H__

#define SYLAR_LITTLE_INDIAN 1
#define SYLAR_BIG_INDIAN 2

#include <byteswap.h>
#include <stdint.h>
#include <type_traits>
namespace sylar
{
template <class T>
typename std::enable_if<sizeof(T) == sizeof(uint64_t), T>::type byteswap(T value)
{
    return bswap_64((uint64_t)value);
}

template <class T>
typename std::enable_if<sizeof(T) == sizeof(uint32_t), T>::type byteswap(T value)
{
    return bswap_32((uint32_t)value);
}

template <class T>
typename std::enable_if<sizeof(T) == sizeof(uint16_t), T>::type byteswap(T value)
{
    return bswap_16((uint16_t)value);
}

#if BYTE_ORDER == BIG_ENDIAN
#define SYLAR_BYTE_ORDER SYLAR_BIG_INDIAN
#else
#define SYLAR_BYTE_ORDER SYLAR_LITTLE_INDIAN
#endif

#if SYLAR_BYTE_ORDER == SYLAR_BIG_INDIAN
//只有当前机器是小端序的时候才会反转一下
template <class T>
T byteswapOnLittleEndian(T value)
{
    return value;
}

template <class T>
T byteswapOnBigEndian(T value)
{
    return byteswap(value);
}
#else
template <class T>
T byteswapOnLittleEndian(T value)
{
    return byteswap(value);
}

template <class T>
T byteswapOnBigEndian(T value)
{
    return value;
}
#endif

} // namespace sylar
#endif