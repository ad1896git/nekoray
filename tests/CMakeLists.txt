add_executable(nkr_smoke_test smoke_test.cpp)

set_target_properties(nkr_smoke_test PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

target_link_libraries(nkr_smoke_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Test
)

add_test(NAME nkr_smoke_test COMMAND nkr_smoke_test)
set_tests_properties(nkr_smoke_test PROPERTIES TIMEOUT 30)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(tst_stop_traffic_persistence
        tst_stop_traffic_persistence.cpp
)
set_target_properties(tst_stop_traffic_persistence PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
)
target_include_directories(tst_stop_traffic_persistence PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_link_libraries(tst_stop_traffic_persistence PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME stop_traffic_persistence
        COMMAND tst_stop_traffic_persistence
)
set_tests_properties(stop_traffic_persistence PROPERTIES TIMEOUT 15)

add_subdirectory(w06)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(tst_external_credentials
    tst_external_credentials.cpp
    ${PROJECT_SOURCE_DIR}/sys/ExternalProcess.cpp
)
set_target_properties(tst_external_credentials PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(tst_external_credentials PRIVATE
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(tst_external_credentials PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME external_credentials COMMAND tst_external_credentials)
set_tests_properties(external_credentials PROPERTIES TIMEOUT 60)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Test)
find_package(Threads REQUIRED)

add_executable(nkr_ui_thread
        tst_ui_thread.cpp
        ${PROJECT_SOURCE_DIR}/main/NekoGui_UiThread.cpp
)

set_target_properties(nkr_ui_thread PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
)

target_include_directories(nkr_ui_thread PRIVATE
        ${PROJECT_SOURCE_DIR}
)

# Assertion probes must run even in the required Release build.
# This definition affects this test executable only.
target_compile_definitions(nkr_ui_thread PRIVATE QT_FORCE_ASSERTS)

target_link_libraries(nkr_ui_thread PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Test
        Threads::Threads
)

add_test(NAME nkr_ui_thread COMMAND nkr_ui_thread)
set_tests_properties(nkr_ui_thread PROPERTIES TIMEOUT 60)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Test)
find_package(Threads REQUIRED)

add_executable(tst_w04
    tst_w04.cpp
)

set_target_properties(tst_w04 PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(tst_w04 PRIVATE
    "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(tst_w04 PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Test
    Threads::Threads
)

add_test(NAME w04 COMMAND tst_w04)
set_tests_properties(w04 PROPERTIES TIMEOUT 45)

add_executable(tst_w03b
    tst_w03b.cpp
    ${PROJECT_SOURCE_DIR}/main/NekoGui_UiThread.cpp
)
set_target_properties(tst_w03b PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(tst_w03b PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(tst_w03b PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Test
    Threads::Threads
)
add_test(NAME w03b COMMAND tst_w03b)
set_tests_properties(w03b PROPERTIES TIMEOUT 30)

if (APPLE)
    add_executable(tst_autorun_mac
        tst_autorun_mac.cpp
    )
    set_target_properties(tst_autorun_mac PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(tst_autorun_mac PRIVATE
        nkr_autorun_mac
        Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME autorun_mac COMMAND tst_autorun_mac)
    set_tests_properties(autorun_mac PROPERTIES
        TIMEOUT 30
        LABELS "W23a"
    )
endif ()

foreach(work IN ITEMS 13 14 17)
    add_executable(tst_w${work} tst_w${work}.cpp)
    set_target_properties(tst_w${work} PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(tst_w${work} PRIVATE
        nkr_input_escaping
        Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME w${work} COMMAND tst_w${work})
    set_tests_properties(w${work} PROPERTIES
        TIMEOUT 90
        LABELS "PR11;W${work}"
    )
endforeach()

target_compile_definitions(tst_w13 PRIVATE
    NKR_VPN_TEMPLATE_PATH="${PROJECT_SOURCE_DIR}/res/vpn/sing-box-vpn.json"
)

add_executable(tst_grpc_frame
    tst_grpc_frame.cpp
)
set_target_properties(tst_grpc_frame PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(tst_grpc_frame PRIVATE
    nkr_grpc_frame
    Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME grpc_frame COMMAND tst_grpc_frame)
set_tests_properties(grpc_frame PROPERTIES
    TIMEOUT 30
    LABELS "W10"
)

add_executable(tst_runtime_fixes
    tst_runtime_fixes.cpp
)
set_target_properties(tst_runtime_fixes PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(tst_runtime_fixes PRIVATE
    nkr_vpn_process
    Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME runtime_fixes COMMAND tst_runtime_fixes)
set_tests_properties(runtime_fixes PROPERTIES
    TIMEOUT 60
    LABELS "W12;W15;W18"
)
