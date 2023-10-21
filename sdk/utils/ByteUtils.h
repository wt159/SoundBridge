#pragma once

#include <stdint.h>

namespace sdk_utils {

extern "C" {

uint16_t U16_AT(const uint8_t *ptr);
uint32_t U32_AT(const uint8_t *ptr);
uint64_t U64_AT(const uint8_t *ptr);
uint16_t U16LE_AT(const uint8_t *ptr);
uint32_t U32LE_AT(const uint8_t *ptr);
uint64_t U64LE_AT(const uint8_t *ptr);
uint32_t ntohl(uint32_t x);
uint32_t htonl(uint32_t x);
uint64_t ntoh64(uint64_t x);
uint64_t hton64(uint64_t x);

void MakeFourCCString(uint32_t x, char *s);

} // extern "C"

} // namespace sdk_utils