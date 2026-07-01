#include <galay/c/galay-mysql-c/mysql_c.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

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
    put_lenenc_string(out, &pos, "bench");
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

static size_t build_result(unsigned char* stream)
{
    unsigned char payload[128];
    static const unsigned char eof_payload[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
    static const unsigned char row_payload[] = {0x01, '9', 0x04, 't', 'e', 's', 't'};
    size_t pos = 0;
    size_t payload_len = 0;
    payload[0] = 0x02;
    pos = put_packet(stream, pos, 1, payload, 1);
    payload_len = build_column(payload, "id", 0x03);
    pos = put_packet(stream, pos, 2, payload, payload_len);
    payload_len = build_column(payload, "name", 0xfd);
    pos = put_packet(stream, pos, 3, payload, payload_len);
    pos = put_packet(stream, pos, 4, eof_payload, sizeof(eof_payload));
    pos = put_packet(stream, pos, 5, row_payload, sizeof(row_payload));
    pos = put_packet(stream, pos, 6, eof_payload, sizeof(eof_payload));
    return pos;
}

static long long elapsed_ns(struct timespec start, struct timespec end)
{
    return (long long)(end.tv_sec - start.tv_sec) * 1000000000LL +
        (long long)(end.tv_nsec - start.tv_nsec);
}

int main(void)
{
    unsigned char stream[512];
    const size_t stream_len = build_result(stream);
    const int iterations = 2000;
    struct timespec start = {0};
    struct timespec end = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return 1;
    }
    for (int i = 0; i < iterations; ++i) {
        galay_mysql_result_set_t* result = NULL;
        size_t rows = 0;
        if (galay_mysql_result_set_decode(stream, stream_len, &result) != GALAY_OK ||
            galay_mysql_result_set_row_count(result, &rows) != GALAY_OK ||
            rows != 1) {
            galay_mysql_result_set_destroy(result);
            return 2;
        }
        galay_mysql_result_set_destroy(result);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return 3;
    }
    if (printf("c_mysql_result_decode iterations=%d elapsed_ns=%lld\n",
               iterations,
               elapsed_ns(start, end)) < 0) {
        return 4;
    }
    return 0;
}
