cmake_minimum_required(VERSION 3.10)
project(udp_send_data_speed_by_need CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(udp_send_data_speed_by_need
    src/main.cpp
    src/config.cpp
    src/udp_sender.cpp
)

target_include_directories(udp_send_data_speed_by_need PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
