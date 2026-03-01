#include "ucp_tlv.h"

#include <limits.h>
#include <string.h>

uint16_t ucp_tlv_rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t ucp_tlv_rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t ucp_tlv_rd_u64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i] << (8 * i));
    }
    return v;
}

void ucp_tlv_wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void ucp_tlv_wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void ucp_tlv_wr_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

void ucp_tlvw_init(struct ucp_tlv_writer *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
}

bool ucp_tlvw_put_raw(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, const void *value, uint16_t len)
{
    if ((w->len + 4U + len) > w->cap) {
        return false;
    }

    w->buf[w->len++] = field_id;
    w->buf[w->len++] = type;
    ucp_tlv_wr_u16_le(&w->buf[w->len], len);
    w->len += 2;

    if (len > 0U && value != NULL) {
        memcpy(&w->buf[w->len], value, len);
        w->len += len;
    }
    return true;
}

bool ucp_tlvw_put_u8(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint8_t v)
{
    return ucp_tlvw_put_raw(w, field_id, type, &v, 1U);
}

bool ucp_tlvw_put_u16(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint16_t v)
{
    uint8_t b[2];
    ucp_tlv_wr_u16_le(b, v);
    return ucp_tlvw_put_raw(w, field_id, type, b, sizeof(b));
}

bool ucp_tlvw_put_u32(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint32_t v)
{
    uint8_t b[4];
    ucp_tlv_wr_u32_le(b, v);
    return ucp_tlvw_put_raw(w, field_id, type, b, sizeof(b));
}

bool ucp_tlvw_put_u64(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint64_t v)
{
    uint8_t b[8];
    ucp_tlv_wr_u64_le(b, v);
    return ucp_tlvw_put_raw(w, field_id, type, b, sizeof(b));
}

bool ucp_tlvw_put_i32(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, int32_t v)
{
    return ucp_tlvw_put_raw(w, field_id, type, &v, sizeof(v));
}

bool ucp_tlvw_put_bytes(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, const uint8_t *data, uint16_t len)
{
    return ucp_tlvw_put_raw(w, field_id, type, data, len);
}

bool ucp_tlvw_put_str(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, const char *str)
{
    size_t n = str ? strlen(str) : 0U;
    if (n > UINT16_MAX) {
        return false;
    }
    return ucp_tlvw_put_raw(w, field_id, type, str, (uint16_t)n);
}

bool ucp_tlv_next(const uint8_t *buf, size_t len, size_t *offset, struct ucp_tlv_elem *elem)
{
    if (!buf || !offset || !elem) {
        return false;
    }
    if (*offset >= len || (len - *offset) < 4U) {
        return false;
    }

    elem->field_id = buf[(*offset)++];
    elem->type = buf[(*offset)++];
    elem->len = ucp_tlv_rd_u16_le(&buf[*offset]);
    *offset += 2U;

    if ((len - *offset) < elem->len) {
        return false;
    }

    elem->value = &buf[*offset];
    *offset += elem->len;
    return true;
}

bool ucp_tlv_read_u8(const struct ucp_tlv_elem *e, uint8_t *out)
{
    if (!e || !out || e->len != 1U) {
        return false;
    }
    *out = e->value[0];
    return true;
}

bool ucp_tlv_read_u16(const struct ucp_tlv_elem *e, uint16_t *out)
{
    if (!e || !out || e->len != 2U) {
        return false;
    }
    *out = ucp_tlv_rd_u16_le(e->value);
    return true;
}

bool ucp_tlv_read_u32(const struct ucp_tlv_elem *e, uint32_t *out)
{
    if (!e || !out || e->len != 4U) {
        return false;
    }
    *out = ucp_tlv_rd_u32_le(e->value);
    return true;
}

bool ucp_tlv_read_u64(const struct ucp_tlv_elem *e, uint64_t *out)
{
    if (!e || !out || e->len != 8U) {
        return false;
    }
    *out = ucp_tlv_rd_u64_le(e->value);
    return true;
}

bool ucp_tlv_read_i32(const struct ucp_tlv_elem *e, int32_t *out)
{
    if (!e || !out || e->len != 4U) {
        return false;
    }
    *out = (int32_t)ucp_tlv_rd_u32_le(e->value);
    return true;
}
