cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME basic_framework)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# basic_framework (HNU YueLu) user code: bsp / modules / application
# 框架内部使用 #include "xxx.h" 不带路径, 因此需要把所有含头文件的子目录都加入包含路径
set(FRAMEWORK_DIRS
    ${CMAKE_SOURCE_DIR}/bsp
    ${CMAKE_SOURCE_DIR}/modules
    ${CMAKE_SOURCE_DIR}/application
)
set(FRAMEWORK_SOURCES)
set(FRAMEWORK_INCLUDE_DIRS)
foreach(dir ${FRAMEWORK_DIRS})
    file(GLOB_RECURSE dir_sources CONFIGURE_DEPENDS ${dir}/*.c)
    file(GLOB_RECURSE dir_headers CONFIGURE_DEPENDS ${dir}/*.h)
    list(APPEND FRAMEWORK_SOURCES ${dir_sources})
    foreach(header ${dir_headers})
        get_filename_component(header_dir ${header} DIRECTORY)
        list(APPEND FRAMEWORK_INCLUDE_DIRS ${header_dir})
    endforeach()
endforeach()
list(REMOVE_DUPLICATES FRAMEWORK_INCLUDE_DIRS)

# SEGGER RTT, bsp_log 依赖
set(SEGGER_RTT_DIR ${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/SEGGER)
set(SEGGER_RTT_SOURCES
    ${SEGGER_RTT_DIR}/RTT/SEGGER_RTT.c
    ${SEGGER_RTT_DIR}/RTT/SEGGER_RTT_printf.c
    ${SEGGER_RTT_DIR}/RTT/SEGGER_RTT_ASM_ARMv7M.s
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${FRAMEWORK_SOURCES}
    ${SEGGER_RTT_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${FRAMEWORK_INCLUDE_DIRS}
    ${SEGGER_RTT_DIR}/RTT
    ${SEGGER_RTT_DIR}/Config
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    ARM_MATH_CM4          # arm_math.h 要求指定内核
    DISABLE_LOG_SYSTEM=1  # 关闭 RTT 日志(LOGINFO/LOGWARNING/LOGERROR 置空), 需要时改为 0
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
