cmake_minimum_required(VERSION 3.2)

# CMake configuration #
# Half-Life 2 SDK
set(HL2_PATH "F:/git/AlliedModders/hl2sdk-csgo" CACHE STRING "Path to Half-Life 2 SDK")
string(REGEX REPLACE "\\\\" "/" HL2_PATH "${HL2_PATH}")
string(REGEX REPLACE "//" "/" HL2_PATH "${HL2_PATH}")
