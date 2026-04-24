add_executable(mdp-health health_check.cpp)

target_link_libraries(mdp-health PRIVATE
    feed
    processing
    book
    infra
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

target_compile_features(mdp-health PRIVATE cxx_std_20)
