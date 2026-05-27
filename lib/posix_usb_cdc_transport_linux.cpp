// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated
//
// Linux-specific implementation of PosixUSBCDCTransport.
// Compiled only on PLATFORM_LINUX builds.

#include "posix_usb_cdc_transport.hpp"
#include "posix_usb_cdc_transport_impl.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

#include <libudev.h>
#include <sys/select.h>
#include <unistd.h>

#include "astra_log.hpp"
#include "posix_usb_cdc_device.hpp"

// ---------------------------------------------------------------------------
// Platform-specific Impl struct
// ---------------------------------------------------------------------------

struct PosixUSBCDCTransport::Impl {
    int m_wakeupPipe[2] = {-1, -1};
    std::thread m_udevThread;

    ~Impl()
    {
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
    }
};

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

PosixUSBCDCTransport::PosixUSBCDCTransport(bool usbDebug)
    : USBCDCTransport(usbDebug), m_impl(std::make_unique<Impl>())
{}

PosixUSBCDCTransport::~PosixUSBCDCTransport()
{
    ASTRA_LOG;
    // Call Shutdown() while the vtable is still valid for the derived class so
    // that m_running becomes false before we signal the udev thread.  The base
    // class destructor also calls Shutdown(), but the exchange(false) guard
    // makes that second call a no-op.
    USBCDCTransport::Shutdown();
    // m_impl->~Impl() signals the wakeup pipe and joins the udev thread.
}

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers (Linux sysfs readers)
// ---------------------------------------------------------------------------

namespace {

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

} // namespace

// ---------------------------------------------------------------------------
// ReadPosixIdentity — platform entry point (called from shared .cpp)
// ---------------------------------------------------------------------------

std::optional<DeviceIdentityInfo> ReadPosixIdentity(const std::string &portPath)
{
    return ReadLinuxIdentity(portPath);
}

// ---------------------------------------------------------------------------
// StartDeviceMonitor
// ---------------------------------------------------------------------------

void PosixUSBCDCTransport::StartDeviceMonitor()
{
    ASTRA_LOG;

    if (pipe(m_impl->m_wakeupPipe) != 0) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Failed to create wakeup pipe, falling back to polling monitor" << endLog;
        USBCDCTransport::StartDeviceMonitor();
        return;
    }

    m_impl->m_udevThread = std::thread(&PosixUSBCDCTransport::UdevMonitorThread, this);
}

// ---------------------------------------------------------------------------
// ProcessUdevDevice
// ---------------------------------------------------------------------------

void PosixUSBCDCTransport::ProcessUdevDevice(udev_device *dev)
{
    ASTRA_LOG;

    const char *devnode = udev_device_get_devnode(dev);
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

    udev_device *usbdev = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
    if (usbdev) {
        auto parseHex = [](const char *s) -> std::optional<uint16_t> {
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
        const char *devClassStr    = udev_device_get_sysattr_value(usbdev, "bDeviceClass");
        const char *devSubClassStr = udev_device_get_sysattr_value(usbdev, "bDeviceSubClass");
        const char *bNumIfStr      = udev_device_get_sysattr_value(usbdev, "bNumInterfaces");

        uint8_t devClass    = 0xFF;
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
            << ": bDeviceClass=0x"    << std::hex << static_cast<int>(devClass)
            << " bDeviceSubClass=0x"  << static_cast<int>(devSubClass)
            << " bNumInterfaces="     << (bNumIfStr ? bNumIfStr : "?") << std::dec << endLog;

        // Composite markers: 0x00 (class from interfaces) and
        // 0xEF/0x02 (Misc/Multi-Function, used with IAD per USB ECN).
        const bool isComposite = (devClass == 0x00) || (devClass == 0xEF && devSubClass == 0x02);
        numInterfaces = isComposite ? uint8_t{2} : uint8_t{1};
    }

    // If VID/PID are known, filter against the supported device list.
    // If they are unavailable, pass through rather than blocking the device.
    if (vendorId != 0 && !m_supportedDevices.empty()) {
        bool matches = false;
        for (const auto &[v, p] : m_supportedDevices) {
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

// ---------------------------------------------------------------------------
// UdevMonitorThread
// ---------------------------------------------------------------------------

void PosixUSBCDCTransport::UdevMonitorThread()
{
    ASTRA_LOG;

    udev *udev_ctx = udev_new();
    if (!udev_ctx) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create udev context" << endLog;
        return;
    }

    // Set up the monitor before the initial enumerate so that no events are
    // missed in the window between enumeration and the start of the event loop.
    udev_monitor *monitor = udev_monitor_new_from_netlink(udev_ctx, "udev");
    if (!monitor) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create udev monitor" << endLog;
        udev_unref(udev_ctx);
        return;
    }
    udev_monitor_filter_add_match_subsystem_devtype(monitor, "tty", nullptr);
    udev_monitor_enable_receiving(monitor);
    const int udevFd = udev_monitor_get_fd(monitor);

    // Initial enumeration: handle devices that were already present at startup.
    udev_enumerate *enumerate = udev_enumerate_new(udev_ctx);
    udev_enumerate_add_match_subsystem(enumerate, "tty");
    udev_enumerate_scan_devices(enumerate);
    udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry *entry;
    udev_list_entry_foreach(entry, devices) {
        const char *syspath = udev_list_entry_get_name(entry);
        udev_device *dev = udev_device_new_from_syspath(udev_ctx, syspath);
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
        FD_SET(m_impl->m_wakeupPipe[0], &fds);
        const int maxFd = std::max(udevFd, m_impl->m_wakeupPipe[0]);

        struct timeval tv = {1, 0};
        const int ret = select(maxFd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log(ASTRA_LOG_LEVEL_ERROR) << "udev monitor select failed: " << errno << endLog;
            break;
        }

        if (!m_running.load() || FD_ISSET(m_impl->m_wakeupPipe[0], &fds)) break;

        if (FD_ISSET(udevFd, &fds)) {
            udev_device *dev = udev_monitor_receive_device(monitor);
            if (dev) {
                const char *action = udev_device_get_action(dev);
                if (action) {
                    if (strcmp(action, "add") == 0) {
                        ProcessUdevDevice(dev);
                    } else if (strcmp(action, "remove") == 0) {
                        const char *devnode = udev_device_get_devnode(dev);
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
