// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

#include "astra_device_impl_internal.hpp"
#include "boot_image_fixtures.hpp"
#include "scripted_usb_device.hpp"
#include "test_support.hpp"

namespace {

constexpr char imageRequestMagic[] = "i*m*g*r*q*";

class StatusCollector
{
public:
    void Install(AstraDeviceImpl &device)
    {
        device.SetStatusCallback([this](AstraDeviceManagerResponse response) {
            if (!response.IsDeviceResponse()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_responses.push_back(response.GetDeviceResponse());
            }
            m_changed.notify_all();
        });
    }

    bool WaitFor(AstraDeviceStatus status)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(2), [this, status] {
            return std::any_of(m_responses.begin(), m_responses.end(),
                [status](const DeviceResponse &response) { return response.m_status == status; });
        });
    }

    bool WaitForImage(const std::string &name, AstraDeviceStatus status)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(2), [this, &name, status] {
            return std::any_of(m_responses.begin(), m_responses.end(),
                [&name, status](const DeviceResponse &response) {
                    return response.m_status == status && response.m_imageName == name;
                });
        });
    }

    std::vector<DeviceResponse> Responses() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_responses;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<DeviceResponse> m_responses;
};

std::shared_ptr<AstraBootImage> MakeBootImage(const test::TempDirectory &directory,
    const std::string &filename = "minildr.img", const std::string &payload = "boot-payload")
{
    auto fields = test::BootManifest();
    fields["secure_boot"] = "gen2";
    test::WriteBootManifest(directory, fields);
    directory.Write(filename, payload);
    auto image = std::make_shared<AstraBootImage>(directory.Path().string());
    REQUIRE(image->Load());
    return image;
}

std::vector<uint8_t> Request(const std::string &name, uint8_t type = 0x01,
    const std::string &prefix = "")
{
    std::vector<uint8_t> packet(prefix.begin(), prefix.end());
    packet.insert(packet.end(), imageRequestMagic, imageRequestMagic + sizeof(imageRequestMagic) - 1);
    packet.push_back(type);
    packet.insert(packet.end(), name.begin(), name.end());
    packet.push_back(0);
    return packet;
}

std::vector<uint8_t> SizeHeader(uint32_t size)
{
    return {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
        static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24), 0, 0, 0, 0};
}

