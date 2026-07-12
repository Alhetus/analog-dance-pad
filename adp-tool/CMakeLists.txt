# CMakePresets (v3) require CMake >= 3.21.
cmake_minimum_required (VERSION 3.21)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")
project(adp-tool VERSION 2.0)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Adp.h.in" "${CMAKE_CURRENT_SOURCE_DIR}/src/Adp.h")

option(ADP_BUILD_TESTS "Build the adp-tool unit tests" ON)
option(ADP_WERROR "Treat first-party compiler warnings as errors" ON)

# Load dependencies start
if(WIN32)
	SET(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "Triplet" FORCE)
endif()

# Pull in the test-only vcpkg dependencies (catch2) only when tests are enabled.
if(ADP_BUILD_TESTS)
	list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")

add_subdirectory(lib)

find_package(fmt REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(argparse REQUIRED)
# Load dependencies end

# Third-party include directories are marked SYSTEM so that first-party
# warnings-as-errors (ADP_WERROR) does not trip over warnings in vendored headers.
include_directories(SYSTEM
	"lib/avrdude"
	"lib/argparse"
	"lib/mbedtls"
	"lib/ixwebsocket"
)

set(GLOBAL_LIBS
	hidapi
	fmt::fmt
	nlohmann_json::nlohmann_json
	esp_serial
	ixwebsocket
)

list(APPEND GLOBAL_LIBS avrdude serial)

if(WIN32 OR MINGW)
	list(APPEND GLOBAL_LIBS setupapi ws2_32)
elseif(APPLE)
	# hidapi's mac backend and serial's list_ports_osx need these frameworks
	# (the udev/X11 the Linux branch links do not exist on macOS).
	list(APPEND GLOBAL_LIBS "-framework IOKit" "-framework CoreFoundation" "-framework AppKit")
elseif(UNIX)
	list(APPEND GLOBAL_LIBS udev X11)
endif()
# Load dependencies end

# First-party warning flags, exposed as an INTERFACE target so they apply only
# to our own targets (never to the vendored libraries under lib/).
add_library(adp_warnings INTERFACE)
if(MSVC)
	# /W4 with a couple of exclusions so warnings-as-errors matches the GCC/Clang
	# bar below (-Wall -Wextra does NOT flag implicit narrowing; that needs
	# -Wconversion, which we do not enable). C4244/C4267 are the pervasive,
	# intentional narrowings in the packed HID wire structs.
	# /wd4127 (constant conditional) is MSVC-only noise from the UTF-8 tables in
	# Utils.cpp; GCC/Clang -Wall -Wextra do not flag it.
	target_compile_options(adp_warnings INTERFACE /W4 /permissive- /wd4244 /wd4267 /wd4127)
	if(ADP_WERROR)
		target_compile_options(adp_warnings INTERFACE /WX)
	endif()
else()
	target_compile_options(adp_warnings INTERFACE -Wall -Wextra -Wpedantic)
	if(ADP_WERROR)
		target_compile_options(adp_warnings INTERFACE -Werror)
	endif()
endif()

# Core library: everything except the entry point, so it can be linked into both
# the executable and the unit-test target.
set(CORE_SOURCES
	"src/Model/Device.cpp"
	"src/Model/Firmware.cpp"
	"src/Model/Reporter.cpp"
	"src/Model/Utils.cpp"
	"src/Application.cpp"
	"src/WebsocketServer.cpp"
)

add_library(adp-core STATIC ${CORE_SOURCES})
target_include_directories(adp-core PUBLIC "src")
target_link_libraries(adp-core PUBLIC ${GLOBAL_LIBS})
target_link_libraries(adp-core PRIVATE adp_warnings)
set_target_properties(adp-core PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)

# Application executable.
add_executable(${PROJECT_NAME} "src/Main.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE adp-core adp_warnings)
set_target_properties(${PROJECT_NAME} PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)

if(MINGW)
	target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++)
endif()

if(WIN32)
	target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:Console)
endif()

# Unit tests.
if(ADP_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
