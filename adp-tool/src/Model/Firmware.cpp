#include <chrono>
#include <algorithm>

#include "libzippp.h"
#include <serial/serial.h>
using namespace serial;

#include <Model/Firmware.h>
#include <Model/Device.h>

using namespace std;
using namespace chrono;

namespace adp {

size_t split_str(const std::string &txt, std::vector<std::string> &strs, char ch)
{
    size_t pos = txt.find( ch );
    size_t initialPos = 0;
    strs.clear();

    // Decompose statement
    while( pos != std::string::npos ) {
        strs.push_back( txt.substr( initialPos, pos - initialPos ) );
        initialPos = pos + 1;

        pos = txt.find( ch, initialPos );
    }

    // Add the last one
    strs.push_back( txt.substr( initialPos, std::min( pos, txt.size() ) - initialPos + 1 ) );

    return strs.size();
}

void ListSerialPorts(std::vector<std::string>& ports)
{
	vector<PortInfo> comPorts = list_ports();
	for (PortInfo port : comPorts)
	{
		ports.push_back(port.port);
	}
}

// BoardTypeStruct

BoardTypeStruct::BoardTypeStruct(std::string boardTypeString)
{
	std::vector<std::string> pts;
	split_str(boardTypeString, pts, '_');
	if(pts.size() == 1)
	{
		archType = ARCH_AVR;
		boardType = ParseBoardType(pts[0]);
	}
	else if(pts.size() == 2)
	{
		archType = ParseArchType(pts[0]);
		boardType = ParseBoardType(pts[1]);
	}
}

ArchType BoardTypeStruct::ParseArchType(const std::string& str)
{
	if (str == "avr" || str == "m32u4")
		return ARCH_AVR;
	
	if (str == "esp" || str == "esp32s3")
		return ARCH_ESP;
	
	if (str == "avrard") { return ARCH_AVR_ARD; }
	
	return ARCH_UNKNOWN;
}

BoardType BoardTypeStruct::ParseBoardType(const std::string& str)
{
	if (str == "fsrio1" || str == "fsriov1")
		return BOARD_FSRIO_V1;
	if (str == "fsrio2" || str == "fsriov2")
		return BOARD_FSRIO_V2;
	if (str == "fsrio3" || str == "fsriov3")
		return BOARD_FSRIO_V3;

	if (str == "fsrminipad") { return BOARD_FSRMINIPAD; }
	if (str == "minipadv4") { return BOARD_FSRMINIPAD_V4; }
	if (str == "teensy2") { return BOARD_TEENSY2; }
	if (str == "leonardo") { return BOARD_LEONARDO; }
	
	return BOARD_UNKNOWN;
}

bool BoardTypeStruct::CompatibleWith(BoardTypeStruct other, bool strict) const
{
	ArchType compArchType = other.archType;
	if(compArchType == ARCH_AVR_ARD)
		compArchType = ARCH_AVR;

	if (archType != compArchType)
		return false;

	if(strict)
	{
		if (boardType == BOARD_UNKNOWN || other.boardType == BOARD_UNKNOWN)
			return false;
	
		if(boardType != other.boardType)
			return false;
	}

	return true;
}

std::string BoardTypeStruct::ToString() const
{
	std::string ret = "";

	switch (archType)
	{
		case ARCH_AVR:
		case ARCH_AVR_ARD:
			ret += "avr_";
			break;
		case ARCH_ESP:
			ret += "esp_";
			break;
		default:
			break;
	}

	switch (boardType)
	{
		case BOARD_FSRIO_V1:
			ret += "fsrio1";
			break;
		case BOARD_FSRIO_V2:
			ret += "fsriov2";
			break;
		case BOARD_FSRIO_V3:
			ret += "fsriov3";
			break;
		case BOARD_FSRMINIPAD:
			ret += "fsrminipad";
			break;
		case BOARD_FSRMINIPAD_V4:
			ret += "minipadv4";
			break;
		default:
			ret += "unknown";
	}

	return ret;
}

}