void StartBoot(AstraDeviceImpl &device, const std::shared_ptr<AstraBootImage> &image)
{
    REQUIRE(device.Boot(image, ASTRA_DEVICE_BOOT_STAGE_AUTO) == 0);
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("boot-opens-device-and-creates-service-files", [] {
        test::TempDirectory directory;
        auto image = MakeBootImage(directory);
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);

        StartBoot(*device, image);

        const auto deviceDirectory = directory.Path() / "devicefake-usb-0";
        REQUIRE(usbProbe->OpenCount() == 1);
        REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_START);
        REQUIRE(test::ReadFile(deviceDirectory / "06_IMAGE") == "fake-usb-0");
        REQUIRE(!std::filesystem::exists(deviceDirectory / "07_IMAGE"));
        device->Close();
        REQUIRE(usbProbe->CloseCount() == 1);
    });
    suite.Add("boot-rejects-null-image-and-propagates-open-failure", [] {
        test::TempDirectory directory;
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        usbProbe->SetOpenResult(-7);
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);

        REQUIRE(device->Boot(nullptr, ASTRA_DEVICE_BOOT_STAGE_AUTO) == -1);
        auto image = MakeBootImage(directory);
        REQUIRE(device->Boot(image, ASTRA_DEVICE_BOOT_STAGE_AUTO) == -7);
        REQUIRE(usbProbe->OpenCount() == 1);
        device->Close();
    });
    suite.Add("truncated-request-is-ignored-and-console-bytes-are-readable", [] {
        test::TempDirectory directory;
        auto image = MakeBootImage(directory);
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);
        StartBoot(*device, image);

        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT,
            std::vector<uint8_t>(imageRequestMagic, imageRequestMagic + sizeof(imageRequestMagic) - 1));
        const std::vector<uint8_t> consoleBytes{'U', '-', 'B', 'o', 'o', 't', '>', ' '};
        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, consoleBytes);

        std::string received;
        REQUIRE(device->ReceiveFromConsole(received) == 0);
        REQUIRE(received == std::string(consoleBytes.begin(), consoleBytes.end()));
        REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_START);
        device->Close();
    });
    suite.Add("final-image-size-handshake-streams-header-and-payload", [] {
        test::TempDirectory directory;
        const std::string bootPayload = "signed-miniloader-image";
        auto image = MakeBootImage(directory, "minildr.img", bootPayload);
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", true);
        StatusCollector statuses;
        statuses.Install(*device);
        StartBoot(*device, image);

        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("minildr.img", 0x80, "padding"));
        REQUIRE(statuses.WaitFor(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE));
        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("07_IMAGE"));
        REQUIRE(statuses.WaitFor(ASTRA_DEVICE_STATUS_BOOT_COMPLETE));
        REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_COMPLETE);

        const auto deviceDirectory = directory.Path() / "devicefake-usb-0";
        const std::string sizeBytes = test::ReadFile(deviceDirectory / "07_IMAGE");
        REQUIRE(sizeBytes.size() == 4);
        REQUIRE(static_cast<uint8_t>(sizeBytes[0]) == bootPayload.size());
        REQUIRE(sizeBytes[1] == 0 && sizeBytes[2] == 0 && sizeBytes[3] == 0);

        const auto writes = usbProbe->Writes();
        REQUIRE(writes.size() == 4);
        REQUIRE(writes[0] == SizeHeader(static_cast<uint32_t>(bootPayload.size())));
        REQUIRE(writes[1] == std::vector<uint8_t>(bootPayload.begin(), bootPayload.end()));
        REQUIRE(writes[2] == SizeHeader(4));
        REQUIRE(writes[3] == std::vector<uint8_t>(sizeBytes.begin(), sizeBytes.end()));

        const auto responses = statuses.Responses();
        REQUIRE(std::none_of(responses.begin(), responses.end(), [](const DeviceResponse &response) {
            return response.m_imageName == "07_IMAGE";
        }));
        device->Close();
    });
    suite.Add("back-to-back-interrupts-are-served-in-order", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["secure_boot"] = "gen2";
        test::WriteBootManifest(directory, fields);
        directory.Write("first.bin", "first");
        directory.Write("second.bin", "second-image");
        directory.Write("third.bin", "third-payload");
        auto image = std::make_shared<AstraBootImage>(directory.Path().string());
        REQUIRE(image->Load());

        std::mutex gateMutex;
        std::condition_variable gateChanged;
        bool firstWriteEntered = false;
        bool releaseFirstWrite = false;
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        usbProbe->SetOnWrite([&](const std::vector<uint8_t> &) {
            std::unique_lock<std::mutex> lock(gateMutex);
            if (firstWriteEntered) {
                return;
            }
            firstWriteEntered = true;
            gateChanged.notify_all();
            gateChanged.wait(lock, [&] { return releaseFirstWrite; });
        });
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);
        StatusCollector statuses;
        statuses.Install(*device);
        StartBoot(*device, image);

        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("first.bin"));
        bool entered = false;
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            entered = gateChanged.wait_for(lock, std::chrono::seconds(2),
                [&] { return firstWriteEntered; });
        }
        if (!entered) {
            {
                std::lock_guard<std::mutex> lock(gateMutex);
                releaseFirstWrite = true;
            }
            gateChanged.notify_all();
        }
        REQUIRE(entered);

        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("second.bin"));
        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("third.bin"));
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            releaseFirstWrite = true;
        }
        gateChanged.notify_all();

        REQUIRE(statuses.WaitForImage("first.bin", ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE));
        REQUIRE(statuses.WaitForImage("second.bin", ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE));
        REQUIRE(statuses.WaitForImage("third.bin", ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE));

        const auto writes = usbProbe->Writes();
        REQUIRE(writes.size() == 6);
        REQUIRE(writes[0] == SizeHeader(5));
        REQUIRE(writes[1] == std::vector<uint8_t>({'f', 'i', 'r', 's', 't'}));
        REQUIRE(writes[2] == SizeHeader(12));
        REQUIRE(writes[3] == std::vector<uint8_t>({
            's', 'e', 'c', 'o', 'n', 'd', '-', 'i', 'm', 'a', 'g', 'e'}));
        REQUIRE(writes[4] == SizeHeader(13));
        REQUIRE(writes[5] == std::vector<uint8_t>({
            't', 'h', 'i', 'r', 'd', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd'}));
        device->Close();
    });
    suite.Add("unknown-image-request-reports-boot-failure", [] {
        test::TempDirectory directory;
        auto image = MakeBootImage(directory);
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);
        StatusCollector statuses;
        statuses.Install(*device);
        StartBoot(*device, image);

        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("missing.img"));

        REQUIRE(statuses.WaitFor(ASTRA_DEVICE_STATUS_BOOT_FAIL));
        const auto responses = statuses.Responses();
        const auto failure = std::find_if(responses.begin(), responses.end(), [](const DeviceResponse &response) {
            return response.m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL;
        });
        REQUIRE(failure != responses.end());
        REQUIRE(failure->m_message == "missing.img image not found");
        device->Close();
    });
    suite.Add("disconnect-during-boot-progress-reports-failure", [] {
        for (const auto event : {USBDevice::USB_DEVICE_EVENT_NO_DEVICE,
            USBDevice::USB_DEVICE_EVENT_TRANSFER_CANCELED,
            USBDevice::USB_DEVICE_EVENT_TRANSFER_ERROR}) {
            test::TempDirectory directory;
            auto image = MakeBootImage(directory, "intermediate.bin", "intermediate-image");
            auto usb = std::make_unique<test::ScriptedUSBDevice>();
            auto *usbProbe = usb.get();
            auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
                true, "", false);
            StatusCollector statuses;
            statuses.Install(*device);
            StartBoot(*device, image);

            usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("intermediate.bin"));
            REQUIRE(statuses.WaitFor(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE));
            usbProbe->Emit(event);

            REQUIRE(statuses.WaitFor(ASTRA_DEVICE_STATUS_BOOT_FAIL));
            REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_FAIL);
            const auto responses = statuses.Responses();
            REQUIRE(std::any_of(responses.begin(), responses.end(), [](const DeviceResponse &response) {
                return response.m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL &&
                    response.m_message == "Device disconnected";
            }));
            REQUIRE(device->WaitForCompletion() == 0);
            device->Close();
        }
    });
    suite.Add("miniloader-reconnect-disconnect-is-not-reported-as-failure", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        test::WriteBootManifest(directory, fields);
        directory.Write("gen3_miniloader.bin.usb", "miniloader");
        auto image = std::make_shared<AstraBootImage>(directory.Path().string());
        REQUIRE(image->Load());

        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);
        StatusCollector statuses;
        statuses.Install(*device);
        StartBoot(*device, image);

        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT, Request("gen3_miniloader.bin.usb"));
        REQUIRE(statuses.WaitFor(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE));
        usbProbe->Emit(USBDevice::USB_DEVICE_EVENT_NO_DEVICE);

        REQUIRE(device->WaitForCompletion() == 0);
        REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_PROGRESS);
        const auto responses = statuses.Responses();
        REQUIRE(std::none_of(responses.begin(), responses.end(), [](const DeviceResponse &response) {
            return response.m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL;
        }));
        device->Close();
    });
    suite.Add("console-write-forwards-bytes-and-transport-error", [] {
        test::TempDirectory directory;
        auto image = MakeBootImage(directory);
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);
        StartBoot(*device, image);

        REQUIRE(device->SendToConsole("reset\n") == 0);
        usbProbe->SetInterruptWriteResult(-3);
        REQUIRE(device->SendToConsole("boot\n") == -3);
        const auto writes = usbProbe->InterruptWrites();
        REQUIRE(writes == std::vector<std::vector<uint8_t>>({
            {'r', 'e', 's', 'e', 't', '\n'}, {'b', 'o', 'o', 't', '\n'}}));
        device->Close();
    });
    suite.Add("close-wakes-completion-wait-and-is-idempotent", [] {
        test::TempDirectory directory;
        auto image = MakeBootImage(directory);
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL16XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);
        StartBoot(*device, image);
        auto waiter = std::async(std::launch::async, [&] { return device->WaitForCompletion(); });

        std::thread firstClose([&] { device->Close(); });
        std::thread secondClose([&] { device->Close(); });
        firstClose.join();
        secondClose.join();
        REQUIRE(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        REQUIRE(waiter.get() == 0);
        device->Close();
        REQUIRE(usbProbe->CloseCount() == 1);
        REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_CLOSED);
    });
    return suite.Run(argc, argv);
}
