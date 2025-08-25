cmake_minimum_required (VERSION 3.6)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")
project(adp-tool VERSION 2.0)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Adp.h.in" "${CMAKE_CURRENT_SOURCE_DIR}/src/Adp.h")

# Load dependencies start
if(WIN32)
	SET(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "Triplet" FORCE)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")

find_package(unofficial-nativefiledialog REQUIRED)
find_package(libzippp REQUIRED)

add_subdirectory(lib)

find_package(fmt REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Stb REQUIRED)
find_package(argparse REQUIRED)
# Load dependencies end

# find_path(SYSTEM_INCLUDE_DIR zlib.h)

include_directories(
	"lib/avrdude"
	"lib/argparse"
	"lib/mbedtls"
	"lib/ixwebsocket"
	"src"
	# ${SYSTEM_INCLUDE_DIR}
)

set(GLOBAL_LIBS
	hidapi
	fmt::fmt
	nlohmann_json::nlohmann_json
	esp_serial
	ixwebsocket
)

list(APPEND GLOBAL_LIBS avrdude serial libzippp::libzippp)

if(WIN32 OR MINGW)
	list(APPEND GLOBAL_LIBS setupapi ws2_32)
elseif(UNIX)
	list(APPEND GLOBAL_LIBS udev X11)
endif()
# Load dependencies end

# GUI application start

set(GUI_SOURCES
	"src/Model/Device.cpp"
	"src/Model/Firmware.cpp"
	"src/Model/Reporter.cpp"
	"src/Model/Utils.cpp"
	"src/Application.cpp"
	"src/WebsocketServer.cpp"
	"src/Main.cpp"
)

#source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${GUI_SOURCES})
#set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

set(GUI_LIBS
	${GLOBAL_LIBS}
)

add_executable (${PROJECT_NAME} ${GUI_SOURCES})

if(MINGW)
	target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
	CXX_STANDARD 20
	CXX_EXTENSIONS OFF)

target_link_libraries(${PROJECT_NAME} ${GUI_LIBS})

if(WIN32)
	target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:Console)
endif()

# GUI application end
