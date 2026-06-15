module;

#include "galay-etcd/module/module_prelude.hpp"

export module galay.etcd;

export {
#include "galay-etcd/base/etcd_config.h"
#include "galay-etcd/base/etcd_error.h"
#include "galay-etcd/base/etcd_log.h"
#include "galay-etcd/base/etcd_value.h"
#include "galay-etcd/base/etcd_types.h"
#include "galay-etcd/base/network_cfg.h"
#include "galay-etcd/async/client.h"
#include "galay-etcd/sync/etcd_client.h"
}
