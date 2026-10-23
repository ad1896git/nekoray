cmake_minimum_required(VERSION 3.5)

project(nekobox VERSION 0.1 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NKR_BUILD_TESTS "Build Nekoray unit tests" OFF)

# WINDOWS PDB FILE
if (WIN32)
    if (MSVC)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    endif ()
endif ()

# Find Qt
if (NOT QT_VERSION_MAJOR)
    set(QT_VERSION_MAJOR 5)
endif ()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Svg LinguistTools)

if (NKR_BUILD_TESTS)
    enable_testing()
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
endif ()

if (NKR_CROSS)
    set_property(TARGET Qt5::moc PROPERTY IMPORTED_LOCATION /usr/bin/moc)
    set_property(TARGET Qt5::uic PROPERTY IMPORTED_LOCATION /usr/bin/uic)
    set_property(TARGET Qt5::rcc PROPERTY IMPORTED_LOCATION /usr/bin/rcc)
    set_property(TARGET Qt5::lrelease PROPERTY IMPORTED_LOCATION /usr/bin/lrelease)
    set_property(TARGET Qt5::lupdate PROPERTY IMPORTED_LOCATION /usr/bin/lupdate)
endif ()

#### Platform Variables ####
if (WIN32)
    include("cmake/windows/windows.cmake")
else ()
    include("cmake/linux/linux.cmake")
endif ()

#### default prefix path ####

if (NOT NKR_LIBS)
    if (NKR_PACKAGE)
        list(APPEND NKR_LIBS ${CMAKE_SOURCE_DIR}/libs/deps/package)
    else ()
        list(APPEND NKR_LIBS ${CMAKE_SOURCE_DIR}/libs/deps/built)
    endif ()
endif ()

if (NOT NKR_DISABLE_LIBS)
    list(APPEND CMAKE_PREFIX_PATH ${NKR_LIBS})
endif ()

message("[CMAKE_PREFIX_PATH] ${CMAKE_PREFIX_PATH}")

# for some cross toolchain
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
message("[CMAKE_FIND_ROOT_PATH] ${CMAKE_FIND_ROOT_PATH}")

#### NKR ####

include("cmake/print.cmake")
include("cmake/nkr.cmake")

find_package(Threads)

#### NKR EXTERNAL ####

if (NKR_NO_EXTERNAL)
    set(NKR_NO_GRPC 1)
    set(NKR_NO_YAML 1)
    set(NKR_NO_ZXING 1)
    set(NKR_NO_QHOTKEY 1)
endif ()

# grpc
if (NKR_NO_GRPC)
    nkr_add_compile_definitions(NKR_NO_GRPC)
else ()
    # My proto
    include("cmake/myproto.cmake")
    list(APPEND NKR_EXTERNAL_TARGETS myproto)
endif ()

# yaml-cpp
if (NKR_NO_YAML)
    nkr_add_compile_definitions(NKR_NO_YAML)
else ()
    find_package(yaml-cpp CONFIG REQUIRED) # only Release is built
    list(APPEND NKR_EXTERNAL_TARGETS yaml-cpp)
endif ()

# zxing-cpp
if (NKR_NO_ZXING)
    nkr_add_compile_definitions(NKR_NO_ZXING)
else ()
    find_package(ZXing CONFIG REQUIRED)
    list(APPEND NKR_EXTERNAL_TARGETS ZXing::ZXing)
endif ()

# QHotkey (static submodule)
if (NKR_NO_QHOTKEY)
    nkr_add_compile_definitions(NKR_NO_QHOTKEY)
else ()
    set(QHOTKEY_INSTALL OFF)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(3rdparty/QHotkey)
    list(APPEND NKR_EXTERNAL_TARGETS qhotkey)
endif ()

#### debug print ####

if (DBG_CMAKE)
    print_all_variables()
    print_target_properties(myproto)
    print_target_properties(yaml-cpp)
    print_target_properties(ZXing::ZXing)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_COMMAND} -E time")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK "${CMAKE_COMMAND} -E time")
endif ()

