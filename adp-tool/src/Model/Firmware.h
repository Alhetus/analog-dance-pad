#pragma once

#include <string>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace adp
{

enum ArchType
{
	ARCH_UNKNOWN,
	ARCH_AVR,
	ARCH_ESP,
	ARCH_AVR_ARD
};

enum BoardType
{
	BOARD_UNKNOWN,
	BOARD_FSRMINIPAD,
	BOARD_FSRMINIPAD_V2,
	BOARD_FSRMINIPAD_V4,
	BOARD_TEENSY2,
	BOARD_LEONARDO,
	BOARD_FSRIO_V1,
	BOARD_FSRIO_V2,
	BOARD_FSRIO_V3,
};

enum FlashResult
{
	FLASHRESULT_NOTHING,
	FLASHRESULT_CONNECTED,
	FLASHRESULT_SUCCESS,
	FLASHRESULT_FAILURE,
	FLASHRESULT_FAILURE_BOARDTYPE,
	FLASHRESULT_RUNNING,
	FLASHRESULT_PROGRESS,
	FLASHRESULT_MESSAGE,
	FLASHRESULT_CANCELLED
};

struct BoardTypeStruct
{
	BoardTypeStruct() : archType(ARCH_UNKNOWN), boardType(BOARD_UNKNOWN) {}

	BoardTypeStruct(std::string boardType);

	BoardTypeStruct(ArchType archType, BoardType boardType) : archType(archType), boardType(boardType) {}

	static ArchType ParseArchType(const std::string& str);
	static BoardType ParseBoardType(const std::string& str);

	bool CompatibleWith(BoardTypeStruct other, bool strict = true) const;

	ArchType archType = ARCH_UNKNOWN;
	BoardType boardType = BOARD_UNKNOWN;

	std::string ToString() const;
};

} // namespace adp