// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "astra_device_manager.hpp"
#include "astra_device_manager_internal.hpp"
#include "boot_image_fixtures.hpp"
#include "scripted_usb_device.hpp"
#include "test_support.hpp"

namespace {

constexpr char imageRequestMagic[] = "i*m*g*r*q*";

std::vector<uint8_t> Request(const std::string &name, uint8_t type = 0x01)
{
    std::vector<uint8_t> packet(imageRequestMagic, imageRequestMagic + sizeof(imageRequestMagic) - 1);
    packet.push_back(type);
    packet.insert(packet.end(), name.begin(), name.end());
    packet.push_back(0);
    return packet;
}

// Hands devices to the manager the way a real transport's hotplug thread would.
class FakeTransport final : public USBTransport
{
public:
    FakeTransport() : USBTransport(false) {}

    int Init(std::vector<USBVendorProductId>, const std::string,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceAdded = std::move(deviceAddedCallback);
        return 0;
    }

    void Shutdown() override {}

    void Arrive(std::unique_ptr<USBDevice> device)
    {
        std::function<void(std::unique_ptr<USBDevice>)> deviceAdded;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            deviceAdded = m_deviceAdded;
        }
        REQUIRE(deviceAdded != nullptr);
        deviceAdded(std::move(device));
    }

private:
    std::mutex m_mutex;
    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAdded;
};

class ResponseCollector
{
public:
    std::function<void(AstraDeviceManagerResponse)> Callback()
    {
        return [this](AstraDeviceManagerResponse response) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_responses.push_back(response);
            }
            m_changed.notify_all();
        };
    }

    bool WaitForDeviceStatus(AstraDeviceStatus status)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(5),
            [&] { return CountDeviceStatusLocked(status) > 0; });
    }

    bool WaitForManagerStatus(AstraDeviceManagerStatus status)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return std::any_of(m_responses.begin(), m_responses.end(),
                [status](const AstraDeviceManagerResponse &response) {
                    return response.IsDeviceManagerResponse()
                        && response.GetDeviceManagerResponse().m_managerStatus == status;
                });
        });
    }

    size_t CountDeviceStatus(AstraDeviceStatus status)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return CountDeviceStatusLocked(status);
    }

private:
    size_t CountDeviceStatusLocked(AstraDeviceStatus status) const
    {
        return static_cast<size_t>(std::count_if(m_responses.begin(), m_responses.end(),
            [status](const AstraDeviceManagerResponse &response) {
                return response.IsDeviceResponse() && response.GetDeviceResponse().m_status == status;
            }));
    }

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<AstraDeviceManagerResponse> m_responses;
};

// Installs a FakeTransport for one test and restores the default afterwards.
class ManagerFixture
{
public:
    ManagerFixture()
    {
        SetUSBTransportFactoryForTesting([this](AstraTransportType, bool) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_transport = std::make_shared<FakeTransport>();
            return m_transport;
        });

        auto fields = test::BootManifest("sl1680-boot");
        fields["chip"] = "sl1680";
        fields["secure_boot"] = "gen2";
        test::WriteBootManifest(bootDirectory, fields);
        bootDirectory.Write("minildr.img", "miniloader");
    }

    ~ManagerFixture()
    {
        SetUSBTransportFactoryForTesting(nullptr);
        if (!managerTempDirectory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(managerTempDirectory, error);
        }
    }

    // Starts a boot-mode manager, delivers one scripted SL16xx device and
    // returns it once the device has opened it.
    test::ScriptedUSBDevice *StartBoot(AstraDeviceManager &manager,
        std::function<void(test::ScriptedUSBDevice &)> prepare = nullptr)
    {
        manager.Boot(bootDirectory.Path().string());
        managerTempDirectory = std::filesystem::path(manager.GetLogFile()).parent_path();

        auto usb = std::make_unique<test::ScriptedUSBDevice>("fake-usb-0");
        auto *usbProbe = usb.get();
        if (prepare) {
            prepare(*usbProbe);
        }
        std::shared_ptr<FakeTransport> transport;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            transport = m_transport;
        }
        REQUIRE(transport != nullptr);
        transport->Arrive(std::move(usb));
        for (int i = 0; i < 250 && usbProbe->OpenCount() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(usbProbe->OpenCount() == 1);
        return usbProbe;
    }

    test::TempDirectory bootDirectory;
    std::filesystem::path managerTempDirectory;

private:
    std::mutex m_mutex;
    std::shared_ptr<FakeTransport> m_transport;
};

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("successful-boot-reports-no-failure-and-removes-temp-directory", [] {
        ManagerFixture fixture;
        ResponseCollector responses;
        AstraDeviceManager manager(responses.Callback());
        auto *usb = fixture.StartBoot(manager);

        usb->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("minildr.img", 0x80));
        usb->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("07_IMAGE"));
        for (int i = 0; i < 250 && usb->Writes().size() < 4; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        usb->Emit(USBDevice::USB_DEVICE_EVENT_NO_DEVICE);
        REQUIRE(responses.WaitForManagerStatus(ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN));
        REQUIRE(responses.CountDeviceStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE) == 1);

        REQUIRE(manager.Shutdown() == false);
        REQUIRE(!std::filesystem::exists(fixture.managerTempDirectory));
    });
    suite.Add("device-reported-failure-sets-result-and-keeps-logs", [] {
        ManagerFixture fixture;
        ResponseCollector responses;
        AstraDeviceManager manager(responses.Callback());
        auto *usb = fixture.StartBoot(manager);

        // The device itself reports BOOT_FAIL for an image it cannot serve.
        usb->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("missing.img"));
        REQUIRE(responses.WaitForDeviceStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        REQUIRE(manager.Shutdown() == true);
        REQUIRE(responses.CountDeviceStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL) == 1);
        REQUIRE(std::filesystem::exists(fixture.managerTempDirectory / "astra_device_manager.log"));
    });
    suite.Add("failure-the-device-does-not-report-is-reported-by-manager", [] {
        ManagerFixture fixture;
        ResponseCollector responses;
        AstraDeviceManager manager(responses.Callback());
        // The miniloader goes out (size header + payload), then sending the
        // 07_IMAGE size reply fails. SL16xx suppresses status events for
        // 07_IMAGE, so the device ends in BOOT_FAIL without reporting it.
        auto *usb = fixture.StartBoot(manager, [](test::ScriptedUSBDevice &device) {
            device.QueueWriteResult({0, test::ScriptedUSBDevice::WriteResult{}.transferred});
            device.QueueWriteResult({0, test::ScriptedUSBDevice::WriteResult{}.transferred});
            device.QueueWriteResult({-1, 0});
        });

        usb->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("minildr.img", 0x80));
        usb->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("07_IMAGE"));
        REQUIRE(responses.WaitForDeviceStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        REQUIRE(manager.Shutdown() == true);
        REQUIRE(responses.CountDeviceStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL) == 1);
        REQUIRE(std::filesystem::exists(fixture.managerTempDirectory / "astra_device_manager.log"));
    });
    return suite.Run(argc, argv);
}
