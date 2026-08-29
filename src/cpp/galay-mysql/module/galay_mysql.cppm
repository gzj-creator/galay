module;

#include "module_prelude.hpp"

export module galay.mysql;

// Match the ABI of the regular galay-mysql shared library when this
// transitional interface is consumed by GCC modules.
export extern "C++" {
#include "../base/mysql_config.h"
#include "../base/mysql_error.h"
#include "../base/mysql_log.h"
#include "../base/mysql_value.h"
#include "../async/client.h"
#include "../async/conn_pool.h"
#include "../sync/mysql_client.h"
}
