#include "ByteUtils.h"
#include <cmath>
#include <stdint.h>

namespace sdk_utils {

extern "C" {
typedef char char_t;
/**
 * @brief Union used to detect host byte order.
 * @note
 * Little-endian: low address stores low byte, high address stores high byte.
 * Big-endian: high address stores low byte, low address stores high byte.
 * Network byte order is big-endian.
 */
static union {
    char_t xct_order[4];
    uint32_t xut_order;
} xhost_order = { { 'L', '?', '?', 'B' } };

#define X_IS_LITTLE_ENDIAN ('L' == (char_t)xhost_order.xut_order)
#define X_IS_BIG_ENDIAN    ('B' == (char_t)xhost_order.xut_order)

/**********************************************************/
/**
 * @brief Byte order conversion: 16-bit integer from network to host order.
 */
uint16_t vx_ntohs(uint16_t xut_short)
{
    if (X_IS_LITTLE_ENDIAN)
        return ((xut_short << 8) | (xut_short >> 8));
    return xut_short;
}

/**********************************************************/
/**
 * @brief Byte order conversion: 16-bit integer from host to network order.
 */
uint16_t vx_htons(uint16_t xut_short)
{
    if (X_IS_LITTLE_ENDIAN)
        return ((xut_short << 8) | (xut_short >> 8));
    return xut_short;
}

/**********************************************************/
/**
 * @brief Byte order conversion: 32-bit integer from network to host order.
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
 * @brief Byte order conversion: 32-bit integer from host to network order.
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
 * @brief Byte order conversion: 64-bit integer from network to host order.
 */
uint64_t vx_ntohll(uint64_t xult_llong)
{
    if (X_IS_LITTLE_ENDIAN)
        return (
            ((xult_llong) << 56) | ((xult_llong & 0x000000000000FF00) << 40)
            | ((xult_llong & 0x0000000000FF0000) << 24) | ((xult_llong & 0x00000000FF000000) << 8)
            | ((xult_llong & 0x000000FF00000000) >> 8) | ((xult_llong & 0x0000FF0000000000) >> 24)
            | ((xult_llong & 0x00FF000000000000) >> 40) | ((xult_llong) >> 56));
    return xult_llong;
}

bool is_little_endian()
{
    return X_IS_LITTLE_ENDIAN;
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

long double ieee754_80bits_to_long_double(const uint8_t *ptr)
{
    // Parse 80-bit floating-point byte array
    // IEEE 754 80-bit floating-point format
    // IEEE 754 layout: 1 sign bit, 15 exponent bits, 64 significand bits

    uint8_t sign         = (ptr[0] & 0x80) >> 7;
    uint16_t exponent    = ((ptr[0] & 0x7F) << 8) | ptr[1];
    uint64_t significand = 0;

    for (int i = 2; i < 10; i++) {
        significand = (significand << 8) | ptr[i];
    }
    long double floatValue = 1.0L;

    // Compute exponent
    int bias           = 16383; // Exponent bias for IEEE 754 80-bit format
    int actualExponent = static_cast<int>(exponent) - bias;

    // Compute integer part
    auto setNumBit = [](int &num, int bit) { num |= (1 << bit); };

    auto clearNumBit = [](int &num, int bit) { num &= ~(1 << bit); };

    int inter  = 0;
    int endPos = 63 - actualExponent;
    for (int i = 63; i >= endPos; i--) {
        int bit = (significand & (1ULL << i)) ? 1 : 0;
        if (bit) {
            setNumBit(inter, i - endPos);
        } else {
            clearNumBit(inter, i - endPos);
        }
    }

    // Compute fractional part
    long double decimal = 0.0L;
    bool valid          = false;
    for (int i = 0; i < endPos; i++) {
        int bit = (significand & (1ULL << i)) ? 1 : 0;
        // Skip trailing invalid zeros
        if (!valid && bit) {
            valid = true;
        }
        if (valid && bit) {
            decimal += 1;
            decimal /= 2;
        } else if (valid && !bit) {
            decimal /= 2;
        }
    }
    floatValue  = inter + decimal;
    // Apply sign
    floatValue *= (sign == 1) ? -1.0L : 1.0L;
    return floatValue;
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
