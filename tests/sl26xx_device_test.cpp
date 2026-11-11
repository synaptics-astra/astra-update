// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <cstdint>
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

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
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
