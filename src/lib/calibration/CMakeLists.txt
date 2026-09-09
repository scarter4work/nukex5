add_library(nukex4_calibration STATIC
    src/qe_database.cpp
    src/channel_decomposer.cpp
    src/background_neutralization.cpp
    src/background_gradient.cpp
    src/frame_normalization.cpp
    src/ed25519_verify.cpp
    src/qe_update.cpp
    src/qe_update_state.cpp
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
)

target_include_directories(nukex4_calibration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Eigen is PUBLIC because channel_decomposer.hpp exposes Eigen::MatrixXd /
# Eigen::Vector3d in its public API. Downstream consumers of nukex4_calibration
# (Phase B Q-solve in stacker) inherit Eigen via this transitive include.
target_link_libraries(nukex4_calibration
    PUBLIC  nukex4_core nukex4_io Eigen3::Eigen
    PRIVATE nlohmann_json::nlohmann_json
)

# TweetNaCl is PRIVATE: an implementation detail of ed25519_verify.cpp, with
# no public header exposing its types.
target_include_directories(nukex4_calibration
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/tweetnacl
)

# Vendored C89 that trips the project's warning flags inside its own bignum
# code. Silence that one translation unit rather than lowering the bar
# project-wide, and keep the upstream file byte-identical.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_compile_features(nukex4_calibration PUBLIC cxx_std_17)
