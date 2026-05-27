// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated
//
// macOS-specific implementation of PosixUSBCDCTransport.
// Compiled only on PLATFORM_MACOS builds.

#include "posix_usb_cdc_transport.hpp"
#include "posix_usb_cdc_transport_impl.hpp"

#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#include "astra_log.hpp"
#include "posix_usb_cdc_device.hpp"

// ---------------------------------------------------------------------------
// Platform-specific Impl struct
// ---------------------------------------------------------------------------

struct PosixUSBCDCTransport::Impl {
    std::thread m_ioKitThread;
    CFRunLoopRef m_runLoop{nullptr};
    std::mutex m_ioKitMutex;
    std::condition_variable m_ioKitCV;
    bool m_ioKitReady{false};

    ~Impl()
    {
        {
            std::unique_lock<std::mutex> lk(m_ioKitMutex);
            m_ioKitCV.wait(lk, [this] { return m_ioKitReady || !m_ioKitThread.joinable(); });
            if (m_runLoop) {
                CFRunLoopStop(m_runLoop);
            }
        }
        if (m_ioKitThread.joinable()) {
            m_ioKitThread.join();
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
    // that m_running becomes false before we stop the IOKit run loop.  The base
    // class destructor also calls Shutdown(), but the exchange(false) guard
    // makes that second call a no-op.
    USBCDCTransport::Shutdown();
    // m_impl->~Impl() waits for IOKit ready, stops the run loop, and joins the thread.
}

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers (IOKit / CoreFoundation utilities)
// ---------------------------------------------------------------------------

namespace {

std::string CFStringToStdString(CFStringRef cfStr)
{
    if (!cfStr) {
        return {};
    }
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(
        CFStringGetLength(cfStr), kCFStringEncodingUTF8) + 1;
    if (maxBytes <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(maxBytes), '\0');
    if (!CFStringGetCString(cfStr, result.data(), maxBytes, kCFStringEncodingUTF8)) {
        return {};
    }
    result.resize(std::strlen(result.c_str()));
    return result;
}

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
        CFTypeRef dialin  = IORegistryEntryCreateCFProperty(service, CFSTR(kIODialinDeviceKey),
            kCFAllocatorDefault, 0);
        CFTypeRef callout = IORegistryEntryCreateCFProperty(service, CFSTR(kIOCalloutDeviceKey),
            kCFAllocatorDefault, 0);

        const bool matchesPort = CFStringEqualsStdString(dialin, portPath) ||
            CFStringEqualsStdString(callout, portPath);

        if (dialin  != nullptr) CFRelease(dialin);
        if (callout != nullptr) CFRelease(callout);

        if (!matchesPort) {
            IOObjectRelease(service);
            continue;
        }

        DeviceIdentityInfo info;
        uint32_t value = 0;

        if (ReadRegistryUInt32(service, "idVendor",  value)) info.vendorId  = static_cast<uint16_t>(value);
        if (ReadRegistryUInt32(service, "idProduct", value)) info.productId = static_cast<uint16_t>(value);

        // bNumInterfaces is a configuration descriptor field not exposed by
        // IOKit as a named registry property.  Derive it from bDeviceClass /
        // bDeviceSubClass using the same composite heuristic as the Linux path.
        uint32_t devClass    = 0xFF;
        uint32_t devSubClass = 0xFF;
        ReadRegistryUInt32(service, "bDeviceClass",    devClass);
        ReadRegistryUInt32(service, "bDeviceSubClass", devSubClass);
        const bool isComposite = (devClass == 0x00) || (devClass == 0xEF && devSubClass == 0x02);
        info.numInterfaces = isComposite ? uint32_t{2} : uint32_t{1};

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

} // namespace

// ---------------------------------------------------------------------------
// ReadPosixIdentity — platform entry point (called from shared .cpp)
// ---------------------------------------------------------------------------

std::optional<DeviceIdentityInfo> ReadPosixIdentity(const std::string &portPath)
{
    return ReadMacIdentity(portPath);
}

// ---------------------------------------------------------------------------
// StartDeviceMonitor
// ---------------------------------------------------------------------------

void PosixUSBCDCTransport::StartDeviceMonitor()
{
    ASTRA_LOG;
    m_impl->m_ioKitThread = std::thread(&PosixUSBCDCTransport::IOKitMonitorThread, this);
}

// ---------------------------------------------------------------------------
// IOKit static callbacks
// ---------------------------------------------------------------------------

// static
void PosixUSBCDCTransport::IOKitDeviceAdded(void *ctx, io_iterator_t iter)
{
    auto *self = static_cast<PosixUSBCDCTransport *>(ctx);
    io_object_t service;
    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        self->ProcessIOKitService(service);
        IOObjectRelease(service);
    }
}

// static
void PosixUSBCDCTransport::IOKitDeviceRemoved(void *ctx, io_iterator_t iter)
{
    auto *self = static_cast<PosixUSBCDCTransport *>(ctx);
    io_object_t service;
    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        CFTypeRef calloutRef = IORegistryEntryCreateCFProperty(service,
            CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (calloutRef) {
            if (CFGetTypeID(calloutRef) == CFStringGetTypeID()) {
                const std::string path = CFStringToStdString(static_cast<CFStringRef>(calloutRef));
                if (!path.empty()) {
                    self->RemoveActiveDevice(path);
                }
            }
            CFRelease(calloutRef);
        }
        IOObjectRelease(service);
    }
}

// ---------------------------------------------------------------------------
// ProcessIOKitService
// ---------------------------------------------------------------------------

void PosixUSBCDCTransport::ProcessIOKitService(io_object_t service)
{
    ASTRA_LOG;

    // Only process cu.* (call-out) devices.
    CFTypeRef calloutRef = IORegistryEntryCreateCFProperty(service,
        CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
    if (!calloutRef) {
        return;
    }

    std::string portPath;
    if (CFGetTypeID(calloutRef) == CFStringGetTypeID()) {
        portPath = CFStringToStdString(static_cast<CFStringRef>(calloutRef));
    }
    CFRelease(calloutRef);

    if (portPath.empty()) {
        return;
    }

    const std::string name = std::filesystem::path(portPath).filename().string();
    if (name.rfind("cu.usbmodem", 0) != 0 && name.rfind("cu.usbserial", 0) != 0) {
        return;
    }

    if (!IsValidPort(portPath)) {
        return;
    }

    // Read VID/PID/numInterfaces directly from the IOKit service.
    uint16_t vendorId  = 0;
    uint16_t productId = 0;
    uint32_t value     = 0;

    if (ReadRegistryUInt32(service, "idVendor",  value)) vendorId  = static_cast<uint16_t>(value);
    if (ReadRegistryUInt32(service, "idProduct", value)) productId = static_cast<uint16_t>(value);

    // bNumInterfaces is a configuration descriptor field that IOKit does not
    // expose as a registry property.  Infer composite vs single-interface from
    // bDeviceClass / bDeviceSubClass, mirroring the Linux udev heuristic:
    //   0x00            → class defined per-interface (IAD composite)
    //   0xEF / 0x02     → Misc / Multi-Function (IAD composite, USB ECN)
    //   anything else   → single-function CDC device (e.g. m52bl, 0x02)
    uint32_t devClass    = 0xFF;
    uint32_t devSubClass = 0xFF;
    ReadRegistryUInt32(service, "bDeviceClass",    devClass);
    ReadRegistryUInt32(service, "bDeviceSubClass", devSubClass);
    const bool isComposite = (devClass == 0x00) || (devClass == 0xEF && devSubClass == 0x02);
    const uint8_t numInterfaces = isComposite ? uint8_t{2} : uint8_t{1};

    log(ASTRA_LOG_LEVEL_DEBUG) << "ProcessIOKitService: " << portPath
        << " bDeviceClass=0x"    << std::hex << devClass
        << " bDeviceSubClass=0x" << devSubClass
        << " isComposite="       << isComposite
        << " numInterfaces="     << std::dec << static_cast<int>(numInterfaces) << endLog;

    // Filter against the supported device list when VID/PID are known.
    if (vendorId != 0 && !m_supportedDevices.empty()) {
        bool matches = false;
        for (const auto &[v, p] : m_supportedDevices) {
            if (vendorId == v && productId == p) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
        if (m_activeDevices.find(portPath) != m_activeDevices.end()) {
            return;
        }
        m_activeDevices.insert(portPath);
    }

    auto device = std::make_unique<PosixUSBCDCDevice>(portPath, vendorId, productId, numInterfaces);
    if (m_deviceAddedCallback) {
        try {
            m_deviceAddedCallback(std::move(device));
        } catch (...) {
            RemoveActiveDevice(portPath);
        }
    } else {
        RemoveActiveDevice(portPath);
    }
}

// ---------------------------------------------------------------------------
// IOKitMonitorThread
// ---------------------------------------------------------------------------

void PosixUSBCDCTransport::IOKitMonitorThread()
{
    ASTRA_LOG;

    IONotificationPortRef notifyPort = IONotificationPortCreate(kIOMasterPortDefault);
    if (!notifyPort) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create IOKit notification port" << endLog;
        {
            std::lock_guard<std::mutex> lk(m_impl->m_ioKitMutex);
            m_impl->m_ioKitReady = true;
        }
        m_impl->m_ioKitCV.notify_one();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_impl->m_ioKitMutex);
        m_impl->m_runLoop   = CFRunLoopGetCurrent();
        m_impl->m_ioKitReady = true;
    }
    m_impl->m_ioKitCV.notify_one();

    CFRunLoopSourceRef source = IONotificationPortGetRunLoopSource(notifyPort);
    CFRunLoopAddSource(m_impl->m_runLoop, source, kCFRunLoopDefaultMode);

    // Register for device arrivals.  Draining the iterator on registration
    // picks up devices already present at startup and arms future notifications.
    io_iterator_t addedIter = IO_OBJECT_NULL;
    {
        CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
        if (matching) {
            CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));
            const kern_return_t kr = IOServiceAddMatchingNotification(notifyPort,
                kIOFirstMatchNotification, matching, IOKitDeviceAdded, this, &addedIter);
            if (kr == KERN_SUCCESS) {
                IOKitDeviceAdded(this, addedIter);
            } else {
                log(ASTRA_LOG_LEVEL_WARNING) << "IOServiceAddMatchingNotification (arrived) failed: "
                    << kr << endLog;
            }
        }
    }

    // Register for device removals.
    io_iterator_t removedIter = IO_OBJECT_NULL;
    {
        CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
        if (matching) {
            CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));
            const kern_return_t kr = IOServiceAddMatchingNotification(notifyPort,
                kIOTerminatedNotification, matching, IOKitDeviceRemoved, this, &removedIter);
            if (kr == KERN_SUCCESS) {
                IOKitDeviceRemoved(this, removedIter);
            } else {
                log(ASTRA_LOG_LEVEL_WARNING) << "IOServiceAddMatchingNotification (terminated) failed: "
                    << kr << endLog;
            }
        }
    }

    // Block until CFRunLoopStop() is called from Impl::~Impl().
    CFRunLoopRun();

    if (addedIter   != IO_OBJECT_NULL) IOObjectRelease(addedIter);
    if (removedIter != IO_OBJECT_NULL) IOObjectRelease(removedIter);
    IONotificationPortDestroy(notifyPort);
}
