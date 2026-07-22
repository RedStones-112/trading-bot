cmake_minimum_required(VERSION 3.15)
project(trading_bot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(trading_bot
    src/main.cpp
    src/kis_client.cpp
    src/http_client.cpp
)

target_link_libraries(trading_bot PRIVATE winhttp)
