find_package(Catch2 3 CONFIG REQUIRED)

add_executable(adp-tool-tests
	WireTests.cpp
	MsgqTests.cpp
)

# Link adp-core for its public include paths (Model/*, the vendored hidapi
# header pulled in by Reporter.h, etc.). adp_warnings keeps the tests held to
# the same first-party warning bar as the rest of the code.
target_link_libraries(adp-tool-tests PRIVATE adp-core Catch2::Catch2WithMain adp_warnings)

set_target_properties(adp-tool-tests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)

include(Catch)
catch_discover_tests(adp-tool-tests)
