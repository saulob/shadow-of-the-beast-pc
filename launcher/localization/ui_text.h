#pragma once

#include <string>

namespace Sotb {
inline const char* Tr(const char* text) { return text; }
inline std::string Tr(const std::string& text) { return text; }
} // namespace Sotb

namespace sotb {
inline const char* Tr(const char* text) { return Sotb::Tr(text); }
inline std::string Tr(const std::string& text) { return Sotb::Tr(text); }
} // namespace sotb
