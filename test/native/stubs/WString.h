#pragma once

// Host stand-in for the Arduino string type, so the host tests can use the real include/util/StringUtils.h.
#include <cctype>
#include <cstddef>

class String {
public:
  String(const char*, std::size_t) { }
};
