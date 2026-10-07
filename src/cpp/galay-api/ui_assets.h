#ifndef GALAY_API_UI_ASSETS_H
#define GALAY_API_UI_ASSETS_H

#include <span>
#include <string_view>

namespace galay::api::docs_detail {

struct UiAsset {
    std::string_view name;
    std::string_view content_type;
    std::string_view bytes;
};

std::span<const UiAsset> embedded_assets() noexcept;

} // namespace galay::api::docs_detail

#endif
