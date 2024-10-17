#pragma once
#include <vector>
#include <tuple>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <libusb-1.0/libusb.h>
#include <mutex>

#include "usb_device.hpp"

class USBTransport {
public:
    USBTransport(bool usbDebug) : m_usbDebug{usbDebug}, m_ctx{nullptr}, m_running{false}
    {}
    virtual ~USBTransport();

    virtual int Init(uint16_t vendorId, uint16_t productId, std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback);
    virtual void Shutdown();

    void StartDeviceMonitor();

protected:
    bool m_usbDebug;
    libusb_context *m_ctx;
    libusb_hotplug_callback_handle m_callbackHandle;
    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAddedCallback;
    std::thread m_deviceMonitorThread;
    std::atomic<bool> m_running;
    std::mutex m_shutdownMutex;
    uint16_t m_vendorId;
    uint16_t m_productId;

    void DeviceMonitorThread();

    static int LIBUSB_CALL HotplugEventCallback(libusb_context *ctx, libusb_device *device,
                                                libusb_hotplug_event event, void *user_data);
};