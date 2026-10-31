#pragma once

// The real include/Checksum.h, seen at the language level the firmware toolchain reports. The header refuses a
// compiler that reports full C++20, which the host g++ does.
#pragma push_macro("__cplusplus")
#undef __cplusplus
#define __cplusplus 201'709L
#include "../../../include/Checksum.h"
#pragma pop_macro("__cplusplus")
