/**
 * @file etcd_log.cc
 * @brief galay-etcd 独立日志槽实现
 */

#include "galay-etcd/base/etcd_log.h"

#include <utility>

namespace
{
using EtcdLoggerSlot = ::galay::kernel::LoggerSlot<::galay::etcd::detail::EtcdLogTag>;
} // namespace

namespace galay::etcd::log
{

void set(::galay::kernel::BaseLogger::uptr logger)
{
    EtcdLoggerSlot::set(std::move(logger));
}

::galay::kernel::BaseLogger* get() noexcept
{
    return EtcdLoggerSlot::get();
}

} // namespace galay::etcd::log
