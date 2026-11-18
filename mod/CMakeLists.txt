# =============================================================================
#  tlou-ii-mp -- TLOU Part II Remastered multiplayer mod (TO1)
#
#  One SHARED target -> winmm.dll: the game's statically-imported winmm proxy
#  and the mod itself in a single file, no separate .asi to load. Third-party:
#  safetyhook (+Zydis) and ws2_32 only.
#
#  One solution, two configurations -- pick the flavor in the VS dropdown:
#    TO1Debug    TO1_LOG diagnostics + the NET debug menu (F1 / R3+Down)
#    TO1Release  shipping build (diagnostics compiled out)
#
#    cmake --preset vs2022                 # build/tlou-ii-mp.sln       (VS2022)
#    cmake --preset vs2026                 # build-vs2026/tlou-ii-mp.slnx (VS2026)
#    cmake --build --preset to1-debug      # or build from inside VS
#    cmake --preset ninja-debug            # clang-cl + Ninja, any prompt (no vcvars needed)
#    cmake --build --preset ninja-debug
#
#  The Ninja path needs only an LLVM toolchain on PATH (clang-cl, lld-link,
#  llvm-rc, llvm-ml, llvm-lib, llvm-strip) plus an installed MSVC + Windows SDK
#  for headers/libs, which clang-cl and lld-link locate on their own.
# =============================================================================
cmake_minimum_required(VERSION 3.25)

foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "${CMAKE_${kind}_LINKER_FLAGS_INIT} /MANIFEST:NO")
endforeach()

project(tlou-ii-mp VERSION 1.3.0 LANGUAGES C CXX RC)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "This project builds with clang-cl only; MSVC cl is untested here (SEH + attribute use, "
        "and the raw mid-hooks are only validated under clang /Od). In Visual Studio, install the "
        "\"C++ Clang tools for Windows\" component and pick a ClangCL preset -- see README/CMakePresets.json.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CONFIGURATION_TYPES "TO1Debug;TO1Release" CACHE STRING "" FORCE)
foreach(lang C CXX)
    set(CMAKE_${lang}_FLAGS_TO1DEBUG   "${CMAKE_${lang}_FLAGS_RELEASE} /Z7")
    set(CMAKE_${lang}_FLAGS_TO1RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
endforeach()
foreach(kind EXE SHARED STATIC MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_TO1DEBUG   "${CMAKE_${kind}_LINKER_FLAGS_RELEASE}")
    set(CMAKE_${kind}_LINKER_FLAGS_TO1RELEASE "${CMAKE_${kind}_LINKER_FLAGS_RELEASE}")
endforeach()
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_TO1DEBUG   "${CMAKE_${kind}_LINKER_FLAGS_TO1DEBUG} /DEBUG /Brepro")
    set(CMAKE_${kind}_LINKER_FLAGS_TO1RELEASE "${CMAKE_${kind}_LINKER_FLAGS_TO1RELEASE} /Brepro")
endforeach()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include("${CMAKE_CURRENT_SOURCE_DIR}/local-config.cmake" OPTIONAL RESULT_VARIABLE TO1_LOCAL_CONFIG)
if(NOT TO1_LOCAL_CONFIG)
    message(FATAL_ERROR
        "local-config.cmake not found. Copy local-config.cmake.example to local-config.cmake "
        "(gitignored) and set TO1_GAME_INSTALLS to your TLOU2 install path(s).")
endif()
if(NOT TO1_GAME_INSTALLS)
    message(FATAL_ERROR "local-config.cmake does not set TO1_GAME_INSTALLS -- it is required.")
endif()
set(TO1_INSTALLS "")
foreach(raw IN LISTS TO1_GAME_INSTALLS)
    file(TO_CMAKE_PATH "${raw}" dir)
    string(REGEX REPLACE "/+$" "" dir "${dir}")
    if(NOT IS_DIRECTORY "${dir}")
        message(FATAL_ERROR "TO1_GAME_INSTALLS lists [${raw}], which is not an existing directory.")
    endif()
    list(APPEND TO1_INSTALLS "${dir}")
endforeach()
option(TO1_STRIP_ASI "Strip symbols from the built winmm.dll (llvm-strip, or strip)" ON)

include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook
    GIT_TAG        629558c64009a7291ba6ed5cfb49187086a27a47)   # pinned: mid-hook ctx layout is relied on
