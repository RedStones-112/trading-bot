cmake_minimum_required(VERSION 3.15)
project(trading_bot CXX C) # C needed for third_party/genann.c (ml_model.cpp's neural net)

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
    src/ml_model.cpp
    third_party/genann.c
)

target_link_libraries(trading_bot PRIVATE winhttp)

add_executable(test_strategy
    tests/test_strategy.cpp
    src/mock_broker.cpp
    src/news_crawler.cpp
    src/http_client.cpp
    src/event_calendar.cpp
    src/ml_model.cpp
    third_party/genann.c
)
target_link_libraries(test_strategy PRIVATE winhttp)

# Offline backtest: replays strategy.hpp's signal functions over real historical daily
# bars (read-only KIS calls, no orders) to validate probability_mode/trend-filter edge
# without live trial-and-error. See src/backtest.cpp.
add_executable(backtest
    src/backtest.cpp
    src/kis_client.cpp
    src/http_client.cpp
)
target_link_libraries(backtest PRIVATE winhttp)
