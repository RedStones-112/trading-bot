cmake_minimum_required(VERSION 3.15)
project(trading_bot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(trading_bot
    src/main.cpp
    src/kis_client.cpp
    src/mock_broker.cpp
    src/sim_broker.cpp
    src/news_crawler.cpp
    src/http_client.cpp
    src/stock_tags.cpp
    src/event_calendar.cpp
)

target_link_libraries(trading_bot PRIVATE winhttp)

add_executable(test_strategy
    tests/test_strategy.cpp
    src/mock_broker.cpp
    src/news_crawler.cpp
    src/http_client.cpp
    src/event_calendar.cpp
)
target_link_libraries(test_strategy PRIVATE winhttp)
