find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Widgets Test)
find_package(Threads REQUIRED)

add_executable(tst_w06_tls_allow_insecure
    tst_tls_allow_insecure.cpp
    "${PROJECT_SOURCE_DIR}/fmt/AbstractBean.cpp"
    "${PROJECT_SOURCE_DIR}/fmt/Bean2CoreObj_box.cpp"
    "${PROJECT_SOURCE_DIR}/fmt/Bean2External.cpp"
    "${PROJECT_SOURCE_DIR}/fmt/Bean2Link.cpp"
    "${PROJECT_SOURCE_DIR}/main/NekoGui.cpp"
    "${PROJECT_SOURCE_DIR}/main/NekoGui_Utils.cpp"
    "${PROJECT_SOURCE_DIR}/3rdparty/base64.cpp"
)

set_target_properties(tst_w06_tls_allow_insecure PROPERTIES
    AUTOMOC ON
    AUTOUIC OFF
    AUTORCC OFF
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(tst_w06_tls_allow_insecure PRIVATE
    "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(tst_w06_tls_allow_insecure PRIVATE
    nkr_link_parser
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Test
    Threads::Threads
)

if(WIN32)
    target_sources(tst_w06_tls_allow_insecure PRIVATE
        "${PROJECT_SOURCE_DIR}/sys/windows/guihelper.cpp"
    )
    target_link_libraries(tst_w06_tls_allow_insecure PRIVATE
        shell32
        user32
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(tst_w06_tls_allow_insecure PRIVATE
        "${PROJECT_SOURCE_DIR}/sys/linux/LinuxCap.cpp"
    )
endif()

add_test(
    NAME w06_tls_allow_insecure
    COMMAND tst_w06_tls_allow_insecure
)

set_tests_properties(w06_tls_allow_insecure PROPERTIES
    TIMEOUT 30
    LABELS "W06"
)
