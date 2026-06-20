#include <galay/c/galay-mysql/mysql.h>

#include <stdio.h>

int main(void)
{
    size_t total_bytes = 0;

    for (size_t i = 0; i < 10000; ++i) {
        galay_mysql_buffer_t* packet = NULL;
        const unsigned char* data = NULL;
        size_t data_len = 0;
        galay_mysql_packet_view_t view;

        if (galay_mysql_encode_query_packet("SELECT 1", 0, &packet) != GALAY_OK) {
            return 1;
        }
        if (galay_mysql_buffer_data(packet, &data, &data_len) != GALAY_OK ||
            galay_mysql_extract_packet(data, data_len, &view) != GALAY_OK) {
            galay_mysql_buffer_destroy(packet);
            return 1;
        }
        total_bytes += view.consumed;
        galay_mysql_buffer_destroy(packet);
    }

    printf("mysql packet codec smoke bytes=%zu\n", total_bytes);
    return total_bytes == 0 ? 1 : 0;
}
