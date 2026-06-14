module;

#include "mongo/module/module_prelude.hpp"
#include "mongo/base/mongo_config.h"
#include "mongo/base/mongo_error.h"
#include "mongo/base/mongo_log.h"
#include "mongo/base/mongo_value.h"
#include "mongo/protoc/builder.h"
#include "mongo/async/client.h"
#include "mongo/sync/mongo_client.h"

export module galay.mongo;

export using ::galay::mongo::MongoConfig;
export using ::galay::mongo::MongoErrorType;
export using ::galay::mongo::MongoError;
export using ::galay::mongo::MongoValueType;
export using ::galay::mongo::MongoValue;
export using ::galay::mongo::MongoDocument;
export using ::galay::mongo::MongoArray;
export using ::galay::mongo::MongoReply;
export using ::galay::mongo::protocol::MongoCommandBuilder;
export using ::galay::mongo::AsyncMongoConfig;
export using ::galay::mongo::MongoConnectAwaitable;
export using ::galay::mongo::MongoCommandAwaitable;
export using ::galay::mongo::MongoPipelineResponse;
export using ::galay::mongo::MongoPipelineAwaitable;
export using ::galay::mongo::AsyncMongoClient;
export using ::galay::mongo::MongoClient;
export using ::galay::mongo::MongoResult;
export using ::galay::mongo::MongoVoidResult;
