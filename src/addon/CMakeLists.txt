add_fcitx5_addon(scribe)

target_sources(scribe PRIVATE addon.cpp config.cpp factory.cpp)

target_link_libraries(scribe PRIVATE Fcitx5::Core)

install(TARGETS scribe DESTINATION "${FCITX_INSTALL_ADDONDIR}")

configure_file(scribe.conf.in.in scribe.conf @ONLY)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/scribe.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
    COMPONENT config
)
