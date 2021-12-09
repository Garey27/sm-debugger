cmake_minimum_required(VERSION 3.2)

# CMake configuration #
# SourceMod SDK
string(REGEX REPLACE "\\\\" "/" SM_PATH "${SM_PATH}")
string(REGEX REPLACE "//" "/" SM_PATH "${SM_PATH}")

if(NOT EXISTS "${SM_PATH}/public")
    message(FATAL_ERROR "Require a path to SourceMod SDK")
endif()

# SourcePawn
string(REGEX REPLACE "\\\\" "/" SP_PATH "${SP_PATH}")
string(REGEX REPLACE "//" "/" SP_PATH "${SP_PATH}")

if(NOT EXISTS "${SP_PATH}/include")
    if(EXISTS "${SM_PATH}/sourcepawn/include")
        message(STATUS "Using a SourcePawn from SourceMod SDK")
    else()
        message(FATAL_ERROR "Require a path to SourcePawn")
    endif()
endif()

# AlliedModders C++ Template Library
set(AMTL_PATH "" CACHE STRING "Path to SourcePawn")
string(REGEX REPLACE "\\\\" "/" AMTL_PATH "${AMTL_PATH}")
string(REGEX REPLACE "//" "/" AMTL_PATH "${AMTL_PATH}")

if(NOT EXISTS "${AMTL_PATH}/amtl")
    if(EXISTS "${SM_PATH}/public/amtl/amtl")
        message(STATUS "Using a AMTL from SourceMod SDK")
    else()
        message(FATAL_ERROR "Require a path to AMTL")
    endif()
endif()

# Add header and source files #
include_directories("${SM_PATH}/public")
include_directories("${SM_PATH}/public/extensions")
include_directories("${SM_PATH}/sourcepawn/include")
include_directories("${SM_PATH}/public/amtl")
include_directories("${SM_PATH}/public/amtl/amtl")

# Add a executable file #
function(add_extension ext_name)
    if(ARGC LESS 3)
        message(FATAL_ERROR "Missing arguments for add_extension")
    endif()

    add_library(${ext_name} SHARED ${ARGN} "${SM_PATH}/public/smsdk_ext.cpp" )

    # Define some macros
    if(WIN32)
        target_compile_definitions(${ext_name} PUBLIC WIN32)
    endif()
    target_compile_definitions(${ext_name} PUBLIC SOURCEMOD_BUILD)

    # Try to statically link with C++ runtime
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        # Lesser than 3.15
        if(${CMAKE_VERSION} VERSION_LESS "3.15.0")
            # Warning D9025
            target_compile_options(${OUTPUT_NAME} PUBLIC "$<$<CONFIG:Debug>:/MTd>")
            target_compile_options(${OUTPUT_NAME} PUBLIC "$<$<CONFIG:Release>:/MT>")
        else()
            set_target_properties(${ext_name} PROPERTIES MSVC_RUNTIME_LIBRARY
                                  "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()

        # Disable the warnings
        target_compile_options(${ext_name} PUBLIC /wd26439 /wd26495)
        target_compile_definitions(${ext_name} PUBLIC _CRT_SECURE_NO_WARNINGS)

    elseif(UNIX)
        if("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU" OR
           "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
            # Lesser than 3.13
            if(NOT COMMAND target_link_options)
                function(target_link_options)
                    target_link_libraries(${ARGV})
                endfunction()
            endif()

            # Use libstdc++ instead of libc++
            #target_compile_options(${ext_name} PUBLIC -stdlib=libstdc++)
            target_link_options(${ext_name} PUBLIC -static-libstdc++ -static-libgcc)
        endif()
    endif()

    # -fpic
    set_target_properties(${ext_name} PROPERTIES POSITION_INDEPENDENT_CODE True)

    # -std=c++14
    set_target_properties(${ext_name} PROPERTIES CXX_STANDARD 14)
    set_target_properties(${ext_name} PROPERTIES CXX_STANDARD_REQUIRED ON)

    # -o (output)
    set_target_properties(${ext_name} PROPERTIES PREFIX "")

    if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
        set_target_properties(${ext_name} PROPERTIES SUFFIX ".ext.dll")
    elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set_target_properties(${ext_name} PROPERTIES SUFFIX ".ext.dylib")
    else()
        set_target_properties(${ext_name} PROPERTIES SUFFIX ".ext.so")
    endif()
endfunction()
