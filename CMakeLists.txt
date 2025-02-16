cmake_minimum_required(VERSION 3.10)

project(pw-link-api)

# Find required dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(SPA REQUIRED libspa-0.2)
pkg_check_modules(Pistache REQUIRED libpistache)

# Add source file
add_executable(pw-link-api pw_link_api.cpp)

# Link libraries and include directories
target_include_directories(pw-link-api PUBLIC
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SPA_INCLUDE_DIRS}
    ${Pistache_INCLUDE_DIRS}
)

target_link_libraries(pw-link-api PUBLIC
    ${PIPEWIRE_LIBRARIES}
    ${Pistache_LIBRARIES}
)

# Add compiler flags (optional, but good practice)
target_compile_options(pw-link-api PUBLIC -Wall -D_REENTRANT -g) # -g for debug symbols
