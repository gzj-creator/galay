#include <galay/c/galay-ssl-c/ssl_c.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef GALAY_SSL_TEST_CERT_DIR
#define GALAY_SSL_TEST_CERT_DIR ""
#endif

static void make_path(char* out, size_t out_size, const char* name)
{
    const int written = snprintf(out, out_size, "%s/%s", GALAY_SSL_TEST_CERT_DIR, name);
    assert(written > 0);
    assert((size_t)written < out_size);
}

static void test_create_destroy(void)
{
    galay_ssl_context_t* context = NULL;

    assert(galay_ssl_context_create(GALAY_SSL_METHOD_TLS_SERVER, &context) == GALAY_OK);
    assert(context != NULL);

    galay_ssl_context_destroy(context);
    galay_ssl_context_destroy(NULL);
}

static void test_rejects_null_context(void)
{
    assert(galay_ssl_context_load_certificate(NULL, "unused.crt") == GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_load_private_key(NULL, "unused.key") == GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_load_ca(NULL, "unused-ca.crt") == GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_verify_mode(NULL, GALAY_SSL_VERIFY_PEER) == GALAY_INVALID_ARGUMENT);
}

static void test_configuration_boundaries(void)
{
    galay_ssl_context_t* context = NULL;
    const char* valid_protocols[] = {"h2", "http/1.1"};
    const char* empty_protocols[] = {""};
    const char* null_protocols[] = {NULL};
    char oversized_protocol[257];
    const char* oversized_protocols[] = {oversized_protocol};

    memset(oversized_protocol, 'x', sizeof(oversized_protocol));
    oversized_protocol[sizeof(oversized_protocol) - 1] = '\0';

    assert(galay_ssl_context_create((galay_ssl_method_t)9999, &context) ==
           GALAY_INVALID_ARGUMENT);
    assert(context == NULL);
    assert(galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, NULL) ==
           GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &context) == GALAY_OK);

    assert(galay_ssl_context_set_alpn_protocols(context, NULL, 1) ==
           GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_alpn_protocols(context, valid_protocols, 0) ==
           GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_alpn_protocols(context, null_protocols, 1) ==
           GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_alpn_protocols(context, empty_protocols, 1) ==
           GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_alpn_protocols(context, oversized_protocols, 1) ==
           GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_alpn_protocols(context, valid_protocols, 2) == GALAY_OK);
    assert(galay_ssl_context_set_alpn_select_protocols(context, valid_protocols, 2) == GALAY_OK);
    assert(galay_ssl_context_set_session_cache_mode(
               context, (galay_ssl_session_cache_mode_t)9999) == GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_session_timeout(context, -1) == GALAY_INVALID_ARGUMENT);
    assert(galay_ssl_context_set_session_timeout(context, 0) == GALAY_OK);
    assert(galay_ssl_context_disable_session_cache(context) == GALAY_OK);
    assert(galay_ssl_context_disable_session_tickets(context) == GALAY_OK);

    galay_ssl_context_destroy(context);
}

static void test_rejects_missing_files(void)
{
    galay_ssl_context_t* context = NULL;
    assert(galay_ssl_context_create(GALAY_SSL_METHOD_TLS_SERVER, &context) == GALAY_OK);

    assert(galay_ssl_context_load_certificate(context, "missing-server.crt") == GALAY_NOT_FOUND);
    assert(galay_ssl_context_load_private_key(context, "missing-server.key") == GALAY_NOT_FOUND);
    assert(galay_ssl_context_load_ca(context, "missing-ca.crt") == GALAY_NOT_FOUND);

    galay_ssl_context_destroy(context);
}

static void test_rejects_invalid_verify_mode(void)
{
    galay_ssl_context_t* context = NULL;
    assert(galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &context) == GALAY_OK);

    assert(galay_ssl_context_set_verify_mode(context, (galay_ssl_verify_mode_t)9999) ==
           GALAY_INVALID_ARGUMENT);

    galay_ssl_context_destroy(context);
}

static void test_repeated_loads_succeed(void)
{
    char cert_path[512];
    char key_path[512];
    char ca_path[512];
    galay_ssl_context_t* context = NULL;

    make_path(cert_path, sizeof(cert_path), "server.crt");
    make_path(key_path, sizeof(key_path), "server.key");
    make_path(ca_path, sizeof(ca_path), "ca.crt");

    assert(galay_ssl_context_create(GALAY_SSL_METHOD_TLS_SERVER, &context) == GALAY_OK);

    assert(galay_ssl_context_load_certificate(context, cert_path) == GALAY_OK);
    assert(galay_ssl_context_load_certificate(context, cert_path) == GALAY_OK);
    assert(galay_ssl_context_load_private_key(context, key_path) == GALAY_OK);
    assert(galay_ssl_context_load_private_key(context, key_path) == GALAY_OK);
    assert(galay_ssl_context_load_ca(context, ca_path) == GALAY_OK);
    assert(galay_ssl_context_load_ca(context, ca_path) == GALAY_OK);
    assert(galay_ssl_context_set_verify_mode(context, GALAY_SSL_VERIFY_PEER) == GALAY_OK);

    galay_ssl_context_destroy(context);
}

int main(void)
{
    test_create_destroy();
    test_rejects_null_context();
    test_configuration_boundaries();
    test_rejects_missing_files();
    test_rejects_invalid_verify_mode();
    test_repeated_loads_succeed();
    return 0;
}
