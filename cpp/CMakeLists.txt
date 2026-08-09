cmake_minimum_required(VERSION 3.16)
project(tail_hedge_backtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -O3)

set(WBT_SOURCES
  src/date.cpp
  src/gbs.cpp
  src/yield.cpp
  src/data.cpp
  src/positions.cpp
  src/chain.cpp
  src/expected_pnl.cpp
  src/strategy.cpp
)

add_library(wbt STATIC ${WBT_SOURCES})
target_include_directories(wbt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(ZLIB REQUIRED)
target_link_libraries(wbt PUBLIC ZLIB::ZLIB)

add_executable(w_backtest src/main.cpp)
target_link_libraries(w_backtest PRIVATE wbt)

add_executable(w_search src/w_search.cpp)
target_link_libraries(w_search PRIVATE wbt)
find_package(Threads REQUIRED)
target_link_libraries(w_search PRIVATE Threads::Threads)

add_executable(w_tests tests/test_parity.cpp)
target_link_libraries(w_tests PRIVATE wbt)

enable_testing()
add_test(NAME parity COMMAND w_tests
  ${CMAKE_SOURCE_DIR}/../data/yield-curve-rates-1990-2024.csv
  ${CMAKE_SOURCE_DIR}/../data/SPY_fixture_small.csv
  ${CMAKE_SOURCE_DIR}/../cpp/tools/python_reference.json)
