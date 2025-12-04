cmake_minimum_required(VERSION 3.5)
project(sdlamp)

add_subdirectory(vendored/physfs)

# Create an option to switch between a system sdl library and a vendored sdl library
add_subdirectory(vendored/SDL EXCLUDE_FROM_ALL)

# Create your game executable target as usual
add_executable(sdlamp sdlamp.c)

target_include_directories(sdlamp PRIVATE
    vendored/SDL/include
    vendored/physfs/src
)

# Link to the actual SDL2 library. SDL2::SDL2 is the shared SDL library, SDL2::SDL2-static is the static SDL libarary.
target_link_libraries(sdlamp PRIVATE SDL2::SDL2-static physfs-static)

