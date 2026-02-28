#ifndef UCP_TLV_H
#define UCP_TLV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ucp_tlv_writer {
    uint8_t *buf;
    size_t cap;
    size_t len;
};

struct ucp_tlv_elem {
    uint8_t field_id;
    uint8_t type;
    uint16_t len;
    const uint8_t *value;
};

void ucp_tlvw_init(struct ucp_tlv_writer *w, uint8_t *buf, size_t cap);
bool ucp_tlvw_put_raw(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, const void *value, uint16_t len);
bool ucp_tlvw_put_u8(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint8_t v);
bool ucp_tlvw_put_u16(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint16_t v);
bool ucp_tlvw_put_u32(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint32_t v);
bool ucp_tlvw_put_u64(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, uint64_t v);
bool ucp_tlvw_put_i32(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, int32_t v);
bool ucp_tlvw_put_bytes(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, const uint8_t *data, uint16_t len);
bool ucp_tlvw_put_str(struct ucp_tlv_writer *w, uint8_t field_id, uint8_t type, const char *str);

bool ucp_tlv_next(const uint8_t *buf, size_t len, size_t *offset, struct ucp_tlv_elem *elem);
bool ucp_tlv_read_u8(const struct ucp_tlv_elem *e, uint8_t *out);
bool ucp_tlv_read_u16(const struct ucp_tlv_elem *e, uint16_t *out);
bool ucp_tlv_read_u32(const struct ucp_tlv_elem *e, uint32_t *out);
bool ucp_tlv_read_u64(const struct ucp_tlv_elem *e, uint64_t *out);
bool ucp_tlv_read_i32(const struct ucp_tlv_elem *e, int32_t *out);

void ucp_tlv_wr_u16_le(uint8_t *p, uint16_t v);
void ucp_tlv_wr_u32_le(uint8_t *p, uint32_t v);
void ucp_tlv_wr_u64_le(uint8_t *p, uint64_t v);
uint16_t ucp_tlv_rd_u16_le(const uint8_t *p);
uint32_t ucp_tlv_rd_u32_le(const uint8_t *p);
uint64_t ucp_tlv_rd_u64_le(const uint8_t *p);

#endif /* UCP_TLV_H */
