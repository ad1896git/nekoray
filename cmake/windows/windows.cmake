set(PLATFORM_SOURCES 3rdparty/WinCommander.cpp sys/windows/guihelper.cpp sys/windows/MiniDump.cpp)
set(PLATFORM_LIBRARIES wininet wsock32 ws2_32 user32 rasapi32 iphlpapi)

include(cmake/windows/generate_product_version.cmake)
file(STRINGS "${CMAKE_SOURCE_DIR}/nekoray_version.txt" NKR_WINDOWS_VERSION)
if (NOT NKR_WINDOWS_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Windows resource requires a major.minor.patch version")
endif ()
generate_product_version(
        QV2RAY_RC
        VERSION_MAJOR "${CMAKE_MATCH_1}"
        VERSION_MINOR "${CMAKE_MATCH_2}"
        VERSION_PATCH "${CMAKE_MATCH_3}"
        VERSION_REVISION 0
        COMMENTS "nekobox v${NKR_WINDOWS_VERSION}"
        ICON "${CMAKE_SOURCE_DIR}/res/nekobox.ico"
        NAME "nekobox"
        BUNDLE "nekobox"
        COMPANY_NAME "nekobox"
        COMPANY_COPYRIGHT "nekobox"
        FILE_DESCRIPTION "nekobox"
)
add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX)
set(GUI_TYPE WIN32)
if (MINGW)
    if (NOT DEFINED MinGW_ROOT)
        set(MinGW_ROOT "C:/msys64/mingw64")
    endif ()
else ()
    add_compile_options("/utf-8")
    add_compile_options("/std:c++17")
    add_definitions(-D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS)
endif ()
