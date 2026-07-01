#include <galay/c/galay-mysql-c/mysql_c.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define REQUIRE_STATUS(expr, expected) \
    do { \
        galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

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

static size_t build_column_payload(unsigned char* out,
                                   const char* schema,
                                   const char* table,
                                   const char* name,
                                   uint8_t column_type,
                                   uint16_t flags)
{
    size_t pos = 0;
    put_lenenc_string(out, &pos, "def");
    put_lenenc_string(out, &pos, schema);
    put_lenenc_string(out, &pos, table);
    put_lenenc_string(out, &pos, table);
    put_lenenc_string(out, &pos, name);
    put_lenenc_string(out, &pos, name);
    out[pos++] = 0x0c;
    put_u16(out, &pos, 45);
    put_u32(out, &pos, 255);
    out[pos++] = column_type;
    put_u16(out, &pos, flags);
    out[pos++] = 0;
    put_u16(out, &pos, 0);
    return pos;
}

static int test_text_result_set_decode(void)
{
    unsigned char stream[512];
    unsigned char payload[128];
    size_t pos = 0;
    size_t payload_len = 0;
    size_t field_count = 0;
    size_t row_count = 0;
    size_t field_index = 99;
    galay_mysql_result_set_t* result = NULL;
    galay_mysql_field_view_t field = {0};
    galay_mysql_value_view_t value = {0};

    payload[0] = 0x02;
    pos = put_packet(stream, pos, 1, payload, 1);
    payload_len = build_column_payload(payload, "app", "users", "id", 0x03, 0x0001);
    pos = put_packet(stream, pos, 2, payload, payload_len);
    payload_len = build_column_payload(payload, "app", "users", "name", 0xfd, 0);
    pos = put_packet(stream, pos, 3, payload, payload_len);
    {
        static const unsigned char eof_payload[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
        pos = put_packet(stream, pos, 4, eof_payload, sizeof(eof_payload));
    }
    {
        static const unsigned char row_payload[] = {
            0x01, '7', 0x05, 'a', 'l', 'i', 'c', 'e'
        };
        pos = put_packet(stream, pos, 5, row_payload, sizeof(row_payload));
    }
    {
        static const unsigned char row_payload[] = {0x01, '8', 0xfb};
        pos = put_packet(stream, pos, 6, row_payload, sizeof(row_payload));
    }
    {
        static const unsigned char eof_payload[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
        pos = put_packet(stream, pos, 7, eof_payload, sizeof(eof_payload));
    }

    REQUIRE_STATUS(galay_mysql_result_set_decode(stream, pos, &result), GALAY_OK);
    REQUIRE_TRUE(result != NULL);
    REQUIRE_STATUS(galay_mysql_result_set_field_count(result, &field_count), GALAY_OK);
    REQUIRE_TRUE(field_count == 2);
    REQUIRE_STATUS(galay_mysql_result_set_row_count(result, &row_count), GALAY_OK);
    REQUIRE_TRUE(row_count == 2);
    REQUIRE_STATUS(galay_mysql_result_set_find_field(result, "name", &field_index), GALAY_OK);
    REQUIRE_TRUE(field_index == 1);
    REQUIRE_STATUS(galay_mysql_result_set_field(result, 1, &field), GALAY_OK);
    REQUIRE_TRUE(field.name != NULL);
    REQUIRE_TRUE(strcmp(field.name, "name") == 0);
    REQUIRE_TRUE(field.column_type == 0xfd);
    REQUIRE_TRUE(field.character_set == 45);
    REQUIRE_STATUS(galay_mysql_result_set_value(result, 0, 1, &value), GALAY_OK);
    REQUIRE_TRUE(value.is_null == GALAY_FALSE);
    REQUIRE_TRUE(value.data_len == 5);
    REQUIRE_TRUE(memcmp(value.data, "alice", 5) == 0);
    REQUIRE_STATUS(galay_mysql_result_set_value(result, 1, 1, &value), GALAY_OK);
    REQUIRE_TRUE(value.is_null == GALAY_TRUE);
    REQUIRE_TRUE(value.data == NULL);
    REQUIRE_TRUE(value.data_len == 0);
    REQUIRE_STATUS(galay_mysql_result_set_value(result, 2, 0, &value), GALAY_NOT_FOUND);
    REQUIRE_STATUS(galay_mysql_result_set_field(result, 9, &field), GALAY_NOT_FOUND);
    galay_mysql_result_set_destroy(result);
    return 0;
}

static int test_ok_packet_decode(void)
{
    static const unsigned char ok_packet[] = {
        0x07, 0x00, 0x00, 0x01,
        0x00, 0x02, 0x03, 0x02, 0x00, 0x01, 0x00
    };
    galay_mysql_result_set_t* result = NULL;
    uint64_t affected_rows = 0;
    uint64_t last_insert_id = 0;
    uint16_t status_flags = 0;
    uint16_t warnings = 0;
    size_t field_count = 1;
    size_t row_count = 1;

    REQUIRE_STATUS(galay_mysql_result_set_decode(ok_packet, sizeof(ok_packet), &result), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_result_set_field_count(result, &field_count), GALAY_OK);
    REQUIRE_TRUE(field_count == 0);
    REQUIRE_STATUS(galay_mysql_result_set_row_count(result, &row_count), GALAY_OK);
    REQUIRE_TRUE(row_count == 0);
    REQUIRE_STATUS(galay_mysql_result_set_affected_rows(result, &affected_rows), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_result_set_last_insert_id(result, &last_insert_id), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_result_set_status_flags(result, &status_flags), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_result_set_warnings(result, &warnings), GALAY_OK);
    REQUIRE_TRUE(affected_rows == 2);
    REQUIRE_TRUE(last_insert_id == 3);
    REQUIRE_TRUE(status_flags == 2);
    REQUIRE_TRUE(warnings == 1);
    galay_mysql_result_set_destroy(result);
    return 0;
}

static int test_decode_boundaries(void)
{
    static const unsigned char short_packet[] = {0x02, 0x00, 0x00};
    galay_mysql_result_set_t* result = NULL;

    REQUIRE_STATUS(galay_mysql_result_set_decode(NULL, 0, &result), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_result_set_decode(short_packet, sizeof(short_packet), &result),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_mysql_result_set_decode(short_packet, sizeof(short_packet), NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_result_set_field_count(NULL, NULL), GALAY_INVALID_ARGUMENT);
    galay_mysql_result_set_destroy(NULL);
    return 0;
}

int main(void)
{
    if (test_text_result_set_decode() != 0) {
        return 1;
    }
    if (test_ok_packet_decode() != 0) {
        return 1;
    }
    if (test_decode_boundaries() != 0) {
        return 1;
    }
    return 0;
}
