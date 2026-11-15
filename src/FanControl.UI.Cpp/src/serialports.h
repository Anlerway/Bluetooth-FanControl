#pragma once
#include <string>
#include <vector>

namespace fc {

// Serial ports present on this machine, read once from
// HKLM\HARDWARE\DEVICEMAP\SERIALCOMM and cached for the process lifetime.
const std::vector<std::wstring>& SerialPorts();

}  // namespace fc
