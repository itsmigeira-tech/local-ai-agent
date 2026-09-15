cmake_minimum_required(VERSION 3.15)
project(LocalAIAgent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /permissive-")
    add_definitions(-DUNICODE -D_UNICODE)
endif()

add_executable(LocalAIAgent WIN32
    src/main.cpp
    src/ai_client.cpp
    src/ai_client.h
    src/json.cpp
    src/json.h
    src/resource.rc
)

target_link_libraries(LocalAIAgent PRIVATE winhttp comctl32 gdi32 user32 shell32 ole32)