# Sources
set(PROJECT_SOURCES
        ${PLATFORM_SOURCES}

        main/main.cpp
        main/NekoGui.cpp
        main/NekoGui_Utils.cpp
        main/NekoGui_UiThread.cpp
        main/HTTPRequestHelper.cpp

        3rdparty/base64.cpp
        3rdparty/qrcodegen.cpp
        3rdparty/QtExtKeySequenceEdit.cpp

        3rdparty/qv2ray/v2/ui/QvAutoCompleteTextEdit.cpp
        3rdparty/qv2ray/v2/components/proxy/QvProxyConfigurator.cpp
        3rdparty/qv2ray/v2/ui/widgets/common/QJsonModel.cpp
        3rdparty/qv2ray/v2/ui/widgets/editors/w_JsonEditor.cpp
        3rdparty/qv2ray/v2/ui/widgets/editors/w_JsonEditor.hpp
        3rdparty/qv2ray/v2/ui/widgets/editors/w_JsonEditor.ui

        3rdparty/qv2ray/v3/components/GeositeReader/GeositeReader.cpp
        3rdparty/qv2ray/v3/components/GeositeReader/picoproto.cpp

        rpc/gRPC.cpp

        db/Database.cpp
        db/Renumber.cpp
        db/Renumber.hpp
        db/traffic/TrafficLooper.cpp
        db/ProfileFilter.cpp
        db/ConfigBuilder.cpp

        fmt/AbstractBean.cpp
        fmt/Bean2CoreObj_box.cpp
        fmt/Bean2External.cpp
        fmt/Bean2Link.cpp
        fmt/Link2Bean.cpp
        fmt/ChainBean.hpp # translate

        sub/GroupUpdater.cpp

        sys/ExternalProcess.cpp
        sys/AutoRun.cpp

        ui/ThemeManager.cpp
        ui/Icon.cpp

        ui/mainwindow_grpc.cpp
        ui/mainwindow.cpp
        ui/mainwindow.h
        ui/mainwindow.ui

        ui/edit/dialog_edit_profile.h
        ui/edit/dialog_edit_profile.cpp
        ui/edit/dialog_edit_profile.ui
        ui/edit/dialog_edit_group.h
        ui/edit/dialog_edit_group.cpp
        ui/edit/dialog_edit_group.ui

        ui/edit/edit_chain.h
        ui/edit/edit_chain.cpp
        ui/edit/edit_chain.ui
        ui/edit/edit_socks_http.h
        ui/edit/edit_socks_http.cpp
        ui/edit/edit_socks_http.ui
        ui/edit/edit_shadowsocks.h
        ui/edit/edit_shadowsocks.cpp
        ui/edit/edit_shadowsocks.ui
        ui/edit/edit_vmess.h
        ui/edit/edit_vmess.cpp
        ui/edit/edit_vmess.ui
        ui/edit/edit_trojan_vless.h
        ui/edit/edit_trojan_vless.cpp
        ui/edit/edit_trojan_vless.ui

        ui/edit/edit_naive.h
        ui/edit/edit_naive.cpp
        ui/edit/edit_naive.ui

        ui/edit/edit_quic.h
        ui/edit/edit_quic.cpp
        ui/edit/edit_quic.ui

        ui/edit/edit_custom.h
        ui/edit/edit_custom.cpp
        ui/edit/edit_custom.ui

        ui/dialog_basic_settings.cpp
        ui/dialog_basic_settings.h
        ui/dialog_basic_settings.ui

        ui/dialog_manage_groups.cpp
        ui/dialog_manage_groups.h
        ui/dialog_manage_groups.ui

        ui/dialog_manage_routes.cpp
        ui/dialog_manage_routes.h
        ui/dialog_manage_routes.ui

        ui/dialog_vpn_settings.cpp
        ui/dialog_vpn_settings.h
        ui/dialog_vpn_settings.ui

        ui/dialog_hotkey.cpp
        ui/dialog_hotkey.h
        ui/dialog_hotkey.ui

        ui/widget/ProxyItem.cpp
        ui/widget/ProxyItem.h
        ui/widget/ProxyItem.ui
        ui/widget/GroupItem.cpp
        ui/widget/GroupItem.h
        ui/widget/GroupItem.ui

        res/neko.qrc
        res/theme/feiyangqingyun/qss.qrc
        ${QV2RAY_RC}
)

