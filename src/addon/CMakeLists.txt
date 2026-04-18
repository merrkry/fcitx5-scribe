set(SCRIBE_PROTO_ROOT "${PROJECT_SOURCE_DIR}/src/proto")
set(SCRIBE_PROTO_FILE "${SCRIBE_PROTO_ROOT}/scribe/v0/scribe.proto")
set(SCRIBE_PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

file(MAKE_DIRECTORY "${SCRIBE_PROTO_GENERATED_DIR}")

add_library(scribe_proto OBJECT)

protobuf_generate(
    TARGET scribe_proto
    LANGUAGE cpp
    PROTOS "${SCRIBE_PROTO_FILE}"
    PROTOC_OUT_DIR "${SCRIBE_PROTO_GENERATED_DIR}"
    IMPORT_DIRS "${SCRIBE_PROTO_ROOT}"
)

target_include_directories(
    scribe_proto
    PUBLIC
        "${SCRIBE_PROTO_GENERATED_DIR}"
)

target_link_libraries(scribe_proto
    PUBLIC
        protobuf::libprotobuf
)

add_library(
    scribe_core
    STATIC
        addon.cpp
        config.cpp
        engine.cpp
        io.cpp
        protocol.cpp
        util.cpp
)

target_include_directories(
    scribe_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${BOOST_INCLUDE_DIR}"
        "${Protobuf_INCLUDE_DIR}"
)

target_link_libraries(
    scribe_core
    PUBLIC
        Fcitx5::Core
        scribe_proto
    PRIVATE
        OpenSSL::Crypto
        protobuf::libprotobuf
        Threads::Threads
)

target_compile_definitions(
    scribe_core
    PRIVATE
        BOOST_ASIO_NO_DEPRECATED
)

add_fcitx5_addon(
    scribe
        factory.cpp
)

target_link_libraries(
    scribe
    PRIVATE
        scribe_core
)

install(TARGETS scribe DESTINATION "${FCITX_INSTALL_ADDONDIR}")

configure_file(${PROJECT_SOURCE_DIR}/data/addon/scribe.conf.in.in scribe.conf @ONLY)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/scribe.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
    COMPONENT config
)
