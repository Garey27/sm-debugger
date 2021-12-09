cmake_minimum_required(VERSION 3.20)

project(sm_debugger)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# CMake configuration #
set(OUTPUT_NAME "sm_debugger" CACHE STRING "Output name of the extension")
string(REGEX REPLACE " " "" OUTPUT_NAME "${OUTPUT_NAME}")

if(OUTPUT_NAME STREQUAL "")
    message(FATAL_ERROR "OUTPUT_NAME must be configured")
endif()

set(SM_PATH "${CMAKE_CURRENT_SOURCE_DIR}/dep/sourcemod")
set(SP_PATH "${CMAKE_CURRENT_SOURCE_DIR}/dep/sourcemod/sourcepawn")

include("cmake/sourcemod.cmake")

find_package(ZLIB REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_path(BRYNET_INCLUDE_DIRS "brynet/Version.hpp")

# Compiler specific jobs #
if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Arch must be Win32")
endif()

set(CPP_FILES 
"src/sourcepawn/vm/smx-v1-image.cpp"
"src/sourcepawn/vm/file-utils.cpp"
"src/extension.cpp"
"src/debugger.cpp"
"src/utlbuffer.cpp"
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CPP_FILES} ${HPP_FILES})

# Add a executable file #
add_extension(${OUTPUT_NAME} ${CPP_FILES})

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /W3 /showIncludes")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
endif()

target_link_libraries(${OUTPUT_NAME}  PRIVATE ZLIB::ZLIB fmt::fmt)

if(MSVC)
target_link_libraries(${OUTPUT_NAME}  PRIVATE ws2_32)
endif()
set_target_properties(${OUTPUT_NAME}
    PROPERTIES
    CXX_STANDARD 20
    CXX_EXTENSIONS OFF
    )
    
target_include_directories(${OUTPUT_NAME} PRIVATE ${BRYNET_INCLUDE_DIRS})
target_include_directories(${OUTPUT_NAME} PUBLIC "src")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/certify/include")
target_include_directories(${OUTPUT_NAME} PUBLIC "src/sourcepawn/include")
target_include_directories(${OUTPUT_NAME} PUBLIC "src/sourcepawn/vm")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/sourcemod/public/amtl")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/metamod-source/core")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/metamod-source/core/sourcehook")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/hl2sdk/public")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/hl2sdk/public/tier0")
target_include_directories(${OUTPUT_NAME} PUBLIC "dep/hl2sdk/public/tier1")

target_include_directories(${OUTPUT_NAME} PUBLIC ${ZLIB_INCLUDE_DIR})