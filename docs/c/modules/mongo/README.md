# Mongo C API

The Mongo C module exposes real BSON encoding/decoding through the existing C++ Mongo BSON codec. Encoded documents are standard BSON bytes, not the previous private `GMD1` test format.

## BSON

Use `galay_mongo_document_t` for BSON documents and `galay_mongo_array_t` for arrays. Documents support scalar values, null, nested documents, arrays, binary data, ObjectId hex strings, UTC datetime milliseconds, and Mongo timestamp values.

`galay_mongo_document_encode` returns a pointer owned by the document. The pointer remains valid until the document is modified, encoded again, or destroyed. Nested document/array getters return allocated copies; free them with the matching destroy function.

## Commands

`galay_mongo_uri_parse` parses `mongodb://` connection strings. Host, database,
and port accessors return borrowed slices owned by the URI handle; release the
handle with `galay_mongo_uri_destroy`.

CRUD helpers build command documents:

- `galay_mongo_command_find_one`
- `galay_mongo_command_insert_one`
- `galay_mongo_command_update_one`
- `galay_mongo_command_delete_one`

The helpers include `$db` in the command document and can be passed to `galay_mongo_client_command_async`.

## Async Client

The async client uses the Galay C coroutine TCP socket ABI. Call these functions only inside a `galay_coro_spawn` entry:

- `galay_mongo_client_connect_async`
- `galay_mongo_client_hello_async`
- `galay_mongo_client_command_async`
- `galay_mongo_client_close_async`

Use `galay_mongo_client_set_endpoint` before connect when not deriving endpoint
data from a URI. `galay_mongo_client_is_connected`, `galay_mongo_client_ping`,
and `galay_mongo_client_close` are synchronous state or compatibility helpers.

Errors are returned as `C_IOResult`; BSON and builder errors use `galay_status_t`. No C API path reports recoverable errors through C++ exceptions.
