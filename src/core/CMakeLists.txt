# INTERFACE library — header-only, nu compilează .cpp propriu
add_library(core INTERFACE)

target_include_directories(core INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(core INTERFACE cxx_std_20)
