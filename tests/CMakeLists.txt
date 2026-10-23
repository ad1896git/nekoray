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
