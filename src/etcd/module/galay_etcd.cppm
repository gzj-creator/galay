module;

#include "etcd/module/module_prelude.hpp"

export module galay.etcd;

export {
#include "etcd/base/etcd_config.h"
#include "etcd/base/etcd_error.h"
#include "etcd/base/etcd_log.h"
#include "etcd/base/etcd_value.h"
#include "etcd/base/etcd_types.h"
#include "etcd/base/network_cfg.h"
#include "etcd/async/client.h"
#include "etcd/sync/etcd_client.h"
}
