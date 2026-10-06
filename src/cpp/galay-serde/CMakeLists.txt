galay_ensure_serde()

add_library(galay-serde INTERFACE)
add_library(galay::serde ALIAS galay-serde)
set_target_properties(galay-serde PROPERTIES EXPORT_NAME serde)
target_link_libraries(galay-serde INTERFACE serde::serde)

set(GALAY_SERDE_CPP23_MODULES_EFFECTIVE OFF)
if(TARGET serde-cpp23-modules)
    set(GALAY_SERDE_CPP23_MODULES_EFFECTIVE ON)
    # 模块提供方直接挂到规范目标上：消费者统一链接 galay::serde。
    target_link_libraries(galay-serde INTERFACE "$<BUILD_INTERFACE:serde-cpp23-modules>")
endif()
set(GALAY_SERDE_CPP23_MODULES_EFFECTIVE "${GALAY_SERDE_CPP23_MODULES_EFFECTIVE}" PARENT_SCOPE)
