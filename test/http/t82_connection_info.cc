#include "http/plugin/common/conn_info_storage.hpp"

#include <concepts>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>

using galay::http::plugin::ConnInfo;
using galay::http::plugin::ConnInfoStorage;
using galay::kernel::Host;
using galay::kernel::IPType;

namespace {

template <typename Storage>
using MutableGetRefResult = decltype(std::declval<Storage&>().getConnInfoRef(
    std::declval<const Host&>()));

template <typename Storage>
using ConstGetResult = decltype(std::declval<const Storage&>().getConnInfo(
    std::declval<const Host&>()));

template <typename Storage>
using GetOrCreateResult = decltype(std::declval<Storage&>().getOrCreateConnInfo(
    std::declval<const Host&>()));

template <typename Storage>
concept HasConstGetConnInfoRef = requires(const Storage& storage, const Host& host) {
    storage.getConnInfoRef(host);
};

template <typename Storage>
concept HasFindConnInfo = requires(Storage& storage, const Host& host) {
    storage.findConnInfo(host);
};

template <typename Storage>
concept HasGetOrCreateConnInfo = requires(Storage& storage, const Host& host) {
    storage.getOrCreateConnInfo(host);
};

static_assert(std::same_as<MutableGetRefResult<ConnInfoStorage>,
                           std::optional<std::reference_wrapper<ConnInfo>>>);
static_assert(std::same_as<ConstGetResult<ConnInfoStorage>,
                           std::optional<ConnInfo>>);
static_assert(std::same_as<GetOrCreateResult<ConnInfoStorage>, ConnInfo&>);
static_assert(!HasConstGetConnInfoRef<ConnInfoStorage>);
static_assert(!HasFindConnInfo<ConnInfoStorage>);
static_assert(HasGetOrCreateConnInfo<ConnInfoStorage>);

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T82] " << message << "\n";
    std::abort();
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

} // namespace

int main()
{
    ConnInfoStorage storage;

    Host ipv4_loopback(IPType::IPV4, "127.0.0.1", 8080);
    Host same_ipv4_loopback(IPType::IPV4, "127.0.0.1", 8080);
    Host ipv4_other_port(IPType::IPV4, "127.0.0.1", 8081);
    Host ipv6_loopback(IPType::IPV6, "::1", 8080);

    auto missing_ref = storage.getConnInfoRef(ipv4_loopback);
    expect(!missing_ref.has_value(), "getConnInfoRef should not create a missing host entry");
    expect(storage.empty(), "missing getConnInfoRef should leave storage unchanged");

    ConnInfo& created_ref = storage.getOrCreateConnInfo(ipv4_loopback);
    expect(storage.size() == 1, "getOrCreateConnInfo should create a missing host entry");

    ConnInfo& repeated_create_ref = storage.getOrCreateConnInfo(same_ipv4_loopback);
    expect(&created_ref == &repeated_create_ref, "getOrCreateConnInfo should return the existing host entry");
    expect(storage.size() == 1, "getOrCreateConnInfo should not duplicate equivalent hosts");

    auto second_ref = storage.getConnInfoRef(same_ipv4_loopback);
    expect(second_ref.has_value(), "getConnInfoRef should find an existing host entry");
    auto third_ref = storage.getConnInfoRef(ipv4_loopback);
    expect(third_ref.has_value(), "getConnInfoRef should find the original host entry");
    expect(&third_ref->get() == &second_ref->get(), "equivalent hosts should resolve to the same ConnInfo");
    expect(storage.size() == 1, "getConnInfoRef should not duplicate equivalent hosts");

    expect(!storage.addConnInfo(ipv4_loopback, ConnInfo{}), "addConnInfo should reject an existing host");
    expect(storage.addConnInfo(ipv4_other_port, ConnInfo{}), "addConnInfo should insert a new host");
    expect(storage.addConnInfo(ipv6_loopback, ConnInfo{}), "addConnInfo should insert an IPv6 host");
    expect(storage.size() == 3, "distinct host keys should be tracked independently");

    expect(storage.containsConnInfo(ipv4_other_port), "containsConnInfo should find an existing host");
    expect(storage.getConnInfoRef(ipv4_other_port).has_value(),
           "getConnInfoRef should return an existing host reference");
    expect(storage.getConnInfo(ipv4_other_port).has_value(), "getConnInfo should return an existing host value");

    const ConnInfoStorage& const_storage = storage;
    expect(const_storage.getConnInfo(ipv6_loopback).has_value(),
           "const getConnInfo should return an existing host value");

    Host absent(IPType::IPV4, "127.0.0.1", 9090);
    expect(!storage.updateConnInfo(absent, ConnInfo{}), "updateConnInfo should reject a missing host");
    expect(storage.updateConnInfo(ipv4_other_port, ConnInfo{}), "updateConnInfo should modify an existing host");
    expect(storage.size() == 3, "updateConnInfo should not create or remove entries");

    expect(storage.deleteConnInfo(ipv4_other_port), "deleteConnInfo should remove an existing host");
    expect(!storage.deleteConnInfo(ipv4_other_port), "deleteConnInfo should reject a missing host");
    expect(!storage.containsConnInfo(ipv4_other_port), "deleted host should not be found");
    expect(!storage.getConnInfo(ipv4_other_port).has_value(), "getConnInfo should reject a deleted host");
    expect(storage.size() == 2, "deleteConnInfo should remove exactly one entry");

    storage.clearConnInfo();
    expect(storage.empty(), "clearConnInfo should remove all entries");

    return 0;
}
