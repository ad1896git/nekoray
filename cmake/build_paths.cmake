# Also injected into the separately configured static dependencies by
# build_deps_all.sh. Map diagnostics/debug metadata, never runtime load paths.
if (MSVC)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" _nkr_source_path)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _nkr_binary_path)
    set(_nkr_path_options
        "/pathmap:${_nkr_binary_path}=./build"
        "/pathmap:${_nkr_source_path}=./src")
    # Keep the PDB itself in the build directory, but embed only its basename.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /PDBALTPATH:%_PDB%")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /PDBALTPATH:%_PDB%")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(_nkr_mapping IN ITEMS "${CMAKE_SOURCE_DIR}=./src" "${CMAKE_BINARY_DIR}=./build")
        list(APPEND _nkr_path_options
            "-ffile-prefix-map=${_nkr_mapping}"
            "-fdebug-prefix-map=${_nkr_mapping}"
            "-fmacro-prefix-map=${_nkr_mapping}")
    endforeach()
endif ()

# Resource compilers must not receive C/C++ options. OBJCXX is enabled later
# for the macOS entry point and uses the same Clang driver.
foreach(_nkr_option IN LISTS _nkr_path_options)
    add_compile_options("$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>,$<COMPILE_LANGUAGE:OBJCXX>>:${_nkr_option}>")
endforeach()
unset(_nkr_path_options)
