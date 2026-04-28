cmake_minimum_required(VERSION 3.20)

project(chiux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(X11 REQUIRED)
find_package(PNG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft fontconfig)

add_executable(chiux
  src/main.cpp
  src/config/config.cpp
  src/util/log.cpp
  src/util/process.cpp
  src/x11/connection.cpp
  src/ui/theme.cpp
  src/ui/menubar.cpp
  src/ui/desktop.cpp
  src/ui/popup_menu.cpp
  src/wm/client.cpp
  src/wm/window_manager.cpp
)

add_executable(chiux-te
  src/terminal/main.cpp
  src/config/config.cpp
  src/util/log.cpp
)

add_executable(chiux-te-2
  src/terminal2/main.cpp
  src/config/config.cpp
  src/util/log.cpp
)

target_include_directories(chiux PRIVATE src)
target_include_directories(chiux-te PRIVATE src)
target_include_directories(chiux-te-2 PRIVATE src)
target_link_libraries(chiux PRIVATE X11::X11 PNG::PNG)
target_link_libraries(chiux-te PRIVATE X11::X11)
target_link_libraries(chiux-te-2 PRIVATE X11::X11 PkgConfig::XFT)
target_compile_definitions(chiux PRIVATE CHIUX_VERSION="${PROJECT_VERSION}")
target_compile_definitions(chiux-te PRIVATE CHIUX_VERSION="${PROJECT_VERSION}")
target_compile_definitions(chiux-te-2 PRIVATE CHIUX_VERSION="${PROJECT_VERSION}")

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(chiux PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  target_compile_options(chiux-te PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  target_compile_options(chiux-te-2 PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

install(TARGETS chiux chiux-te chiux-te-2 RUNTIME DESTINATION bin)
install(FILES res/chimera.png DESTINATION share/chiux/res)