# Shared intermediary lifecycle implementation for production and focused tests.
add_library(nkr_vpn_process STATIC
    sys/VpnProcess.cpp
    sys/VpnProcess.hpp
)
set_target_properties(nkr_vpn_process PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(nkr_vpn_process PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_vpn_process PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Threads::Threads
)

# Pure unary gRPC frame codec shared by production and tests.
add_library(nkr_grpc_frame INTERFACE)
target_include_directories(nkr_grpc_frame INTERFACE
    "${PROJECT_SOURCE_DIR}"
)
target_link_libraries(nkr_grpc_frame INTERFACE
    Qt${QT_VERSION_MAJOR}::Core
)

add_library(nkr_input_escaping STATIC
    main/pure/VpnAuth.cpp
    main/pure/VpnAuth.hpp
    main/pure/ArgCodec.cpp
    main/pure/ArgCodec.hpp
    main/pure/DesktopExec.cpp
    main/pure/DesktopExec.hpp
)
set_target_properties(nkr_input_escaping PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(nkr_input_escaping PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_input_escaping PUBLIC Qt${QT_VERSION_MAJOR}::Core)

# Shared subscription protocol. Compile these implementations once.
add_library(nkr_subscription STATIC
    db/ProfileFilter.cpp
    sub/SubscriptionCommit.cpp
    sub/SubscriptionCommit.hpp
)
set_target_properties(nkr_subscription PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(nkr_subscription PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_subscription PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

# Share the production link parser with regression tests.
add_library(nkr_link_parser STATIC fmt/Link2Bean.cpp)
set_target_properties(nkr_link_parser PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(nkr_link_parser PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_link_parser PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

add_library(nkr_data STATIC
    db/Database.cpp
)
set_target_properties(nkr_data PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(nkr_data PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_data PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

add_library(nkr_timed_process STATIC
    sys/TimedProcess.cpp
    sys/TimedProcess.hpp
)
set_target_properties(nkr_timed_process PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(nkr_timed_process PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_timed_process PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
)

# Keep PROJECT_SOURCES intact for the Qt 5 translation scan.
set(NKR_APPLICATION_SOURCES ${PROJECT_SOURCES})
list(REMOVE_ITEM NKR_APPLICATION_SOURCES
    db/ProfileFilter.cpp
    fmt/Link2Bean.cpp
    db/Database.cpp
)

set(NKR_AUTORUN_TARGET)
if (APPLE)
    find_library(NKR_AUTORUN_COREFOUNDATION NAMES CoreFoundation)
    find_library(NKR_AUTORUN_CORESERVICES NAMES CoreServices)
    if (NOT NKR_AUTORUN_COREFOUNDATION OR NOT NKR_AUTORUN_CORESERVICES)
        message(FATAL_ERROR "macOS autorun requires CoreFoundation and CoreServices")
    endif ()

    add_library(nkr_autorun_mac STATIC
        sys/AutoRun.cpp
        sys/AutoRun.hpp
        sys/AutoRunMac_p.hpp
    )
    set_target_properties(nkr_autorun_mac PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(nkr_autorun_mac PUBLIC
        "${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(nkr_autorun_mac PUBLIC
        Qt${QT_VERSION_MAJOR}::Widgets
        "${NKR_AUTORUN_COREFOUNDATION}"
        "${NKR_AUTORUN_CORESERVICES}"
    )

    list(REMOVE_ITEM NKR_APPLICATION_SOURCES sys/AutoRun.cpp)
    set(NKR_AUTORUN_TARGET nkr_autorun_mac)
endif ()

set(NKR_ENTRY_SOURCES main/main.cpp res/neko.qrc res/theme/feiyangqingyun/qss.qrc ${QV2RAY_RC})
set(NKR_RUNTIME_SOURCES ${NKR_APPLICATION_SOURCES})
list(REMOVE_ITEM NKR_RUNTIME_SOURCES ${NKR_ENTRY_SOURCES})
add_library(nkr_runtime STATIC ${NKR_RUNTIME_SOURCES})
set_target_properties(nkr_runtime PROPERTIES
    AUTOMOC ON AUTOUIC ON AUTORCC ON POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(nkr_runtime PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nkr_runtime PUBLIC nkr_data nkr_timed_process nkr_grpc_frame nkr_subscription nkr_vpn_process nkr_input_escaping nkr_link_parser
    ${NKR_AUTORUN_TARGET}
    Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Svg
    Threads::Threads ${NKR_EXTERNAL_TARGETS} ${PLATFORM_LIBRARIES})


# Qt exe
if (${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(nekobox
            MANUAL_FINALIZATION
            ${NKR_ENTRY_SOURCES}
    )
    # Define target properties for Android with Qt 6 as:
    #    set_property(TARGET nekobox APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
    #                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
    # For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else ()
    if (ANDROID)
        add_library(nekobox SHARED
                ${NKR_ENTRY_SOURCES}
        )
        # Define properties for Android with Qt 5 after find_package() calls as:
        #    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else ()
        add_executable(nekobox
                ${NKR_ENTRY_SOURCES}
        )
    endif ()
endif ()

# Target

set_property(TARGET nekobox PROPERTY AUTOUIC ON)
set_property(TARGET nekobox PROPERTY AUTOMOC ON)
set_property(TARGET nekobox PROPERTY AUTORCC ON)

set_target_properties(nekobox PROPERTIES
        WIN32_EXECUTABLE TRUE
)

# Target Source Translations

set(TS_FILES
        translations/zh_CN.ts
        translations/fa_IR.ts
        translations/ru_RU.ts
)
set(LUPDATE_OPTIONS
        -locations none -no-obsolete
)
if (${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_lupdate(nekobox TS_FILES ${TS_FILES} OPTIONS ${LUPDATE_OPTIONS})
    qt_add_lrelease(nekobox TS_FILES ${TS_FILES} QM_FILES_OUTPUT_VARIABLE QM_FILES)
else ()
    qt5_create_translation(QM_FILES ${PROJECT_SOURCES} ${TS_FILES} OPTIONS ${LUPDATE_OPTIONS})
endif ()
configure_file(translations/translations.qrc ${CMAKE_BINARY_DIR} COPYONLY)
target_sources(nekobox PRIVATE ${CMAKE_BINARY_DIR}/translations.qrc)

# Target Link

target_link_libraries(nekobox PRIVATE nkr_runtime)

if (QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(nekobox)
endif ()

if (NKR_BUILD_TESTS)
    add_subdirectory(tests)
    # W06 also consumes JsonStore; use the shared runtime after its new
    # recovery dependency rather than compiling a partial production graph.
    set_property(TARGET tst_w06_tls_allow_insecure PROPERTY SOURCES
        "${PROJECT_SOURCE_DIR}/tests/w06/tst_tls_allow_insecure.cpp"
        "${PROJECT_SOURCE_DIR}/res/neko.qrc"
        "${PROJECT_SOURCE_DIR}/res/theme/feiyangqingyun/qss.qrc")
    set_property(TARGET tst_w06_tls_allow_insecure PROPERTY AUTORCC ON)
    set_property(TARGET tst_w06_tls_allow_insecure APPEND PROPERTY LINK_LIBRARIES nkr_runtime)
endif ()

if (NKR_BUILD_TESTS)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

    if (NOT NKR_NO_GRPC)
        # Reuse the production runtime library, excluding the process entry point.
        add_executable(tst_rpc_timeouts
            tests/tst_rpc_timeouts.cpp
        )
        set_target_properties(tst_rpc_timeouts PROPERTIES
            AUTOMOC ON
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        )
        target_link_libraries(tst_rpc_timeouts PRIVATE
            nkr_runtime
            Qt${QT_VERSION_MAJOR}::Test
        )
        add_test(NAME rpc_timeouts COMMAND tst_rpc_timeouts)
        # Allow the real five-minute download deadline to finish before QtTest's watchdog.
        set_tests_properties(rpc_timeouts PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=330000"
            TIMEOUT 330
            LABELS "W11"
        )
    endif ()

    foreach(_name IN ITEMS persistence subscription_commit renumber w19)
        add_executable(tst_${_name} tests/tst_${_name}.cpp
            res/neko.qrc res/theme/feiyangqingyun/qss.qrc)
        set_target_properties(tst_${_name} PROPERTIES
            AUTOMOC ON AUTOUIC ON AUTORCC ON
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
        target_link_libraries(tst_${_name} PRIVATE nkr_runtime Qt${QT_VERSION_MAJOR}::Test)
        add_test(NAME ${_name} COMMAND tst_${_name})
        set_tests_properties(${_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    target_compile_definitions(tst_renumber PRIVATE NKR_RENUMBER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(w19 PROPERTIES TIMEOUT 45 LABELS "W19")
    set_tests_properties(persistence PROPERTIES TIMEOUT 90)
    set_tests_properties(subscription_commit PROPERTIES TIMEOUT 180 LABELS "W01;W08;W16")
    # Exhaustive recovery is one QtTest function; its default five-minute
    # watchdog must not preempt CTest's checked crash-matrix timeout.
    set_tests_properties(renumber PROPERTIES
        TIMEOUT 1500 LABELS "W02;crash"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1500000")
endif ()
