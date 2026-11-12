/**
 * @file ui_assets.h
 * @brief 内嵌 Swagger UI 文件的只读资源视图。
 * @details 本接口位于 docs_detail 内部命名空间，供文档路由加载器使用。
 */

#ifndef GALAY_DOC_UI_ASSETS_H
#define GALAY_DOC_UI_ASSETS_H

#include <span>
#include <string_view>

namespace galay::api::docs_detail {

struct UiAsset {
    std::string_view name; ///< 资源文件名。
    std::string_view content_type; ///< HTTP Content-Type。
    std::string_view bytes; ///< 编译进程序的资源字节视图。
};

/** @brief 返回随 galay-doc 编译的完整 Swagger UI 资源集合。 */
std::span<const UiAsset> embedded_assets() noexcept;

} // namespace galay::api::docs_detail

#endif
