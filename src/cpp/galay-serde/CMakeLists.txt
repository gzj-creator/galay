galay_ensure_serde()

add_library(galay-serde INTERFACE)
add_library(galay::serde ALIAS galay-serde)
set_target_properties(galay-serde PROPERTIES EXPORT_NAME serde)
target_link_libraries(galay-serde INTERFACE serde::serde)

if(TARGET serde-cpp23-modules)
    # A direct alias preserves CMake's imported module-provider identity.
    add_library(galay::serde-modules ALIAS serde-cpp23-modules)
endif()
