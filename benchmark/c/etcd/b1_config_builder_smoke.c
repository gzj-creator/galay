#include <galay/c/galay-etcd/etcd.h>

#include <stdio.h>

int main(void)
{
    for (int i = 0; i < 10000; ++i) {
        galay_etcd_config_builder_t* builder = NULL;
        galay_etcd_client_t* client = NULL;
        if (galay_etcd_config_builder_create(&builder) != GALAY_OK) {
            return 1;
        }
        if (galay_etcd_config_builder_set_endpoint(builder, "http://127.0.0.1:2379") != GALAY_OK ||
            galay_etcd_client_create(builder, &client) != GALAY_OK) {
            galay_etcd_config_builder_destroy(builder);
            return 1;
        }
        galay_etcd_client_destroy(client);
        galay_etcd_config_builder_destroy(builder);
    }
    puts("c etcd config builder smoke: 10000 iterations");
    return 0;
}