FetchContent_MakeAvailable(safetyhook)
if(TARGET safetyhook)
    target_compile_options(safetyhook PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
endif()

find_program(TO1_GUI2_CLI NAMES gui2 gui2.exe DOC "Naughty Dog GUI2 CLI (gui2.exe)")
find_program(TO1_NDARC    NAMES ndarc ndarc.exe DOC "psarc packer (ndarc.exe)")
if(NOT TO1_GUI2_CLI)
    message(FATAL_ERROR "gui2 CLI not found. Put gui2.exe on PATH, or set TO1_GUI2_CLI in local-config.cmake.")
endif()
if(NOT TO1_NDARC)
    message(FATAL_ERROR "ndarc not found. Put ndarc.exe on PATH or set TO1_NDARC in local-config.cmake.")
endif()

foreach(tool TO1_GUI2_CLI TO1_NDARC)
    if(NOT EXISTS "${${tool}}")
        message(FATAL_ERROR
            "${tool} is set to [${${tool}}] but there is no such file. "
            "Fix the path (local-config.cmake / -D${tool}=...), or clear it and let "
            "CMake find the tool on PATH:  cmake -U${tool} -B <builddir>")
    endif()
endforeach()

get_filename_component(TO1_LLVM_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(TO1_LLVM_STRIP
    NAMES llvm-strip strip
    HINTS "${TO1_LLVM_BIN_DIR}"
    DOC "Symbol stripper for the built winmm.dll (llvm-strip preferred, strip accepted)")
if(TO1_LLVM_STRIP)
    get_filename_component(TO1_STRIP_NAME "${TO1_LLVM_STRIP}" NAME_WE)
endif()
if(TO1_STRIP_ASI AND NOT TO1_LLVM_STRIP)
    message(WARNING
        "Neither llvm-strip nor strip was found next to ${CMAKE_CXX_COMPILER} or on PATH -- "
        "symbol stripping is DISABLED. "
        "Fine for local development. Release builds SHOULD be stripped: install LLVM or set TO1_LLVM_STRIP.")
    set(TO1_STRIP_ASI OFF)
endif()

set(TO1_DATA_DIR    "${CMAKE_BINARY_DIR}/data")
set(TO1_GUI2_PAK    "${TO1_DATA_DIR}/gui2-widgets.pak")
set(TO1_PSARC_STAGE "${TO1_DATA_DIR}/psarc-stage")
set(TO1_PSARC       "${TO1_DATA_DIR}/tlou-ii-mp.psarc")
set(TO1_PSARC_ENC   "${TO1_DATA_DIR}/tlou-ii-mp.psarc.enc")
set(TO1_PSARC_RC    "${TO1_DATA_DIR}/embedded-psarc.rc")
set(TO1_RES_PSARC_ID 1001)


set(TO1_PSARC_KEY   "0x0000000000000000" CACHE STRING "psarc keystream key (64-bit hex)")
set(TO1_PSARC_NONCE "0x0000000000000000" CACHE STRING "psarc keystream nonce (64-bit hex)")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(TO1_VERIFY_KEY "0x0000000000000000" CACHE STRING "psarc sample-offset key (64-bit hex)")
set(TO1_VERIFY_FULL_MAX_MB "128" CACHE STRING
    "Game psarcs up to this many MB are hashed in full at load (bin.psarc, which holds the gameplay dc1 data, is ~77 MB); larger ones fall back to index + sampled payload.")
set(TO1_GAME_DATA_DIR "" CACHE PATH
    "Reference game data dir (…/build/pc/main) whose psarcs get fingerprinted. Empty disables game-archive verification.")
if(NOT TO1_GAME_DATA_DIR)
    list(GET TO1_INSTALLS 0 TO1_FIRST_INSTALL)
    set(TO1_GAME_DATA_DIR "${TO1_FIRST_INSTALL}/build/pc/main" CACHE PATH "" FORCE)
endif()
message(STATUS "Game psarc verification: fingerprinting ${TO1_GAME_DATA_DIR}")
file(GLOB_RECURSE TO1_GAME_PSARCS CONFIGURE_DEPENDS "${TO1_GAME_DATA_DIR}/*.psarc")

set(TO1_PSARC_MANIFEST "${CMAKE_BINARY_DIR}/generated/game-psarc-manifest.inc")
add_custom_command(OUTPUT "${TO1_PSARC_MANIFEST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-psarc-manifest.py"
            "${TO1_GAME_DATA_DIR}" "${TO1_PSARC_MANIFEST}" "${TO1_VERIFY_KEY}" "${TO1_VERIFY_FULL_MAX_MB}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-psarc-manifest.py" ${TO1_GAME_PSARCS}
    COMMENT "fingerprint: game psarcs -> game-psarc-manifest.inc"
    VERBATIM)

file(GLOB_RECURSE TO1_WIDGET_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/widgets/*.widget.json")
file(GLOB_RECURSE TO1_PSARC_SOURCES  CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/psarc/*")
if(NOT TO1_WIDGET_SOURCES)
    message(FATAL_ERROR "widgets/ contains no *.widget.json -- the gui2 pak would be empty.")
endif()
if(NOT TO1_PSARC_SOURCES)
    message(FATAL_ERROR "psarc/ is empty -- nothing to pack.")
endif()

add_custom_command(OUTPUT "${TO1_GUI2_PAK}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TO1_DATA_DIR}"
    COMMAND "${TO1_GUI2_CLI}" build "${CMAKE_CURRENT_SOURCE_DIR}/widgets" "${TO1_GUI2_PAK}" --t2r
    DEPENDS ${TO1_WIDGET_SOURCES}
    COMMENT "gui2: baking widgets/ -> gui2-widgets.pak"
    VERBATIM)

add_custom_command(OUTPUT "${TO1_PSARC}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${TO1_PSARC_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/psarc" "${TO1_PSARC_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy "${TO1_GUI2_PAK}" "${TO1_PSARC_STAGE}/actor97/gui2-widgets.pak"
    COMMAND "${TO1_NDARC}" -c "${TO1_PSARC_STAGE}" -o "${TO1_PSARC}"
    DEPENDS "${TO1_GUI2_PAK}" ${TO1_PSARC_SOURCES}
    COMMENT "ndarc: packing psarc/ + gui2-widgets.pak -> tlou-ii-mp.psarc"
    VERBATIM)

add_custom_command(OUTPUT "${TO1_PSARC_ENC}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/encrypt-resource.py"
            "${TO1_PSARC}" "${TO1_PSARC_ENC}" "${TO1_PSARC_KEY}" "${TO1_PSARC_NONCE}"
    DEPENDS "${TO1_PSARC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/encrypt-resource.py"
    COMMENT "obfuscate: tlou-ii-mp.psarc -> tlou-ii-mp.psarc.enc"
    VERBATIM)

file(CONFIGURE OUTPUT "${TO1_PSARC_RC}"
    CONTENT "${TO1_RES_PSARC_ID} RCDATA \"${TO1_PSARC_ENC}\"\n"
    @ONLY)
set_source_files_properties("${TO1_PSARC_RC}" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${TO1_PSARC_ENC}")

if(NOT CMAKE_ASM_MASM_COMPILER)
    get_filename_component(TO1_LLVM_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(CMAKE_ASM_MASM_COMPILER NAMES llvm-ml64 llvm-ml ml64
        HINTS "${TO1_LLVM_BIN_DIR}"
        DOC "MASM-compatible assembler for src/loader/winmm_thunks.asm")
    if(NOT CMAKE_ASM_MASM_COMPILER)
        message(FATAL_ERROR
            "No MASM assembler found: need llvm-ml64/llvm-ml (LLVM) or ml64 (vcvars) on PATH, "
            "or set CMAKE_ASM_MASM_COMPILER.")
    endif()
endif()
get_filename_component(TO1_MASM_NAME "${CMAKE_ASM_MASM_COMPILER}" NAME_WE)
if(TO1_MASM_NAME STREQUAL "llvm-ml")
    set(CMAKE_ASM_MASM_FLAGS_INIT "-m64")
endif()
enable_language(ASM_MASM)
set(TO1_WINMM_ASM "${CMAKE_CURRENT_SOURCE_DIR}/src/loader/winmm_thunks.asm")
set(TO1_WINMM_DEF "${CMAKE_CURRENT_SOURCE_DIR}/src/loader/winmm_exports.def")
set(TO1_WINMM_INC "${CMAKE_CURRENT_SOURCE_DIR}/src/loader/winmm_names.inc")

add_library(winmm-thunks OBJECT "${TO1_WINMM_ASM}")
source_group("loader" FILES "${TO1_WINMM_ASM}")

file(GLOB_RECURSE MOD_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.c)
file(GLOB_RECURSE MOD_HEADERS CONFIGURE_DEPENDS src/*.hpp src/*.h)

add_library(tlou-ii-mp SHARED ${MOD_SOURCES} ${MOD_HEADERS} "${TO1_PSARC_RC}" "${TO1_PSARC_MANIFEST}"
    "${TO1_WINMM_DEF}" "${TO1_WINMM_INC}")
set_source_files_properties("${TO1_PSARC_MANIFEST}" PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
set_source_files_properties("${TO1_WINMM_INC}" PROPERTIES HEADER_FILE_ONLY TRUE)


source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${MOD_SOURCES} ${MOD_HEADERS})
source_group("data" FILES "${TO1_PSARC_RC}")
source_group("loader" FILES "${TO1_WINMM_DEF}" "${TO1_WINMM_INC}")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT tlou-ii-mp)
foreach(dep safetyhook Zydis zydis Zycore zycore)
    if(TARGET ${dep})
        set_target_properties(${dep} PROPERTIES FOLDER "third_party")
    endif()
endforeach()

target_include_directories(tlou-ii-mp PRIVATE
    src
    "${CMAKE_BINARY_DIR}/generated"
    src/net/base src/net/protocol src/net/online src/net/match src/net/anim src/net/game src/net/ui)

target_compile_definitions(tlou-ii-mp PRIVATE
    _CRT_SECURE_NO_WARNINGS
    TO1_RES_PSARC_ID=${TO1_RES_PSARC_ID}
    TO1_PSARC_KEY=${TO1_PSARC_KEY}ull
    TO1_PSARC_NONCE=${TO1_PSARC_NONCE}ull
    TO1_VERIFY_KEY=${TO1_VERIFY_KEY}ull
    $<$<CONFIG:TO1Debug>:TO1_DEBUG>
    $<$<CONFIG:TO1Release>:TO1_RELEASE>)

target_compile_options(tlou-ii-mp PRIVATE
    $<$<CONFIG:TO1Debug>:/Od>
    -fno-strict-aliasing
    /GR-
    -Wno-unused-private-field
    -Wno-unused-variable
    -Wno-ignored-qualifiers
    -Wno-reorder-ctor
    -Wno-microsoft-unqualified-friend)

target_link_libraries(tlou-ii-mp PRIVATE safetyhook ws2_32 iphlpapi bcrypt winmm-thunks)

set_target_properties(tlou-ii-mp PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(TO1_STAMP_DIR "${CMAKE_BINARY_DIR}/stamps/$<CONFIG>")

set(TO1_POST_BUILD_STAMPS "")
set(TO1_POST_BUILD_PREV "$<TARGET_FILE:tlou-ii-mp>")

if(TO1_STRIP_ASI)
    set(stamp "${TO1_STAMP_DIR}/strip.stamp")
    add_custom_command(OUTPUT "${stamp}"
        COMMAND "${TO1_LLVM_STRIP}" --strip-all "$<TARGET_FILE:tlou-ii-mp>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TO1_STAMP_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS "${TO1_POST_BUILD_PREV}"
        COMMENT "${TO1_STRIP_NAME}: stripping winmm.dll"
        VERBATIM)
    list(APPEND TO1_POST_BUILD_STAMPS "${stamp}")
    set(TO1_POST_BUILD_PREV "${stamp}")
endif()

set(n 0)
foreach(dir IN LISTS TO1_INSTALLS)
    set(stamp "${TO1_STAMP_DIR}/deploy${n}.stamp")
    add_custom_command(OUTPUT "${stamp}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:tlou-ii-mp>" "${dir}/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TO1_STAMP_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS "${TO1_POST_BUILD_PREV}"
        COMMENT "deploy: winmm.dll -> ${dir}"
        VERBATIM)
    list(APPEND TO1_POST_BUILD_STAMPS "${stamp}")
    set(TO1_POST_BUILD_PREV "${stamp}")
    math(EXPR n "${n} + 1")
endforeach()

if(TO1_POST_BUILD_STAMPS)
    add_custom_target(tlou-ii-mp-deploy ALL DEPENDS ${TO1_POST_BUILD_STAMPS})
endif()
