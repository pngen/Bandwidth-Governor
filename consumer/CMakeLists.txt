cmake_minimum_required(VERSION 3.21)
project(BandwidthConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Downstream consumer: locates the installed Bandwidth Governor package.
find_package(BandwidthGovernor CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Bandwidth::bgcore)
