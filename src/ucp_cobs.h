#ifndef UCP_COBS_H
#define UCP_COBS_H

#include <stddef.h>
#include <stdint.h>

int ucp_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);
int ucp_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

#endif /* UCP_COBS_H */
