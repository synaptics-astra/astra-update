// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "posix_usb_cdc_transport.hpp"

#include <cstring>
#include <filesystem>
#include <optional>

#include "astra_log.hpp"
#include "posix_usb_cdc_device.hpp"

#if defined(PLATFORM_LINUX)
#include <fstream>
#elif defined(PLATFORM_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#endif

namespace {
struct DeviceIdentityInfo {
    std::optional<uint16_t> vendorId;
    std::optional<uint16_t> productId;
    std::optional<uint32_t> numInterfaces;
    std::optional<uint32_t> interfaceNumber;
};

#if defined(PLATFORM_LINUX)
std::optional<uint16_t> ReadHexValueFromCandidates(const std::vector<std::filesystem::path> &candidates)
{
    for (const auto &candidate : candidates) {
        std::ifstream file(candidate);
        if (!file.is_open()) {
            continue;
        }

        std::string value;
        file >> value;
        if (value.empty()) {
            continue;
        }

        if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
            value = value.substr(2);
        }

        try {
            return static_cast<uint16_t>(std::stoul(value, nullptr, 16));
        } catch (...) {
            continue;
        }
    }

    return std::nullopt;
}

std::optional<DeviceIdentityInfo> ReadLinuxIdentity(const std::string &portPath)
{
    const std::filesystem::path ttyPath(portPath);
    const std::string ttyName = ttyPath.filename().string();
    if (ttyName.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path ttyBase = std::filesystem::path("/sys/class/tty") / ttyName / "device";
    const std::vector<std::filesystem::path> vendorCandidates = {
        ttyBase / "idVendor",
        ttyBase / ".." / "idVendor",
        ttyBase / ".." / ".." / "idVendor",
    };
    const std::vector<std::filesystem::path> productCandidates = {
        ttyBase / "idProduct",
        ttyBase / ".." / "idProduct",
        ttyBase / ".." / ".." / "idProduct",
    };
    const std::vector<std::filesystem::path> numInterfacesCandidates = {
        ttyBase / "bNumInterfaces",
        ttyBase / ".." / "bNumInterfaces",
        ttyBase / ".." / ".." / "bNumInterfaces",
    };
    const std::vector<std::filesystem::path> interfaceNumberCandidates = {
        ttyBase / "bInterfaceNumber",
        ttyBase / ".." / "bInterfaceNumber",
    };

    DeviceIdentityInfo info;
    info.vendorId = ReadHexValueFromCandidates(vendorCandidates);
    info.productId = ReadHexValueFromCandidates(productCandidates);

    if (const auto numIf = ReadHexValueFromCandidates(numInterfacesCandidates); numIf.has_value()) {
        info.numInterfaces = numIf.value();
    }

    if (const auto iface = ReadHexValueFromCandidates(interfaceNumberCandidates); iface.has_value()) {
        info.interfaceNumber = iface.value();
    }

    return info;
}
#elif defined(PLATFORM_MACOS)
bool CFStringEqualsStdString(CFTypeRef cfValue, const std::string &expected)
{
    if (cfValue == nullptr || CFGetTypeID(cfValue) != CFStringGetTypeID()) {
        return false;
    }

    const CFStringRef cfString = static_cast<CFStringRef>(cfValue);
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfString),
        kCFStringEncodingUTF8) + 1;
    if (maxBytes <= 0) {
        return false;
    }

    std::string converted(static_cast<size_t>(maxBytes), '\0');
    if (!CFStringGetCString(cfString, converted.data(), maxBytes, kCFStringEncodingUTF8)) {
        return false;
    }

    converted.resize(std::strlen(converted.c_str()));
    return converted == expected;
}

bool ReadRegistryUInt32(io_registry_entry_t entry, const char *propertyName, uint32_t &value)
{
    if (entry == IO_OBJECT_NULL || propertyName == nullptr) {
        return false;
    }

    const CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, propertyName,
        kCFStringEncodingUTF8);
    if (key == nullptr) {
        return false;
    }

    CFTypeRef property = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    CFRelease(key);
    if (property == nullptr) {
        return false;
    }

    bool parsed = false;
    if (CFGetTypeID(property) == CFNumberGetTypeID()) {
        int64_t number = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &number) && number >= 0) {
            value = static_cast<uint32_t>(number);
            parsed = true;
        }
    } else if (CFGetTypeID(property) == CFStringGetTypeID()) {
        const CFStringRef cfString = static_cast<CFStringRef>(property);
        const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfString),
            kCFStringEncodingUTF8) + 1;
        if (maxBytes > 0) {
            std::string text(static_cast<size_t>(maxBytes), '\0');
            if (CFStringGetCString(cfString, text.data(), maxBytes, kCFStringEncodingUTF8)) {
                text.resize(std::strlen(text.c_str()));
                try {
                    value = static_cast<uint32_t>(std::stoul(text, nullptr, 0));
                    parsed = true;
                } catch (...) {
                    try {
                        value = static_cast<uint32_t>(std::stoul(text, nullptr, 16));
                        parsed = true;
                    } catch (...) {
                    }
                }
            }
        }
    }

    CFRelease(property);
    return parsed;
}

