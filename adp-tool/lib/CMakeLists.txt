# Load dependencies start

include(hidapi.cmake)
add_subdirectory (avrdude)
include(serial.cmake)

include(esp-flasher.cmake)
include(ixwebsocket.cmake)