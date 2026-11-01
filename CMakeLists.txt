cmake_minimum_required( VERSION 3.20 )
project(ccmatrix)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(source main.c)

add_executable( ${PROJECT_NAME} ${source} )