std::optional<DeviceIdentityInfo> ReadMacIdentity(const std::string &portPath)
{
    CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
    if (matching == nullptr) {
        return std::nullopt;
    }

    CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iterator) != KERN_SUCCESS) {
        return std::nullopt;
    }

    std::optional<DeviceIdentityInfo> identity;

    io_object_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef dialin = IORegistryEntryCreateCFProperty(service, CFSTR(kIODialinDeviceKey),
            kCFAllocatorDefault, 0);
        CFTypeRef callout = IORegistryEntryCreateCFProperty(service, CFSTR(kIOCalloutDeviceKey),
            kCFAllocatorDefault, 0);

        const bool matchesPort = CFStringEqualsStdString(dialin, portPath) ||
            CFStringEqualsStdString(callout, portPath);

        if (dialin != nullptr) {
            CFRelease(dialin);
        }
        if (callout != nullptr) {
            CFRelease(callout);
        }

        if (!matchesPort) {
            IOObjectRelease(service);
            continue;
        }

        DeviceIdentityInfo info;
        uint32_t value = 0;

        if (ReadRegistryUInt32(service, "idVendor", value)) {
            info.vendorId = static_cast<uint16_t>(value);
        }

        if (ReadRegistryUInt32(service, "idProduct", value)) {
            info.productId = static_cast<uint16_t>(value);
        }

        if (ReadRegistryUInt32(service, "bNumInterfaces", value)) {
            info.numInterfaces = value;
        }

        if (ReadRegistryUInt32(service, "bInterfaceNumber", value)) {
            info.interfaceNumber = value;
        }

        identity = info;
        IOObjectRelease(service);
        break;
    }

    IOObjectRelease(iterator);
    return identity;
}
#endif

std::optional<DeviceIdentityInfo> ReadPosixIdentity(const std::string &portPath)
{
#if defined(PLATFORM_LINUX)
    return ReadLinuxIdentity(portPath);
#elif defined(PLATFORM_MACOS)
    return ReadMacIdentity(portPath);
#else
    (void)portPath;
    return std::nullopt;
#endif
}
} // namespace

void PosixUSBCDCTransport::ProcessPendingDevices()
{
    ASTRA_LOG;

    const auto candidatePorts = EnumerateCandidatePorts();
    for (const auto &candidatePort : candidatePorts) {
        const std::string normalizedPort = NormalizePortPath(candidatePort);
        if (!IsValidPort(normalizedPort)) {
            continue;
        }

        if (!MatchesVendorProduct(normalizedPort)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
            if (m_activeDevices.find(normalizedPort) != m_activeDevices.end()) {
                continue;
            }
            m_activeDevices.insert(normalizedPort);
        }

        // Extract vendor/product IDs and interface count from device identity
        uint16_t vendorId = 0;
        uint16_t productId = 0;
        uint8_t numInterfaces = 0;
        const auto identity = ReadPosixIdentity(normalizedPort);
        if (identity.has_value()) {
            if (identity->vendorId.has_value()) {
                vendorId = identity->vendorId.value();
            }
            if (identity->productId.has_value()) {
                productId = identity->productId.value();
            }
            if (identity->numInterfaces.has_value()) {
                const uint32_t n = identity->numInterfaces.value();
                numInterfaces = (n > 255U) ? uint8_t{255} : static_cast<uint8_t>(n);
            }
        }

        std::unique_ptr<USBDevice> usbDevice = std::make_unique<PosixUSBCDCDevice>(normalizedPort,
            vendorId, productId, numInterfaces);
        if (m_deviceAddedCallback) {
            try {
                m_deviceAddedCallback(std::move(usbDevice));
            } catch (...) {
                RemoveActiveDevice(normalizedPort);
            }
        } else {
            RemoveActiveDevice(normalizedPort);
        }
    }
}

std::vector<std::string> PosixUSBCDCTransport::EnumerateCandidatePorts() const
{
    ASTRA_LOG;

    std::vector<std::string> ports;

    const std::filesystem::path devPath("/dev");
    if (!std::filesystem::exists(devPath)) {
        return ports;
    }

    try {
        for (const auto &entry : std::filesystem::directory_iterator(devPath)) {
            const std::string name = entry.path().filename().string();
            const bool isLinuxCdc = (name.rfind("ttyACM", 0) == 0 || name.rfind("ttyUSB", 0) == 0);
            const bool isMacosCdc = (name.rfind("cu.usbmodem", 0) == 0 || name.rfind("cu.usbserial", 0) == 0 ||
                name.rfind("tty.usbmodem", 0) == 0 || name.rfind("tty.usbserial", 0) == 0);

            if (isLinuxCdc || isMacosCdc) {
                ports.push_back(entry.path().string());
            }
        }
    } catch (...) {
        log(ASTRA_LOG_LEVEL_WARNING) << "CDC candidate enumeration failed" << endLog;
    }

    return ports;
}

bool PosixUSBCDCTransport::MatchesVendorProduct(const std::string& portPath) const
{
    ASTRA_LOG;

    if (m_supportedDevices.empty()) {
        return true;
    }

    const auto identity = ReadPosixIdentity(portPath);
    if (!identity.has_value() || !identity->vendorId.has_value() || !identity->productId.has_value()) {
        // If platform metadata is unavailable, do not block detection.
        return true;
    }

    const uint16_t vid = identity->vendorId.value();
    const uint16_t pid = identity->productId.value();
    for (const auto& [v, p] : m_supportedDevices) {
        if (vid == v && pid == p) return true;
    }
    return false;
}

