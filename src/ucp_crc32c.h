#ifndef UCP_CRC32C_H
#define UCP_CRC32C_H

#include <stddef.h>
#include <stdint.h>

uint32_t ucp_crc32c(const uint8_t *data, size_t len);

#endif /* UCP_CRC32C_H */
