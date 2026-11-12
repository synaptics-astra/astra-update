// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <cstdint>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "astra_device_impl_internal.hpp"
#include "boot_image_fixtures.hpp"
#include "scripted_usb_device.hpp"
#include "test_support.hpp"

namespace {

constexpr uint32_t kKeyResendSpkVersion = 0xA00C0307U;

class BootRomUSB final : public test::ScriptedUSBDevice
{
public:
    explicit BootRomUSB(uint32_t spkVersion, bool invalidVersionResponse = false)
        : m_spkVersion(spkVersion), m_invalidVersionResponse(invalidVersionResponse)
    {
        SetOnWrite([this](const std::vector<uint8_t> &packet) {
            if (packet.size() < 4) {
                return;
            }

            std::vector<uint8_t> response(8, 0);
            response[0] = m_invalidVersionResponse && packet[3] == 0x03 ? 0 : 0x5B;
            response[1] = 0x5A;
            if (packet[3] == 0x03) {
                response[4] = static_cast<uint8_t>(m_spkVersion);
                response[5] = static_cast<uint8_t>(m_spkVersion >> 8);
                response[6] = static_cast<uint8_t>(m_spkVersion >> 16);
                response[7] = static_cast<uint8_t>(m_spkVersion >> 24);
            }
            Emit(USB_DEVICE_EVENT_INTERRUPT, response);
        });
    }

    uint16_t GetVendorId() const override { return 0x06CB; }
    uint16_t GetProductId() const override { return 0x019E; }
    uint8_t GetNumInterfaces() const override { return 2; }

private:
    uint32_t m_spkVersion;
    bool m_invalidVersionResponse;
};

std::shared_ptr<AstraBootImage> MakeBootImage(const test::TempDirectory &directory)
{
    test::WriteBootManifest(directory, test::BootManifest());
    directory.Write("key.bin", "key-payload");
    directory.Write("spk.bin", "spk-payload");
    directory.Write("m52bl.bin", "m52bl-payload");
    auto image = std::make_shared<AstraBootImage>(directory.Path().string());
    REQUIRE(image->Load());
    return image;
}

std::vector<uint8_t> UploadOpcodes(const std::vector<std::vector<uint8_t>> &writes)
{
    std::vector<uint8_t> opcodes;
    for (const auto &write : writes) {
        REQUIRE(write.size() >= 4);
        opcodes.push_back(write[3]);
    }
    return opcodes;
}

void RequireBootROMUploadSequence(uint32_t spkVersion, std::vector<uint8_t> expectedOpcodes)
{
    test::TempDirectory directory;
    const auto image = MakeBootImage(directory);
    auto usb = std::make_unique<BootRomUSB>(spkVersion);
    auto *usbProbe = usb.get();
    auto device = CreateAstraDeviceSL26XXImpl(std::move(usb), directory.Path().string(),
        true, "", false);

    REQUIRE(device->Boot(image, ASTRA_DEVICE_BOOT_STAGE_AUTO) == 1);
    REQUIRE(UploadOpcodes(usbProbe->Writes()) == expectedOpcodes);
    REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_PROGRESS);
    device->Close();
}

class FastbootUSB final : public test::ScriptedUSBDevice
{
public:
    FastbootUSB()
    {
        SetOnWrite([this](const std::vector<uint8_t> &packet) {
            const std::string command(packet.begin(), packet.end());
            if (command == "getvar:serialno") {
                QueueRead("OKAY0123456789abcdef0123456789abcdef");
            } else if (command == "getvar:fb_command") {
                QueueRead(requestImage.exchange(false) ? "OKAYstage final.bin" : "OKAY");
            } else if (command == "download:00000007") {
                QueueRead("DATA00000007");
                QueueRead("OKAY");
            } else if (command == "oem run:setenv fb_ret OKAY") {
                QueueRead("OKAY");
            }
        });
    }

    uint16_t GetVendorId() const override { return 0x18D1; }
    uint16_t GetProductId() const override { return 0x4EE0; }
    std::atomic<bool> requestImage{false};
};

class FinalFlashImage final : public FlashImage
{
public:
    FinalFlashImage(const test::TempDirectory &directory, bool reset)
        : FlashImage(FLASH_IMAGE_TYPE_SPI, directory.Path().string(), "", "sl2619", "rdk",
            ASTRA_SECURE_BOOT_V3, ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED,
            ASTRA_UBOOT_VERSION_2025_01, reset, nullptr)
    {
        m_finalImage = "final.bin";
        const auto path = directory.Write(m_finalImage, "payload");
        m_images.emplace_back(path.string(), ASTRA_IMAGE_TYPE_UPDATE_SPI);
    }
    int Load() override { return 0; }
};

