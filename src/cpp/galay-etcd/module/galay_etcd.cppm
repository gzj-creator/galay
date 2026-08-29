module;

#include "module_prelude.hpp"

export module galay.etcd;

// Match the ABI of the regular galay-etcd shared library when this
// transitional interface is consumed by GCC modules.
export extern "C++" {
#include "../base/etcd_config.h"
#include "../base/etcd_error.h"
#include "../base/etcd_log.h"
#include "../base/etcd_value.h"
#include "../base/etcd_types.h"
#include "../base/network_cfg.h"
#include "../async/client.h"
#include "../cluster/etcd_cluster_client.h"
#include "../sync/etcd_client.h"
}
