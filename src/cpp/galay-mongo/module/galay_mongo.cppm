module;

#include "module_prelude.hpp"

export module galay.mongo;

export extern "C++" {
#include "../base/mongo_config.h"
#include "../base/mongo_error.h"
#include "../base/mongo_log.h"
#include "../base/mongo_uri.h"
#include "../base/mongo_value.h"
#include "../protoc/builder.h"
#include "../async/client.h"
#include "../sync/mongo_client.h"
}