void RequireFlashCompletion(const std::string &flag, bool reset = true, bool acceptReboot = true,
    bool openRebind = true)
{
    test::TempDirectory directory;
    auto fields = test::BootManifest();
    fields["fastboot_vendor_id"] = "18D1";
    fields["fastboot_product_id"] = "4EE0";
    if (!flag.empty()) {
        fields["fastboot_reboot"] = flag;
    }
    test::WriteBootManifest(directory, fields);
    auto bootImage = std::make_shared<AstraBootImage>(directory.Path().string());
    REQUIRE(bootImage->Load());

    auto usb = std::make_unique<FastbootUSB>();
    auto *usbProbe = usb.get();
    auto device = CreateAstraDeviceSL26XXImpl(std::move(usb), directory.Path().string(),
        false, "", false);
    std::promise<void> waiting;
    auto waitingFuture = waiting.get_future();
    std::atomic<int> legacyRegistrations{0};
    std::string completionMessage;
    device->SetStatusCallback([&](AstraDeviceManagerResponse response) {
        if (response.IsDeviceResponse()) {
            const auto event = response.GetDeviceResponse();
            if (event.m_status == ASTRA_DEVICE_STATUS_WAITING_FOR_DEVICE && event.m_imageName.empty()) {
                waiting.set_value();
            } else if (event.m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                completionMessage = event.m_message;
            }
        }
    });
    device->SetRegistrationCallbacks([](const std::string &) {}, [](const std::string &) {},
        [&](const std::string &) {
            ++legacyRegistrations;
            device->NotifyFinalBootDetected();
        }, [](const std::string &) {});

    REQUIRE(device->Boot(bootImage, ASTRA_DEVICE_BOOT_STAGE_AUTO) == 0);
    REQUIRE(device->Update(std::make_shared<FinalFlashImage>(directory, reset)) == 0);
    usbProbe->requestImage.store(true);
    REQUIRE(waitingFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    const bool useFastboot = flag == "true" && reset;
    FastbootUSB *reboundProbe = nullptr;
    if (useFastboot) {
        auto rebound = std::make_unique<FastbootUSB>();
        reboundProbe = rebound.get();
        rebound->SetOpenResult(openRebind ? 0 : -1);
        rebound->SetOnWrite([probe = reboundProbe, acceptReboot](const std::vector<uint8_t> &packet) {
            if (std::string(packet.begin(), packet.end()) == "reboot") {
                probe->QueueRead(acceptReboot ? "OKAY" : "FAILdenied");
                probe->QueueRead("", 0, -9);
                probe->QueueRead("", 0, -1);
            }
        });
        device->Rebind(std::move(rebound));
    }

    auto completion = std::async(std::launch::async, [&] { return device->WaitForCompletion(); });
    const bool completed = completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!completed) {
        device->Close();
    }
    REQUIRE(completed);
    const bool failed = useFastboot && (!acceptReboot || !openRebind);
    REQUIRE(completion.get() == (failed ? -1 : 0));
    REQUIRE(device->GetDeviceStatus() == (failed
        ? ASTRA_DEVICE_STATUS_UPDATE_FAIL : ASTRA_DEVICE_STATUS_UPDATE_COMPLETE));
    if (!failed) {
        REQUIRE(completionMessage == "Success");
    }
    REQUIRE(legacyRegistrations.load() == (useFastboot ? 0 : 1));
    if (reboundProbe && openRebind) {
        REQUIRE(reboundProbe->Writes() == std::vector<std::vector<uint8_t>>{
            {'r', 'e', 'b', 'o', 'o', 't'}});
        REQUIRE(reboundProbe->ReadTimeouts().size() == 1);
    }
    device->Close();
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("missing-fastboot-reboot-preserves-linux-gadget-completion", [] {
        RequireFlashCompletion("");
    });
    suite.Add("disabled-fastboot-reboot-preserves-linux-gadget-completion", [] {
        RequireFlashCompletion("false");
    });
    suite.Add("fastboot-reboot-completes-on-ack-without-further-usb-io", [] {
        RequireFlashCompletion("true");
    });
    suite.Add("fastboot-reboot-rejection-fails-update", [] {
        RequireFlashCompletion("true", true, false);
    });
    suite.Add("post-flash-fastboot-open-failure-does-not-report-success", [] {
        RequireFlashCompletion("true", true, true, false);
    });
    suite.Add("disabled-reset-does-not-send-fastboot-reboot", [] {
        RequireFlashCompletion("true", false);
    });
    suite.Add("old-spk-does-not-resend-key", [] {
        RequireBootROMUploadSequence(kKeyResendSpkVersion - 1, {0x01, 0x02, 0x03, 0x04});
    });
    suite.Add("spk-at-key-resend-threshold-resends-key-before-m52bl", [] {
        RequireBootROMUploadSequence(kKeyResendSpkVersion, {0x01, 0x02, 0x03, 0x01, 0x04});
    });
    suite.Add("invalid-spk-version-response-fails-before-m52bl", [] {
        test::TempDirectory directory;
        const auto image = MakeBootImage(directory);
        auto usb = std::make_unique<BootRomUSB>(kKeyResendSpkVersion, true);
        auto *usbProbe = usb.get();
        auto device = CreateAstraDeviceSL26XXImpl(std::move(usb), directory.Path().string(),
            true, "", false);

        REQUIRE(device->Boot(image, ASTRA_DEVICE_BOOT_STAGE_AUTO) == -1);
        REQUIRE(UploadOpcodes(usbProbe->Writes()) == std::vector<uint8_t>({0x01, 0x02, 0x03}));
        REQUIRE(device->GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_FAIL);
        device->Close();
    });
    return suite.Run(argc, argv);
}
