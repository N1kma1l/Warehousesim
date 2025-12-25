cmake_minimum_required(VERSION 3.20)
project(WholesaleWarehouseSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- ImGui (FetchContent) ---
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.2
)
FetchContent_MakeAvailable(imgui)

# --- SDL2 ---
# Works with vcpkg (CONFIG) and with some system setups.
find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
  message(STATUS "Found SDL2 via CONFIG")
else()
  find_package(SDL2 REQUIRED)
endif()

find_package(OpenGL REQUIRED)

add_library(imgui_lib
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp

  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
)

target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

# SDL2 include dirs / libs differ depending on find mode.
if(TARGET SDL2::SDL2)
  target_link_libraries(imgui_lib PUBLIC SDL2::SDL2)
else()
  target_include_directories(imgui_lib PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(imgui_lib PUBLIC ${SDL2_LIBRARIES})
endif()

target_link_libraries(imgui_lib PUBLIC OpenGL::GL)

add_executable(warehouse_sim
  frontend.cpp
  backend.cpp
)
target_link_libraries(warehouse_sim PRIVATE imgui_lib)

if(MSVC)
  target_compile_options(warehouse_sim PRIVATE /W4)
else()
  target_compile_options(warehouse_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
