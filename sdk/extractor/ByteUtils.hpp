#pragma once

#include <stdint.h>

namespace sdk_utils {

extern "C" {

typedef char char_t;

/**
 * @brief 用于进行判断主机字节序的联合体。
 * @note
 * 小端：低地址存放低字节，高地址存放高字节；
 * 大端：高地址存放低字节，低地址存放高字节；
 * 网络字节序是大端。
 */
static union {
    char_t xct_order[4];
    uint32_t xut_order;
} xhost_order = { { 'L', '?', '?', 'B' } };

#define X_IS_LITTLE_ENDIAN ('L' == (char_t)xhost_order.xut_order)
#define X_IS_BIG_ENDIAN    ('B' == (char_t)xhost_order.xut_order)

/**********************************************************/
/**
 * @brief 字节序转换：16 位整数从 网络字节序 转成 主机字节序。
 */
uint16_t vx_ntohs(uint16_t xut_short)
{
    if (X_IS_LITTLE_ENDIAN)
        return ((xut_short << 8) | (xut_short >> 8));
    return xut_short;
}

/**********************************************************/
/**
 * @brief 字节序转换：16 位整数从 主机字节序 转成 网络字节序。
 */
uint16_t vx_htons(uint16_t xut_short)
{
    if (X_IS_LITTLE_ENDIAN)
        return ((xut_short << 8) | (xut_short >> 8));
    return xut_short;
}

/**********************************************************/
/**
 * @brief 字节序转换：32 位整数从 网络字节序 转成 主机字节序。
 */
uint32_t vx_ntohl(uint32_t xut_long)
{
    if (X_IS_LITTLE_ENDIAN)
        return (((xut_long) << 24) | ((xut_long & 0x0000FF00) << 8) | ((xut_long & 0x00FF0000) >> 8)
            | ((xut_long) >> 24));
    return xut_long;
}

/**********************************************************/
/**
 * @brief 字节序转换：32 位整数从 主机字节序 转成 网络字节序。
 */
uint32_t vx_htonl(uint32_t xut_long)
{
    if (X_IS_LITTLE_ENDIAN)
        return (((xut_long) << 24) | ((xut_long & 0x0000FF00) << 8) | ((xut_long & 0x00FF0000) >> 8)
            | ((xut_long) >> 24));
    return xut_long;
}

/**********************************************************/
/**
 * @brief 字节序转换：64 位整数从 网络字节序 转成 主机字节序。
 */
uint64_t vx_ntohll(uint64_t xult_llong)
{
    if (X_IS_LITTLE_ENDIAN)
        return (((xult_llong) << 56) | ((xult_llong & 0x000000000000FF00) << 40)
            | ((xult_llong & 0x0000000000FF0000) << 24) | ((xult_llong & 0x00000000FF000000) << 8)
            | ((xult_llong & 0x000000FF00000000) >> 8) | ((xult_llong & 0x0000FF0000000000) >> 24)
            | ((xult_llong & 0x00FF000000000000) >> 40) | ((xult_llong) >> 56));
    return xult_llong;
}

uint16_t U16_AT(const uint8_t *ptr)
{
    return ptr[0] << 8 | ptr[1];
}

uint32_t U32_AT(const uint8_t *ptr)
{
    return ptr[0] << 24 | ptr[1] << 16 | ptr[2] << 8 | ptr[3];
}

uint64_t U64_AT(const uint8_t *ptr)
{
    return ((uint64_t)U32_AT(ptr)) << 32 | U32_AT(ptr + 4);
}

uint16_t U16LE_AT(const uint8_t *ptr)
{
    return ptr[0] | (ptr[1] << 8);
}

uint32_t U32LE_AT(const uint8_t *ptr)
{
    return ptr[3] << 24 | ptr[2] << 16 | ptr[1] << 8 | ptr[0];
}

uint64_t U64LE_AT(const uint8_t *ptr)
{
    return ((uint64_t)U32LE_AT(ptr + 4)) << 32 | U32LE_AT(ptr);
}

uint32_t ntohl(uint32_t x)
{
    return vx_ntohl(x);
}

uint32_t htonl(uint32_t x)
{
    return vx_htonl(x);
}

uint64_t ntoh64(uint64_t x)
{
    return vx_ntohll(x);
}

uint64_t hton64(uint64_t x)
{
    return ((uint64_t)vx_htonl(x & 0xffffffff) << 32) | vx_htonl(x >> 32);
}

void MakeFourCCString(uint32_t x, char *s)
{
    s[0] = x >> 24;
    s[1] = (x >> 16) & 0xff;
    s[2] = (x >> 8) & 0xff;
    s[3] = x & 0xff;
    s[4] = '\0';
}

} // extern "C"

} // namespace sdk_utils