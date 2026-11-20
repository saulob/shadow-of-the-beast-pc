// SPDX-License-Identifier: GPL-2.0-or-later
// A bounded, Vulkan-independent copy of the driver's VK_EXT_device_fault report and its text form.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Vulkan {

constexpr std::uint32_t MaxFaultAddresses = 8;
constexpr std::uint32_t MaxVendorFaults = 8;
constexpr std::size_t MaxFaultTextBytes = 256;

enum class FaultStatus { Unsupported, Failed, Captured };

struct FaultReport {
    struct Address {
        std::uint32_t type{};
        std::uint64_t address{};
        std::uint64_t precision{};
    };
    struct Vendor {
        std::uint64_t code{};
        std::uint64_t data{};
        std::string description;
    };

    FaultStatus status{FaultStatus::Unsupported};
    std::int32_t result{};
    std::uint32_t address_count{};
    std::uint32_t vendor_count{};
    std::string description;
    std::vector<Address> addresses;
    std::vector<Vendor> vendors;
};

// Driver text is not guaranteed to be terminated or printable.
inline std::string SanitizeDriverText(const char* text, std::size_t capacity) {
    std::string out;
    for (std::size_t i = 0; i < capacity && i < MaxFaultTextBytes && text[i] != '\0'; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        out.push_back(c >= 0x20 && c < 0x7f ? char(c) : '?');
    }
    return out;
}

inline std::uint32_t BoundedFaultCount(std::uint32_t reported, std::uint32_t limit) {
    return std::min(reported, limit);
}

inline std::vector<std::string> FormatFaultReport(const FaultReport& report) {
    char line[MaxFaultTextBytes + 96];
    std::vector<std::string> lines;
    switch (report.status) {
    case FaultStatus::Unsupported:
        lines.emplace_back("Device fault report unavailable (no VK_EXT_device_fault)");
        return lines;
    case FaultStatus::Failed:
        std::snprintf(line, sizeof(line), "Device fault report failed (result %d)", report.result);
        lines.emplace_back(line);
        return lines;
    case FaultStatus::Captured:
        break;
    }
    lines.push_back("Device fault: " + report.description);
    for (const auto& a : report.addresses) {
        std::snprintf(line, sizeof(line), "Fault address type %u at %#llx (precision %#llx)", a.type,
                      static_cast<unsigned long long>(a.address),
                      static_cast<unsigned long long>(a.precision));
        lines.emplace_back(line);
    }
    for (const auto& v : report.vendors) {
        std::snprintf(line, sizeof(line), "Vendor fault %#llx data %#llx: ",
                      static_cast<unsigned long long>(v.code),
                      static_cast<unsigned long long>(v.data));
        lines.push_back(line + v.description);
    }
    if (report.address_count > report.addresses.size() ||
        report.vendor_count > report.vendors.size()) {
        std::snprintf(line, sizeof(line), "Device fault: %u address and %u vendor entries, first %zu and %zu shown",
                      report.address_count, report.vendor_count, report.addresses.size(),
                      report.vendors.size());
        lines.emplace_back(line);
    }
    return lines;
}

} // namespace Vulkan
