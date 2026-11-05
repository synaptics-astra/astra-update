// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <chrono>
#include <condition_variable>
#include <deque>
#include <algorithm>
#include <mutex>
#include <optional>

#include "astra_device_impl_internal.hpp"
#include "boot_image_fixtures.hpp"
#include "scripted_usb_device.hpp"
#include "test_support.hpp"

namespace {

class RequestLoopProbe final : public AstraDeviceImpl
{
public:
    struct Request {
        std::string name;
        uint8_t type;
    };

    RequestLoopProbe(std::unique_ptr<test::ScriptedUSBDevice> usb, bool bootOnly = false,
        bool keepLoop = false)
        : AstraDeviceImpl(std::move(usb), "", bootOnly, "boot-test", keepLoop),
          m_usb(static_cast<test::ScriptedUSBDevice *>(m_usbDevice.get()))
    {
        m_deviceName = "probe-device";
    }

    ~RequestLoopProbe() override { Stop(); }

    int Boot(std::shared_ptr<AstraBootImage>, AstraDeviceBootStage) override { return 0; }
    int Update(std::shared_ptr<FlashImage>) override { return 0; }
    int WaitForCompletion() override { return 0; }
    int SendToConsole(const std::string &) override { return 0; }
    int ReceiveFromConsole(std::string &) override { return 0; }

    void AddImage(const std::filesystem::path &path, const std::string &name,
        AstraImageType type = ASTRA_IMAGE_TYPE_BOOT)
    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        m_images.emplace_back(path.string(), type, name);
    }

    void SetFinalImages(std::string boot, std::string update = "")
    {
        m_finalBootImage = std::move(boot);
        m_finalUpdateImage = std::move(update);
    }

    void SetSizeRequest(std::string name) { m_sizeRequestImageFilename = std::move(name); }
    void SetCompleteOnIdleTimeout(bool value) { m_completeOnIdleTimeout = value; }
    void SetStatus(AstraDeviceStatus status) { m_status = status; }
    void SetDeviceDirectory(const std::filesystem::path &path) { m_deviceDir = path.string(); }
    void SetUuid(std::string uuid) { m_updateSessionUuid = std::move(uuid); }
    std::string Uuid() const { return m_updateSessionUuid; }
    std::string FinalBootImage() const { return m_finalBootImage; }
    std::string FinalUpdateImage() const { return m_finalUpdateImage; }
    bool Running() const { return m_running.load(); }

    bool Build(std::shared_ptr<AstraBootImage> image, AstraDeviceBootStage stage)
    {
        BuildBootImageList(std::move(image), stage);
        return m_uEnvSupport;
    }

    void Append(std::shared_ptr<FlashImage> image) { AppendUpdateImages(std::move(image)); }
    bool WriteUEnv(const std::string &command) { return WriteUEnvFile(command); }
    bool ResetWhenComplete() const { return m_resetWhenComplete; }

    std::vector<std::string> ImageNames()
    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        std::vector<std::string> names;
        for (const auto &image : m_images) {
            names.push_back(image.GetName());
        }
        return names;
    }

    int Start(AstraDeviceStatus status)
    {
        m_status = status;
        return StartImageRequestThread();
    }

    void Queue(Request request)
    {
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            m_requests.emplace_back(std::move(request));
        }
        m_requestChanged.notify_all();
    }

    void QueueTimeout()
    {
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            m_requests.emplace_back(std::nullopt);
        }
        m_requestChanged.notify_all();
    }

    bool WaitForRequestCount(size_t count)
    {
        std::unique_lock<std::mutex> lock(m_requestMutex);
        return m_requestChanged.wait_for(lock, std::chrono::seconds(2),
            [&] { return m_requestsObserved >= count; });
    }

    bool WaitForSentCount(size_t count)
    {
        std::unique_lock<std::mutex> lock(m_sentMutex);
        return m_sentChanged.wait_for(lock, std::chrono::seconds(2),
            [&] { return m_sent.size() >= count; });
    }

    bool WaitForStatus(AstraDeviceStatus status)
    {
        std::unique_lock<std::mutex> lock(m_statusMutex);
        return m_statusChanged.wait_for(lock, std::chrono::seconds(2), [&] {
            return std::any_of(m_statuses.begin(), m_statuses.end(),
                [status](const DeviceResponse &response) { return response.m_status == status; });
        });
    }

    void CaptureStatuses()
    {
        SetStatusCallback([this](AstraDeviceManagerResponse response) {
            if (response.IsDeviceResponse()) {
                const auto device = response.GetDeviceResponse();
                {
                    std::lock_guard<std::mutex> lock(m_statusMutex);
                    m_statuses.push_back(device);
                }
                m_statusChanged.notify_all();
            }
        });
    }

    std::vector<DeviceResponse> Statuses() const
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_statuses;
    }

    std::vector<std::pair<std::string, bool>> Sent() const
    {
        std::lock_guard<std::mutex> lock(m_sentMutex);
        return m_sent;
    }

    std::vector<std::string> Payloads() const
    {
        std::lock_guard<std::mutex> lock(m_sentMutex);
        return m_payloads;
    }

    void SetSendResult(int result) { m_sendResult = result; }
    void Suppress(std::string name) { m_suppressed = std::move(name); }

    void Stop()
    {
        StopImageRequestThread();
    }

    void JoinLoop()
    {
        if (m_imageRequestThread.joinable()) {
            m_imageRequestThread.join();
        }
    }

    test::ScriptedUSBDevice &USB() { return *m_usb; }

