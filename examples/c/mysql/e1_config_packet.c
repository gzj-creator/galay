#include <galay/c/galay-mysql/mysql.h>

#include <stdio.h>

int main(void)
{
    galay_mysql_config_t* config = NULL;
    galay_mysql_buffer_t* packet = NULL;
    const unsigned char* data = NULL;
    size_t data_len = 0;

    if (galay_mysql_config_create(&config) != GALAY_OK) {
        return 1;
    }
    if (galay_mysql_config_set_username(config, "demo") != GALAY_OK ||
        galay_mysql_config_set_password(config, "secret") != GALAY_OK ||
        galay_mysql_config_validate(config) != GALAY_OK) {
        galay_mysql_config_destroy(config);
        return 1;
    }

    if (galay_mysql_encode_query_packet("SELECT 1", 0, &packet) != GALAY_OK ||
        galay_mysql_buffer_data(packet, &data, &data_len) != GALAY_OK) {
        galay_mysql_buffer_destroy(packet);
        galay_mysql_config_destroy(config);
        return 1;
    }

    printf("encoded mysql query packet: %zu bytes\n", data_len);
    galay_mysql_buffer_destroy(packet);
    galay_mysql_config_destroy(config);
    return data == NULL ? 1 : 0;
}
