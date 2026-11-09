// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "libusb_transport.hpp"
#include "test_support.hpp"

namespace {

class ObservedUSBTransport : public LibUSBTransport
{
public:
    ObservedUSBTransport() : LibUSBTransport(false) {}

    using USBTransport::NotifyDeviceObserved;
    using LibUSBTransport::OpenDevice;
};

libusb_device_descriptor GadgetDescriptor()
{
    libusb_device_descriptor desc{};
    desc.idVendor = 0x1D6B;
    desc.idProduct = 0x0104;
    return desc;
}

}

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("no-observer-preserves-normal-device-handling", [] {
        ObservedUSBTransport transport;
        REQUIRE(!transport.NotifyDeviceObserved(0x18D1, 0x4EE0, "2-3.4.2"));
        REQUIRE(!transport.NotifyDeviceObserved(0x1D6B, 0x0104, "2-3.4.2"));
    });
    suite.Add("completion-observation-needs-no-openable-device", [] {
        ObservedUSBTransport transport;
        unsigned int arrivals = 0;
        transport.SetDeviceObservedCallback([&](uint16_t vid, uint16_t pid, const std::string &path) {
            REQUIRE(vid == 0x1D6B);
            REQUIRE(pid == 0x0104);
            REQUIRE(path == "2-3.4.2");
            ++arrivals;
            return true;
        });
        libusb_device_handle *handle = nullptr;
        // No libusb device exists: reaching libusb_open would be invalid.
        REQUIRE(transport.OpenDevice(nullptr, GadgetDescriptor(), "2-3.4.2", &handle) == LIBUSB_SUCCESS);
        REQUIRE(handle == nullptr);
        REQUIRE(arrivals == 1);
    });
    suite.Add("completion-observer-does-not-consume-fastboot-or-other-ids", [] {
        ObservedUSBTransport transport;
        transport.SetDeviceObservedCallback([](uint16_t vid, uint16_t pid, const std::string &) {
            return vid == 0x1D6B && pid == 0x0104;
        });
        REQUIRE(!transport.NotifyDeviceObserved(0x18D1, 0x4EE0, "2-3.4.2"));
        REQUIRE(!transport.NotifyDeviceObserved(0x1D6B, 0x0105, "2-3.4.2"));
        REQUIRE(!transport.NotifyDeviceObserved(0x1D6C, 0x0104, "2-3.4.2"));
        REQUIRE(transport.NotifyDeviceObserved(0x1D6B, 0x0104, "2-3.4.2"));
    });
    suite.Add("observations-preserve-port-identity-across-devices-and-reenumeration", [] {
        ObservedUSBTransport transport;
        std::vector<std::string> paths;
        transport.SetDeviceObservedCallback([&](uint16_t, uint16_t, const std::string &path) {
            paths.push_back(path);
            return true;
        });
        libusb_device_handle *handle = nullptr;
        for (const std::string path : {"2-3.4.2", "2-3.4.3", "2-3.4.2"}) {
            REQUIRE(transport.OpenDevice(nullptr, GadgetDescriptor(), path, &handle) == LIBUSB_SUCCESS);
            REQUIRE(handle == nullptr);
        }
        REQUIRE(paths == std::vector<std::string>({"2-3.4.2", "2-3.4.3", "2-3.4.2"}));
    });
    suite.Add("observer-can-be-cleared-before-init", [] {
        ObservedUSBTransport transport;
        transport.SetDeviceObservedCallback([](uint16_t, uint16_t, const std::string &) {
            return true;
        });
        REQUIRE(transport.NotifyDeviceObserved(0x1D6B, 0x0104, "2-3.4.2"));
        transport.SetDeviceObservedCallback(nullptr);
        REQUIRE(!transport.NotifyDeviceObserved(0x1D6B, 0x0104, "2-3.4.2"));
    });
    return suite.Run(argc, argv);
}