protected:
    bool WaitForImageRequest(std::string &name, uint8_t &type,
        std::chrono::milliseconds timeout) override
    {
        std::unique_lock<std::mutex> lock(m_requestMutex);
        if (m_requests.empty()) {
            m_requestChanged.wait_for(lock, timeout, [&] {
                return !m_requests.empty() || !m_running.load();
            });
        }
        if (m_requests.empty()) {
            return false;
        }
        auto request = std::move(m_requests.front());
        m_requests.pop_front();
        if (!request) {
            ++m_requestsObserved;
            m_requestChanged.notify_all();
            return false;
        }
        name = request->name;
        type = request->type;
        ++m_requestsObserved;
        m_requestChanged.notify_all();
        return true;
    }

    int SendImagePayload(Image &image) override
    {
        std::string payload;
        const int loadResult = image.Load();
        if (loadResult == 0) {
            std::vector<uint8_t> bytes(image.GetSize());
            const int count = image.GetDataBlock(bytes.data(), bytes.size());
            if (count > 0) {
                payload.assign(bytes.begin(), bytes.begin() + count);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_sentMutex);
            m_payloads.push_back(std::move(payload));
        }
        return loadResult == 0 ? m_sendResult : -1;
    }

    void OnImageSent(const Image &image, bool success) override
    {
        std::lock_guard<std::mutex> lock(m_sentMutex);
        m_sent.emplace_back(image.GetName(), success);
        m_sentChanged.notify_all();
    }

    bool ShouldSuppressImageStatus(const std::string &name) override
    {
        return name == m_suppressed;
    }

    void WakeImageRequestThread() override
    {
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
        }
        m_requestChanged.notify_all();
    }

private:
    test::ScriptedUSBDevice *m_usb;
    mutable std::mutex m_requestMutex;
    std::condition_variable m_requestChanged;
    std::deque<std::optional<Request>> m_requests;
    size_t m_requestsObserved = 0;
    mutable std::mutex m_statusMutex;
    std::condition_variable m_statusChanged;
    std::vector<DeviceResponse> m_statuses;
    mutable std::mutex m_sentMutex;
    std::condition_variable m_sentChanged;
    std::vector<std::pair<std::string, bool>> m_sent;
    std::vector<std::string> m_payloads;
    int m_sendResult = 0;
    std::string m_suppressed;
};

std::shared_ptr<AstraBootImage> LoadBootImage(const test::TempDirectory &directory,
    test::Manifest fields, const std::vector<std::pair<std::string, std::string>> &files)
{
    for (const auto &file : files) {
        directory.Write(file.first, file.second);
    }
    test::WriteBootManifest(directory, fields);
    auto image = std::make_shared<AstraBootImage>(directory.Path().string());
    REQUIRE(image->Load());
    return image;
}

std::vector<AstraDeviceStatus> StatusSequence(const std::vector<DeviceResponse> &responses)
{
    std::vector<AstraDeviceStatus> statuses;
    for (const auto &response : responses) {
        statuses.push_back(response.m_status);
    }
    return statuses;
}

