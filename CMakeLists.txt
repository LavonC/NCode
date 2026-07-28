cmake_minimum_required(VERSION 3.15)
project(NCode VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)
find_package(SDL3_net CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(llama CONFIG REQUIRED) 

add_executable(NCode main.cpp)

target_link_libraries(NCode PRIVATE 
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_mixer::SDL3_mixer
    SDL3_net::SDL3_net
    SDL3_ttf::SDL3_ttf
    llama                           
)

add_custom_command(TARGET NCode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/font.ttf"
    "$<TARGET_FILE_DIR:NCode>/font.ttf"
)
