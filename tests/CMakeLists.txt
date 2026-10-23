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
