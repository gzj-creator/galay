#ifndef GALAY_API_BINDING_CONTRACT_H
#define GALAY_API_BINDING_CONTRACT_H

#include "operation.h"
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <functional>

namespace galay::api {

struct NoInput {};
struct NoContent {};

template<class Input>
struct BindingPlan {
    std::vector<Parameter> parameters;
    std::optional<Schema> body_schema;
    bool body_required = false;
    std::function<ApiResult<Input>(http::HttpRequest&)> decode;
};

template<class Input>
class InputBinding {
public:
    template<auto Member>
    InputBinding& path(std::string name);
    template<auto Member>
    InputBinding& query(std::string name);
    ApiResult<BindingPlan<Input>> prepare(http::HttpMethod method,
                                         std::string_view path) const;

private:
    struct MemberBinding {
        ParameterSource source;
        std::string name;
        std::string field_name;
        std::function<ApiResult<Parameter>()> describe;
        std::function<ApiResult<void>(Input&, std::optional<std::string_view>)> decode;
    };
    template<auto Member>
    InputBinding& add(ParameterSource source, std::string name);
    std::vector<MemberBinding> members_;
    std::optional<ApiError> error_;
};

} // namespace galay::api

#endif
