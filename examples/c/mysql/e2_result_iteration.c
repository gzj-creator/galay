#include <galay/c/galay-mysql-c/mysql.h>

#include <stdio.h>
#include <string.h>

static void put_u16(unsigned char* out, size_t* pos, uint16_t value)
{
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8u) & 0xffu);
}

static void put_u24(unsigned char* out, size_t* pos, uint32_t value)
{
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8u) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 16u) & 0xffu);
}

static void put_u32(unsigned char* out, size_t* pos, uint32_t value)
{
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8u) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 16u) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 24u) & 0xffu);
}

static void put_lenenc_string(unsigned char* out, size_t* pos, const char* value)
{
    const size_t len = strlen(value);
    out[(*pos)++] = (unsigned char)len;
    memcpy(out + *pos, value, len);
    *pos += len;
}

static size_t put_packet(unsigned char* out,
                         size_t pos,
                         uint8_t sequence_id,
                         const unsigned char* payload,
                         size_t payload_len)
{
    size_t cursor = pos;
    put_u24(out, &cursor, (uint32_t)payload_len);
    out[cursor++] = sequence_id;
    memcpy(out + cursor, payload, payload_len);
    return cursor + payload_len;
}

static size_t build_column(unsigned char* out, const char* name, uint8_t type)
{
    size_t pos = 0;
    put_lenenc_string(out, &pos, "def");
    put_lenenc_string(out, &pos, "app");
    put_lenenc_string(out, &pos, "items");
    put_lenenc_string(out, &pos, "items");
    put_lenenc_string(out, &pos, name);
    put_lenenc_string(out, &pos, name);
    out[pos++] = 0x0c;
    put_u16(out, &pos, 45);
    put_u32(out, &pos, 255);
    out[pos++] = type;
    put_u16(out, &pos, 0);
    out[pos++] = 0;
    put_u16(out, &pos, 0);
    return pos;
}

int main(void)
{
    unsigned char stream[512];
    unsigned char payload[128];
    static const unsigned char eof_payload[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
    static const unsigned char row_payload[] = {0x01, '1', 0x06, 'w', 'i', 'd', 'g', 'e', 't'};
    size_t pos = 0;
    size_t payload_len = 0;
    galay_mysql_result_set_t* result = NULL;
    size_t field_count = 0;
    size_t row_count = 0;

    payload[0] = 0x02;
    pos = put_packet(stream, pos, 1, payload, 1);
    payload_len = build_column(payload, "id", 0x03);
    pos = put_packet(stream, pos, 2, payload, payload_len);
    payload_len = build_column(payload, "name", 0xfd);
    pos = put_packet(stream, pos, 3, payload, payload_len);
    pos = put_packet(stream, pos, 4, eof_payload, sizeof(eof_payload));
    pos = put_packet(stream, pos, 5, row_payload, sizeof(row_payload));
    pos = put_packet(stream, pos, 6, eof_payload, sizeof(eof_payload));

    if (galay_mysql_result_set_decode(stream, pos, &result) != GALAY_OK ||
        galay_mysql_result_set_field_count(result, &field_count) != GALAY_OK ||
        galay_mysql_result_set_row_count(result, &row_count) != GALAY_OK) {
        galay_mysql_result_set_destroy(result);
        return 1;
    }
    for (size_t row = 0; row < row_count; ++row) {
        for (size_t column = 0; column < field_count; ++column) {
            galay_mysql_field_view_t field = {0};
            galay_mysql_value_view_t value = {0};
            if (galay_mysql_result_set_field(result, column, &field) != GALAY_OK ||
                galay_mysql_result_set_value(result, row, column, &value) != GALAY_OK) {
                galay_mysql_result_set_destroy(result);
                return 2;
            }
            if (value.is_null == GALAY_TRUE) {
                if (printf("%s=NULL\n", field.name) < 0) {
                    galay_mysql_result_set_destroy(result);
                    return 3;
                }
            } else if (printf("%s=%.*s\n",
                              field.name,
                              (int)value.data_len,
                              (const char*)value.data) < 0) {
                galay_mysql_result_set_destroy(result);
                return 4;
            }
        }
    }
    galay_mysql_result_set_destroy(result);
    return 0;
}
