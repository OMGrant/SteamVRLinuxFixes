cmake_minimum_required(VERSION 3.16)

project(steamvr_compositor_hooks)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Vulkan REQUIRED)

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(FUNCHOOK_INSTALL OFF CACHE BOOL "Install Funchook" FORCE)
add_subdirectory(funchook)

add_library(steamvr_linux_fixes SHARED steamvr_linux_fixes.cpp vulkan_hooks.cpp vrcompositor_patches.cpp timing_trace.cpp vblank_align.cpp present_defer.cpp)

target_include_directories(steamvr_linux_fixes PRIVATE ${Vulkan_INCLUDE_DIRS} funchook)

target_link_libraries(steamvr_linux_fixes PRIVATE Vulkan::Vulkan funchook-static)

set_target_properties(steamvr_linux_fixes PROPERTIES CXX_VISIBILITY_PRESET hidden)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_steamvr_linux_fixes.json")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_steamvr_linux_fixes.json" 
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_steamvr_linux_fixes.json"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/install.sh")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/install.sh"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

install(TARGETS steamvr_linux_fixes
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})