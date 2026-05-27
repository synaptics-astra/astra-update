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
#include <libudev.h>
#include <sys/select.h>
#include <unistd.h>
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

PosixUSBCDCTransport::~PosixUSBCDCTransport()
{
    ASTRA_LOG;
#if defined(PLATFORM_LINUX)
    // Call the base class Shutdown() here (while the vtable is still valid for
    // the derived class) so that m_running becomes false before we signal the
    // udev thread.  The base class destructor calls Shutdown() too, but the
    // m_running.exchange(false) guard makes that second call a no-op.
    USBCDCTransport::Shutdown();

    if (m_wakeupPipe[1] >= 0) {
        const char c = 0;
        (void)write(m_wakeupPipe[1], &c, 1);
        close(m_wakeupPipe[1]);
        m_wakeupPipe[1] = -1;
    }
    if (m_udevThread.joinable()) {
        m_udevThread.join();
    }
    if (m_wakeupPipe[0] >= 0) {
        close(m_wakeupPipe[0]);
        m_wakeupPipe[0] = -1;
    }
#endif
}

#if defined(PLATFORM_LINUX)

void PosixUSBCDCTransport::StartDeviceMonitor()
{
    ASTRA_LOG;

    if (pipe(m_wakeupPipe) != 0) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Failed to create wakeup pipe, falling back to polling monitor" << endLog;
        USBCDCTransport::StartDeviceMonitor();
        return;
    }

    m_udevThread = std::thread(&PosixUSBCDCTransport::UdevMonitorThread, this);
}

void PosixUSBCDCTransport::ProcessUdevDevice(udev_device* dev)
{
    ASTRA_LOG;

    const char* devnode = udev_device_get_devnode(dev);
    if (!devnode) {
        return;
    }

    const std::string portPath = NormalizePortPath(std::string(devnode));
    const std::string name = std::filesystem::path(portPath).filename().string();
    if (name.rfind("ttyACM", 0) != 0 && name.rfind("ttyUSB", 0) != 0) {
        return;
    }

    if (!IsValidPort(portPath)) {
        return;
    }

    // Read VID/PID/numInterfaces directly from the USB device parent — these
    // are guaranteed to be present by the time udev dispatches the event.
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint8_t numInterfaces = 0;

    udev_device* usbdev = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
    if (usbdev) {
        auto parseHex = [](const char* s) -> std::optional<uint16_t> {
            if (!s) return std::nullopt;
            try { return static_cast<uint16_t>(std::stoul(s, nullptr, 16)); }
            catch (...) { return std::nullopt; }
        };

        if (auto v = parseHex(udev_device_get_sysattr_value(usbdev, "idVendor"))) vendorId = *v;
        if (auto p = parseHex(udev_device_get_sysattr_value(usbdev, "idProduct"))) productId = *p;

        // Mirror the Windows MI_ heuristic used in WinUSBCDCTransport:
        // a composite USB device (bDeviceClass=0x00 or 0xEF with IAD) presents
        // its interfaces individually, and on Windows each interface's Hardware
        // ID contains "MI_xx", which maps to numInterfaces=2 (bootrom mode).
        // A device with a specific class at device level (e.g., 0x02 for CDC)
        // has no MI_ on Windows, yielding numInterfaces=1 (m52bl mode).
        const char* devClassStr = udev_device_get_sysattr_value(usbdev, "bDeviceClass");
        const char* devSubClassStr = udev_device_get_sysattr_value(usbdev, "bDeviceSubClass");
        const char* bNumIfStr = udev_device_get_sysattr_value(usbdev, "bNumInterfaces");

        uint8_t devClass = 0xFF;
        uint8_t devSubClass = 0xFF;
        if (devClassStr) {
            try { devClass = static_cast<uint8_t>(std::stoul(devClassStr, nullptr, 16)); }
            catch (...) {}
        }
        if (devSubClassStr) {
            try { devSubClass = static_cast<uint8_t>(std::stoul(devSubClassStr, nullptr, 16)); }
            catch (...) {}
        }

        log(ASTRA_LOG_LEVEL_DEBUG) << "USB device attributes for " << portPath
            << ": bDeviceClass=0x" << std::hex << static_cast<int>(devClass)
            << " bDeviceSubClass=0x" << static_cast<int>(devSubClass)
            << " bNumInterfaces=" << (bNumIfStr ? bNumIfStr : "?") << std::dec << endLog;

        // Composite markers: 0x00 (class from interfaces) and
        // 0xEF/0x02 (Misc/Multi-Function, used with IAD per USB ECN).
        const bool isComposite = (devClass == 0x00) || (devClass == 0xEF && devSubClass == 0x02);
        numInterfaces = isComposite ? uint8_t{2} : uint8_t{1};
    }

    // If VID/PID are known, filter against the supported device list.
    // If they are unavailable, pass through rather than blocking the device.
    if (vendorId != 0 && !m_supportedDevices.empty()) {
        bool matches = false;
        for (const auto& [v, p] : m_supportedDevices) {
            if (vendorId == v && productId == p) { matches = true; break; }
        }
        if (!matches) return;
    }

    {
        std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
        if (m_activeDevices.find(portPath) != m_activeDevices.end()) return;
        m_activeDevices.insert(portPath);
    }

    auto usbDevice = std::make_unique<PosixUSBCDCDevice>(portPath, vendorId, productId, numInterfaces);
    if (m_deviceAddedCallback) {
        try {
            m_deviceAddedCallback(std::move(usbDevice));
        } catch (...) {
            RemoveActiveDevice(portPath);
        }
    } else {
        RemoveActiveDevice(portPath);
    }
}

