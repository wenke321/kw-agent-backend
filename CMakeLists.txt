

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
#set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} -std=gnu++17)
#set(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} -std=gnu17)

set(pchs
    <cstdint>
    <memory>
    <cmath>
    <string>
    <functional>
    <unordered_map>
    <cstdio>
    <cstdlib>
    <cstring>
    <algorithm>
    <cerrno>
    <fstream>
    <thread>
    <limits>
    <charconv>
    <cstring>
    [["json.hpp"]]
    [["sqlite3.h"]]
    [["curl/curl.h"]]
    [["spdlog/spdlog.h"]]
)

if(WIN32)
    set(pchs
        ${pchs}
        <Windows.h>
        <io.h>
        <process.h>
    )
elseif(LINUX)
    set(pchs
        ${pchs}
        <unistd.h>
        <fcntl.h>
        <sys/stat.h>
        <poll.h>
        <signal.h>
        <spawn.h>
        <sys/file.h>
        <sys/prctl.h>
        <sys/resource.h>
        <sys/stat.h>
        <sys/syscall.h>
        <sys/types.h>
        <sys/wait.h>
        <sys/xattr.h>
        <unistd.h>
    )
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(ALL-SRC
    ${SRC_DIR}/https/https_client.cpp
    ${SRC_DIR}/llm_tools.cpp
    ${SRC_DIR}/agent_loop.cpp
    ${SRC_DIR}/session.cpp
    ${SRC_DIR}/history_manager.cpp
    ${SRC_DIR}/helpers/function.cpp
)

add_library(kw_agent_backend SHARED ${ALL-SRC})

if(WIN32)
    target_link_libraries(kw_agent_backend PRIVATE
        #ssl-4-x64
        #llhttp
        #crypto-4-x64
        spdlog
        curl-x64
        sqlite3
    )
elseif(LINUX)
    target_link_libraries(kw_agent_backend PRIVATE
        # ssl
        # crypto
        #llhttp
        spdlog
        curl
        sqlite3
        # ngtcp2_crypto_quictls
        #ngtcp2
        # nghttp3
        # OpenSSL::SSL
        # OpenSSL::Crypto
    )
endif()

target_include_directories(kw_agent_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/json/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/spdlog/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sqlite/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/curl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    #${CMAKE_CURRENT_SOURCE_DIR}/third-party/llhttp
    #${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl
)

target_link_directories(kw_agent_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/spdlog/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/curl/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sqlite/lib
    #${CMAKE_CURRENT_SOURCE_DIR}/third-party/llhttp/lib
    #${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl/lib
)

target_precompile_headers(kw_agent_backend PUBLIC ${pchs})

add_subdirectory(test)

target_compile_options(kw_agent_backend
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<BOOL:${GNU_BIG_OBJ_FLAG_ENABLE}>>:-Wa,-mbig-obj,-Wall -Wextra -Wpedantic>
    )

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(kw_agent_backend PUBLIC -Wall -Wextra -Wpedantic)
endif()



