cmake_minimum_required(VERSION 3.9.4)
project(sdldoom CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)
option(BOLT        "Enable BOLT" OFF)
option(LINKER_GC   "Enable linker section garbage collection" OFF)
option(STATIC_BUILD "Built the emulator statically" OFF)

## D00M SDL2 emulation
file(DOWNLOAD
	https://github.com/fwsGonzo/libriscv/releases/download/v0.17/doom-rv32g
	EXPECTED_MD5 73b9e61f0c8c73cb6706bf955a6f6301
	${CMAKE_SOURCE_DIR}/doom-rv32g)

set(SOURCES
	src/main.cpp
)

if (NATIVE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()
if (BOLT)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -gdwarf-4")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--emit-relocs")
endif()
if (LINKER_GC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-gc-sections")
endif()

if (LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT supported OUTPUT error)
	if (supported)
		message(STATUS "IPO / LTO enabled")
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
	else()
		message(STATUS "IPO / LTO not supported: <${error}>")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
	endif()
endif()

add_subdirectory(../../lib lib)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

add_executable(rvdoom ${SOURCES})
target_link_libraries(rvdoom riscv ${SDL2_LIBRARIES})
if (STATIC_BUILD)
	target_link_libraries(rvdoom -static)
endif()