void PosixUSBCDCTransport::UdevMonitorThread()
{
    ASTRA_LOG;

    udev* udev_ctx = udev_new();
    if (!udev_ctx) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create udev context" << endLog;
        return;
    }

    // Set up the monitor before the initial enumerate so that no events are
    // missed in the window between enumeration and the start of the event loop.
    udev_monitor* monitor = udev_monitor_new_from_netlink(udev_ctx, "udev");
    if (!monitor) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create udev monitor" << endLog;
        udev_unref(udev_ctx);
        return;
    }
    udev_monitor_filter_add_match_subsystem_devtype(monitor, "tty", nullptr);
    udev_monitor_enable_receiving(monitor);
    const int udevFd = udev_monitor_get_fd(monitor);

    // Initial enumeration: handle devices that were already present at startup.
    udev_enumerate* enumerate = udev_enumerate_new(udev_ctx);
    udev_enumerate_add_match_subsystem(enumerate, "tty");
    udev_enumerate_scan_devices(enumerate);
    udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry* entry;
    udev_list_entry_foreach(entry, devices) {
        const char* syspath = udev_list_entry_get_name(entry);
        udev_device* dev = udev_device_new_from_syspath(udev_ctx, syspath);
        if (dev) {
            ProcessUdevDevice(dev);
            udev_device_unref(dev);
        }
    }
    udev_enumerate_unref(enumerate);

    // Event loop: process device add/remove as they occur.
    while (m_running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(udevFd, &fds);
        FD_SET(m_wakeupPipe[0], &fds);
        const int maxFd = std::max(udevFd, m_wakeupPipe[0]);

        struct timeval tv = {1, 0};
        const int ret = select(maxFd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log(ASTRA_LOG_LEVEL_ERROR) << "udev monitor select failed: " << errno << endLog;
            break;
        }

        if (!m_running.load() || FD_ISSET(m_wakeupPipe[0], &fds)) break;

        if (FD_ISSET(udevFd, &fds)) {
            udev_device* dev = udev_monitor_receive_device(monitor);
            if (dev) {
                const char* action = udev_device_get_action(dev);
                if (action) {
                    if (strcmp(action, "add") == 0) {
                        ProcessUdevDevice(dev);
                    } else if (strcmp(action, "remove") == 0) {
                        const char* devnode = udev_device_get_devnode(dev);
                        if (devnode) RemoveActiveDevice(std::string(devnode));
                    }
                }
                udev_device_unref(dev);
            }
        }
    }

    udev_monitor_unref(monitor);
    udev_unref(udev_ctx);
}

#endif // PLATFORM_LINUX

// ProcessPendingDevices is used on macOS (and as a fallback on Linux if the
// udev pipe setup fails) via the polling-based DeviceMonitorThread.
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

