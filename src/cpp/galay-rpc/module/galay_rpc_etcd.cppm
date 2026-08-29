module;

#include "module_prelude.hpp"

export module galay.rpc.etcd;

// Match the ABI of the regular galay-rpc shared library when this
// transitional interface is consumed by GCC modules.
export extern "C++" {
#include "../discovery/etcd_service_registry.h"
}
