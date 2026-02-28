#include "ucp_cobs.h"

#include <errno.h>

int ucp_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    size_t ri = 0;
    size_t wi = 1;
    size_t ci = 0;
    uint8_t code = 1;

    if (!out || out_cap == 0U) {
        return -EINVAL;
    }

    while (ri < in_len) {
        if (wi >= out_cap) {
            return -ENOSPC;
        }
        if (in[ri] == 0U) {
            out[ci] = code;
            code = 1;
            ci = wi++;
            ri++;
            continue;
        }

        out[wi++] = in[ri++];
        code++;
        if (code == 0xFFU) {
            out[ci] = code;
            code = 1;
            ci = wi++;
            if (wi > out_cap) {
                return -ENOSPC;
            }
        }
    }

    if (ci >= out_cap) {
        return -ENOSPC;
    }
    out[ci] = code;
    return (int)wi;
}

int ucp_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    size_t ri = 0;
    size_t wi = 0;

    if (!in || !out) {
        return -EINVAL;
    }

    while (ri < in_len) {
        uint8_t code = in[ri++];
        if (code == 0U) {
            return -EINVAL;
        }

        for (uint8_t i = 1; i < code; i++) {
            if (ri >= in_len || wi >= out_cap) {
                return -EINVAL;
            }
            out[wi++] = in[ri++];
        }

        if (code != 0xFFU && ri < in_len) {
            if (wi >= out_cap) {
                return -ENOSPC;
            }
            out[wi++] = 0U;
        }
    }

    return (int)wi;
}
