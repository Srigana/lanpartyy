cmake_minimum_required(VERSION 3.16)
project(lan_party C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SANITIZE "build with AddressSanitizer and UBSan" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(netc STATIC
    netc/src/packet.c
    netc/src/socket.c
    netc/src/endpoint.c
)
target_include_directories(netc PUBLIC netc/include)
target_compile_definitions(netc PRIVATE _POSIX_C_SOURCE=200809L)

add_executable(lanserver
    server/src/main.cpp
    server/src/server.cpp
    server/src/world.cpp
    server/src/snapshot.cpp
)
target_link_libraries(lanserver PRIVATE netc)

enable_testing()
add_executable(test_netc netc/tests/test_netc.c)
target_link_libraries(test_netc PRIVATE netc)
add_test(NAME netc COMMAND test_netc)