void RequireImageNames(RequestLoopProbe &probe, std::vector<std::string> expected)
{
    auto actual = probe.ImageNames();
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    REQUIRE(actual == expected);
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("generated-uenv-has-reusable-32-hex-uuid", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["uenv_support"] = "true";
        const auto boot = LoadBootImage(directory, fields, {{"boot.bin", "boot"}});
        test::ScriptedUSBDevice *raw = nullptr;
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        raw = usb.get();
        RequestLoopProbe probe(std::move(usb));
        const auto deviceDirectory = directory.Path() / "device";
        std::filesystem::create_directory(deviceDirectory);
        probe.SetDeviceDirectory(deviceDirectory);
        REQUIRE(probe.Build(boot, ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER));
        REQUIRE(probe.FinalBootImage() == "uEnv.txt");
        RequireImageNames(probe, {"boot.bin", "uEnv.txt"});
        auto content = test::ReadFile(deviceDirectory / "uEnv.txt");
        const std::string marker = "setenv serial# ";
        auto uuidStart = content.find(marker);
        REQUIRE(uuidStart != std::string::npos);
        uuidStart += marker.size();
        const auto uuid = content.substr(uuidStart, 32);
        REQUIRE(uuid.size() == 32);
        REQUIRE(std::all_of(uuid.begin(), uuid.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }));
        REQUIRE(content == "bootcmd=setenv serial# " + uuid + "; boot-test");
        REQUIRE(probe.WriteUEnv("updated-command"));
        REQUIRE(test::ReadFile(deviceDirectory / "uEnv.txt") ==
            "bootcmd=setenv serial# " + uuid + "; updated-command");
        probe.Close();
        REQUIRE(raw->CloseCount() == 1);
    });
    suite.Add("supplied-uenv-is-not-overwritten", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["uenv_support"] = "true";
        const auto boot = LoadBootImage(directory, fields,
            {{"boot.bin", "boot"}, {"uEnv.txt", "provided-uenv"}});
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.SetDeviceDirectory(directory.Path());
        REQUIRE(probe.Build(boot, ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER));
        RequireImageNames(probe, {"boot.bin", "uEnv.txt"});
        REQUIRE(test::ReadFile(directory.Path() / "uEnv.txt") == "provided-uenv");
        probe.Close();
    });
    suite.Add("linux-image-final-selection-differs-for-boot-only", [] {
        test::TempDirectory directory;
        const auto boot = LoadBootImage(directory, test::BootManifest(),
            {{"Image.gz", "kernel"}, {"ramdisk.cpio.gz", "ramdisk"}, {"uEnv.txt", "provided"}});
        RequestLoopProbe bootOnly(std::make_unique<test::ScriptedUSBDevice>(), true);
        bootOnly.SetDeviceDirectory(directory.Path());
        REQUIRE(!bootOnly.Build(boot, ASTRA_DEVICE_BOOT_STAGE_LINUX));
        REQUIRE(bootOnly.FinalBootImage() == "ramdisk.cpio.gz");
        RequireImageNames(bootOnly, {"Image.gz", "ramdisk.cpio.gz", "uEnv.txt"});
        bootOnly.Close();

        RequestLoopProbe update(std::make_unique<test::ScriptedUSBDevice>());
        update.SetDeviceDirectory(directory.Path());
        REQUIRE(!update.Build(boot, ASTRA_DEVICE_BOOT_STAGE_LINUX));
        REQUIRE(update.FinalBootImage() == "uEnv.txt");
        RequireImageNames(update, {"Image.gz", "ramdisk.cpio.gz", "uEnv.txt"});
        update.Close();
    });
    suite.Add("missing-payload-file-is-a-send-failure", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.AddImage(directory.Path() / "missing.bin", "missing.bin");
        probe.SetFinalImages("missing.bin");
        probe.Queue({"missing.bin", 0});
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL));
        probe.JoinLoop();
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({
            {"missing.bin", false}}));
        REQUIRE(probe.Payloads() == std::vector<std::string>({""}));
        probe.Close();
    });
    suite.Add("uenv-write-failure-is-reported", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.SetDeviceDirectory(directory.Path() / "missing");
        REQUIRE(!probe.WriteUEnv("boot-test"));
        probe.Close();
    });
    suite.Add("boot-only-final-image-completes-without-size-request", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>(), true);
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("boot.bin", "boot"), "boot.bin");
        probe.SetFinalImages("boot.bin");
        probe.Queue({"boot.bin", 0});
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForRequestCount(2));
        probe.JoinLoop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_COMPLETE);
        REQUIRE(StatusSequence(probe.Statuses()) == std::vector<AstraDeviceStatus>({
            ASTRA_DEVICE_STATUS_IMAGE_SEND_START, ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE}));
        probe.Close();
    });
    suite.Add("nand-update-does-not-wait-for-size-request", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.AddImage(directory.Write("nand.img", "nand"), "nand.img", ASTRA_IMAGE_TYPE_UPDATE_NAND);
        probe.AddImage(directory.Write("size.req", "size"), "size.req", ASTRA_IMAGE_TYPE_BOOT);
        probe.SetFinalImages("", "nand.img");
        probe.SetSizeRequest("size.req");
        probe.Queue({"nand.img", 0});
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_START) == 0);
        REQUIRE(probe.WaitForRequestCount(2));
        probe.JoinLoop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE);
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({
            {"nand.img", true}}));
        probe.Close();
    });
    suite.Add("append-update-images-copies-payload-and-tracks-final", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", "image_type: spi\nimages:\n  first.bin: {}\n  final.bin: {}\n");
        directory.Write("first.bin", "first");
        directory.Write("final.bin", "final");
        test::Manifest config;
        auto flash = FlashImage::FlashImageFactory(directory.Path().string(), config);
        REQUIRE(flash->Load() == 0);
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.Append(flash);
        REQUIRE(probe.FinalUpdateImage() == "final.bin");
        REQUIRE(probe.ResetWhenComplete());
        REQUIRE(probe.ImageNames() == std::vector<std::string>({"first.bin", "final.bin"}));
        probe.Close();
    });
    suite.Add("final-boot-request-normalizes-slashes-and-reports-sequence", [] {
        test::TempDirectory directory;
        const auto payload = directory.Write("boot.bin", "boot-payload");
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        RequestLoopProbe probe(std::move(usb));
        probe.CaptureStatuses();
        probe.AddImage(payload, "boot.bin");
        probe.SetFinalImages("boot.bin");
        probe.Queue({"some\\prefix/boot.bin", 7});
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE));
        probe.Stop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_COMPLETE);
        REQUIRE(probe.Payloads() == std::vector<std::string>({"boot-payload"}));
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({{"boot.bin", true}}));
        const auto statuses = probe.Statuses();
        REQUIRE(StatusSequence(statuses) == std::vector<AstraDeviceStatus>({
            ASTRA_DEVICE_STATUS_IMAGE_SEND_START, ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE,
            ASTRA_DEVICE_STATUS_BOOT_COMPLETE}));
        REQUIRE(statuses[0].m_imageName == "boot.bin" && statuses[0].m_progress == 0);
        REQUIRE(statuses[1].m_progress == 100);
        REQUIRE(statuses[2].m_message == "Success");
    });
    suite.Add("update-final-image-and-idle-completion", [] {
        test::TempDirectory directory;
        const auto payload = directory.Write("final.img", "firmware");
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.AddImage(payload, "final.img", ASTRA_IMAGE_TYPE_UPDATE_EMMC);
        probe.SetFinalImages("", "final.img");
        probe.Queue({"final.img", 0});
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_START) == 0);
        REQUIRE(probe.WaitForRequestCount(2));
        probe.JoinLoop();
        REQUIRE(!probe.Running());
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE);
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({{"final.img", true}}));
        REQUIRE(StatusSequence(probe.Statuses()) == std::vector<AstraDeviceStatus>({
            ASTRA_DEVICE_STATUS_IMAGE_SEND_START, ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE}));
        probe.Close();
    });
    suite.Add("size-request-handshake-gates-update-completion", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("flash.img", "flash"), "flash.img", ASTRA_IMAGE_TYPE_UPDATE_SPI);
        probe.AddImage(directory.Write("size.req", "size"), "size.req", ASTRA_IMAGE_TYPE_BOOT);
        probe.SetFinalImages("", "flash.img");
        probe.SetSizeRequest("size.req");
        probe.Queue({"flash.img", 0});
        probe.Queue({"size.req", 0});
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_START) == 0);
        REQUIRE(probe.WaitForRequestCount(3));
        probe.JoinLoop();
        REQUIRE(!probe.Running());
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE);
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({
            {"flash.img", true}, {"size.req", true}}));
        probe.Close();
    });
    suite.Add("boot-size-request-handshake", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>(), true);
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("boot.bin", "boot"), "boot.bin");
        probe.AddImage(directory.Write("07_IMAGE", "size"), "07_IMAGE");
        probe.SetFinalImages("boot.bin");
        probe.SetSizeRequest("07_IMAGE");
        probe.Suppress("07_IMAGE");
        probe.Queue({"boot.bin", 0});
        probe.Queue({"07_IMAGE", 0});
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForRequestCount(3));
        probe.JoinLoop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_COMPLETE);
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({
            {"boot.bin", true}, {"07_IMAGE", true}}));
        REQUIRE(StatusSequence(probe.Statuses()) == std::vector<AstraDeviceStatus>({
            ASTRA_DEVICE_STATUS_IMAGE_SEND_START, ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE}));
        probe.Close();
    });
    suite.Add("send-failure-sets-boot-fail-and-notifies", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("boot.bin", "boot"), "boot.bin");
        probe.SetFinalImages("boot.bin");
        probe.SetSendResult(-1);
        probe.Queue({"boot.bin", 0});
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL));
        probe.JoinLoop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_FAIL);
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({{"boot.bin", false}}));
        const auto statuses = probe.Statuses();
        REQUIRE(StatusSequence(statuses) == std::vector<AstraDeviceStatus>({
            ASTRA_DEVICE_STATUS_IMAGE_SEND_START, ASTRA_DEVICE_STATUS_BOOT_FAIL}));
        REQUIRE(statuses.back().m_message == "Failed to send image");
        probe.Close();
    });
    suite.Add("missing-requested-image-sets-update-fail", [] {
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.Queue({"missing.bin", 0});
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_START) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL));
        probe.JoinLoop();
        const auto statuses = probe.Statuses();
        REQUIRE(statuses.size() == 1);
        REQUIRE(statuses.front().m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL);
        REQUIRE(statuses.front().m_imageName == "missing.bin");
        REQUIRE(statuses.front().m_message == "missing.bin image not found");
        probe.Close();
    });
    suite.Add("suppressed-image-status-keeps-final-completion", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("quiet.bin", "payload"), "quiet.bin");
        probe.SetFinalImages("quiet.bin");
        probe.Suppress("quiet.bin");
        probe.Queue({"quiet.bin", 0});
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE));
        probe.Stop();
        REQUIRE(StatusSequence(probe.Statuses()) ==
            std::vector<AstraDeviceStatus>({ASTRA_DEVICE_STATUS_BOOT_COMPLETE}));
        probe.Close();
    });
    suite.Add("keep-loop-after-boot-reports-post-boot-failure", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>(), true, true);
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("boot.bin", "boot"), "boot.bin");
        probe.AddImage(directory.Write("post.bin", "post"), "post.bin");
        probe.SetFinalImages("boot.bin");
        probe.SetSendResult(0);
        probe.Queue({"boot.bin", 0});
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_START) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE));
        probe.SetSendResult(-1);
        probe.Queue({"post.bin", 0});
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL));
        probe.Stop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_BOOT_COMPLETE);
        const auto statuses = StatusSequence(probe.Statuses());
        REQUIRE(statuses == std::vector<AstraDeviceStatus>({
            ASTRA_DEVICE_STATUS_IMAGE_SEND_START, ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE,
            ASTRA_DEVICE_STATUS_BOOT_COMPLETE, ASTRA_DEVICE_STATUS_IMAGE_SEND_START,
            ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL}));
        REQUIRE(probe.Sent() == std::vector<std::pair<std::string, bool>>({
            {"boot.bin", true}, {"post.bin", false}}));
        probe.Close();
    });
    suite.Add("boot-timeout-fails-and-wakes-device-event", [] {
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_BOOT_PROGRESS) == 0);
        REQUIRE(probe.WaitForStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL));
        probe.JoinLoop();
        REQUIRE(!probe.Running());
        REQUIRE(probe.Statuses().front().m_message.find("Timeout during boot") != std::string::npos);
        probe.Close();
    });
    suite.Add("update-idle-timeout-can-await-real-disconnect", [] {
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.SetCompleteOnIdleTimeout(false);
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) == 0);
        REQUIRE(probe.WaitForRequestCount(1));
        probe.JoinLoop();
        REQUIRE(probe.Running());
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE);
        probe.Stop();
        REQUIRE(!probe.Running());
        probe.Close();
    });
    suite.Add("stop-wakes-blocked-request-hook-and-close-is-idempotent", [] {
        auto usb = std::make_unique<test::ScriptedUSBDevice>();
        auto *raw = usb.get();
        RequestLoopProbe probe(std::move(usb));
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_PROGRESS) == 0);
        probe.Stop();
        probe.Close();
        probe.Close();
        REQUIRE(raw->CloseCount() == 1);
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_CLOSED);
    });
    suite.Add("characterization-final-image-match-is-substring-based", [] {
        test::TempDirectory directory;
        RequestLoopProbe probe(std::make_unique<test::ScriptedUSBDevice>());
        probe.CaptureStatuses();
        probe.AddImage(directory.Write("backup-final.img.old", "payload"), "backup-final.img.old",
            ASTRA_IMAGE_TYPE_UPDATE_SPI);
        probe.SetFinalImages("", "final.img");
        probe.Queue({"backup-final.img.old", 0});
        probe.QueueTimeout();
        REQUIRE(probe.Start(ASTRA_DEVICE_STATUS_UPDATE_START) == 0);
        REQUIRE(probe.WaitForRequestCount(2));
        probe.JoinLoop();
        REQUIRE(probe.GetDeviceStatus() == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE);
        probe.Close();
    });
    return suite.Run(argc, argv);
}
