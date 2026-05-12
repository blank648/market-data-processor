# Health check tool
add_executable(mdp-health health_check.cpp)
target_link_libraries(mdp-health PRIVATE
    feed
    book
    infra
)
target_compile_features(mdp-health PRIVATE cxx_std_20)

# Main processor service
add_executable(mdp-processor processor_main.cpp)
target_link_libraries(mdp-processor PRIVATE
    core
    feed
    processing
    book
    infra
    PostgreSQL::PostgreSQL
)
target_compile_features(mdp-processor PRIVATE cxx_std_20)

# Set output directory for service
set_target_properties(mdp-processor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
