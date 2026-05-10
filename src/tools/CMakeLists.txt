add_executable(mdp-health health_check.cpp)

target_link_libraries(mdp-health PRIVATE
    feed
    book
    infra
)

target_compile_features(mdp-health PRIVATE cxx_std_20)
