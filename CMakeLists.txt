cmake_minimum_required(VERSION 3.20)
project(SentinelAnalysisCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# -O3 + native tuning for the analysis core's hot path. Drop -march=native
# if you're cross-compiling or shipping a binary to a different CPU.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

find_package(Boost 1.81 REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# nlohmann-json: try the package config first (apt: nlohmann-json3-dev,
# brew: nlohmann-json). If it's not installed, vendor the single-header
# amalgam from https://github.com/nlohmann/json into
# include/nlohmann/json.hpp and this falls back to plain include-dir usage.
find_package(nlohmann_json 3.10 QUIET)

add_executable(sentinel_core
    src/Sentinel_Main.cpp
    src/Sentinel_Network.cpp
    src/Sentinel_Analysis_Core.cpp
)

target_include_directories(sentinel_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(sentinel_core PRIVATE
    Boost::system
    Boost::thread
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

if(nlohmann_json_FOUND)
    target_link_libraries(sentinel_core PRIVATE nlohmann_json::nlohmann_json)
endif()

target_compile_definitions(sentinel_core PRIVATE
    BOOST_ASIO_NO_DEPRECATED
)

# Boost.Beast pulls in a lot of Asio headers; keep compile times sane.
target_compile_definitions(sentinel_core PRIVATE
    BOOST_BEAST_USE_STD_STRING_VIEW
)
