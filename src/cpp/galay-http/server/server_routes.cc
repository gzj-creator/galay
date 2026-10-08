#include "server_routes.h"

namespace galay::http::server_detail {
namespace {

std::vector<std::string_view> path_segments(std::string_view path)
{
    std::vector<std::string_view> result;
    for (std::size_t offset = 0; offset < path.size();) {
        while (offset < path.size() && path[offset] == '/') ++offset;
        if (offset == path.size()) break;
        const auto end = path.find('/', offset);
        result.push_back(path.substr(offset, end == path.npos ? path.size() - offset : end - offset));
        if (end == path.npos) break;
        offset = end + 1;
    }
    return result;
}

} // namespace

api::ApiResult<void> check_registration(const RouteKey& route, std::span<const RegisteredRoute> existing)
{
    const auto segments = path_segments(route.path);
    const bool fuzzy = route.path.find_first_of(":*") != std::string::npos;
    for (const auto& registered : existing) {
        const auto& other = registered.key;
        const auto previous = path_segments(other.path);
        bool same_shape = segments.size() == previous.size();
        bool overlap = same_shape;
        bool left_specific = false, right_specific = false;
        for (std::size_t index = 0; index < std::min(segments.size(), previous.size()); ++index) {
            const auto left = segments[index], right = previous[index];
            if (left == "*" || left == "**" || right == "*" || right == "**") {
                same_shape &= left == right;
                overlap = true;
                break;
            }
            const bool left_parameter = left.starts_with(':'), right_parameter = right.starts_with(':');
            if (!left_parameter && !right_parameter && left != right) { overlap = same_shape = false; break; }
            same_shape &= (left_parameter && right_parameter) || left == right;
            left_specific |= !left_parameter && right_parameter;
            right_specific |= left_parameter && !right_parameter;
        }
        const bool duplicate = route.method == other.method &&
            (route.path == other.path || (fuzzy && segments == previous));
        const bool renamed_parameters = same_shape && segments != previous;
        const bool ambiguous = overlap && left_specific && right_specific;
        if (duplicate || renamed_parameters || ambiguous) {
            return std::unexpected(api::ApiError{api::ApiErrorCode::kRouteConflict,
                "conflicting routes: " + other.path + " and " + route.path, 409});
        }
        if (!route.operation_id.empty() && other.operation_id == route.operation_id) {
            return std::unexpected(api::ApiError{api::ApiErrorCode::kDuplicateOperation,
                "operationId is already registered: " + route.operation_id, 500});
        }
    }
    return {};
}

} // namespace galay::http::server_detail
