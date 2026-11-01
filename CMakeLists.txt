cmake_minimum_required( VERSION 3.20 )
project(ccmatrix VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(source main.c config.c text.c)

add_executable( ${PROJECT_NAME} ${source} )
target_compile_options( ${PROJECT_NAME} PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -pedantic> )

# `cmake --install build` puts the binary in bin/, and the example config,
# sample message and screensaver hooks under share/.
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md ccmatrix.conf.example
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY messages contrib
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})
