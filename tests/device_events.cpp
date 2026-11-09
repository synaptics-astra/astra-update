#include <iostream>
#include <stdexcept>
#include <vector>

#include "astra_device_impl_internal.hpp"

class EventTestDevice : public AstraDeviceImpl {
public:
    EventTestDevice(bool sizeHandshake, bool bootOnly, bool failTransfer,
        AstraDeviceStatus initialStatus = ASTRA_DEVICE_STATUS_UPDATE_START)
        : AstraDeviceImpl(nullptr, "", bootOnly, "", false),
          m_failTransfer(failTransfer)
    {
        m_status = bootOnly ? ASTRA_DEVICE_STATUS_BOOT_START : initialStatus;
        m_deviceName = "test-device";
        m_images.emplace_back("rootfs_subimg.0", ASTRA_IMAGE_TYPE_UPDATE_EMMC);
        m_images.emplace_back("rootfs_subimg.1", ASTRA_IMAGE_TYPE_UPDATE_EMMC);
        m_finalUpdateImage = "rootfs_subimg.1";
        m_requests = {"rootfs_subimg.0", "rootfs_subimg.1"};
        if (sizeHandshake) {
            m_sizeRequestImageFilename = "07_IMAGE";
            m_images.emplace_back("07_IMAGE", ASTRA_IMAGE_TYPE_UPDATE_EMMC);
            m_requests = {"rootfs_subimg.0", "07_IMAGE", "rootfs_subimg.1", "07_IMAGE"};
        }
        SetStatusCallback([this](AstraDeviceManagerResponse response) {
            m_events.push_back(response.GetDeviceResponse());
        });
    }

    int Boot(std::shared_ptr<AstraBootImage>, AstraDeviceBootStage) override { return 0; }
    int Update(std::shared_ptr<FlashImage>) override { return 0; }
    int WaitForCompletion() override { return 0; }
    int SendToConsole(const std::string &) override { return 0; }
    int ReceiveFromConsole(std::string &) override { return 0; }

    void Run()
    {
        StartImageRequestThread();
        m_imageRequestThread.join();
    }

    const std::vector<DeviceResponse> &Events() const { return m_events; }
    AstraDeviceStatus LifecycleStatus() const { return m_status.load(); }

protected:
    bool WaitForImageRequest(std::string &name, uint8_t &,
        std::chrono::milliseconds) override
    {
        if (m_requestIndex == m_requests.size()) {
            m_running.store(false);
            return false;
        }
        name = m_requests[m_requestIndex++];
        return true;
    }

    int SendImagePayload(Image &) override { return m_failTransfer ? -1 : 0; }

    bool ShouldSuppressImageStatus(const std::string &name) override
    {
        return name == m_sizeRequestImageFilename;
    }

private:
    bool m_failTransfer;
    size_t m_requestIndex = 0;
    std::vector<std::string> m_requests;
    std::vector<DeviceResponse> m_events;
};

static void Require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void CheckUpdateEvents(bool sizeHandshake,
    AstraDeviceStatus initialStatus = ASTRA_DEVICE_STATUS_UPDATE_START)
{
    EventTestDevice device(sizeHandshake, false, false, initialStatus);
    device.Run();
    const auto &events = device.Events();
    const size_t startOffset = initialStatus == ASTRA_DEVICE_STATUS_UPDATE_START ? 0 : 1;
    Require(events.size() == 6 + startOffset, "Expected START, COMPLETE, WAIT for each visible image");
    if (startOffset != 0) {
        Require(events.front().m_status == ASTRA_DEVICE_STATUS_UPDATE_START, "Missing resumed update start");
    }
    for (size_t imageIndex = 0; imageIndex < 2; ++imageIndex) {
        const auto offset = startOffset + imageIndex * 3;
        Require(events[offset].m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_START, "Missing transfer start");
        Require(events[offset + 1].m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, "Missing transfer completion");
        const auto &wait = events[offset + 2];
        Require(wait.m_status == ASTRA_DEVICE_STATUS_WAITING_FOR_DEVICE, "Missing device wait");
        Require(wait.m_imageName == events[offset].m_imageName, "Wait must identify its image");
        Require(wait.m_deviceName == "test-device", "Wait must identify its device");
        Require(wait.m_message.find(wait.m_imageName) != std::string::npos, "Wait must explain its reason");
    }
    Require(device.LifecycleStatus() == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE, "Wait must not replace lifecycle state");
}

int main()
{
    try {
        CheckUpdateEvents(true);
        CheckUpdateEvents(false);
        for (const auto initialStatus : {ASTRA_DEVICE_STATUS_BOOT_START,
                 ASTRA_DEVICE_STATUS_BOOT_PROGRESS, ASTRA_DEVICE_STATUS_BOOT_COMPLETE}) {
            CheckUpdateEvents(true, initialStatus);
            CheckUpdateEvents(false, initialStatus);
        }
        EventTestDevice bootDevice(false, true, false);
        bootDevice.Run();
        for (const auto &event : bootDevice.Events()) {
            Require(event.m_status != ASTRA_DEVICE_STATUS_WAITING_FOR_DEVICE, "Boot transfer must not report a flash wait");
        }
        EventTestDevice failedDevice(false, false, true);
        failedDevice.Run();
        Require(failedDevice.Events().size() == 2, "Failed transfer must not report a wait");
        Require(failedDevice.Events().back().m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL, "Missing update failure");
        Require(AstraDevice::AstraDeviceStatusToString(ASTRA_DEVICE_STATUS_WAITING_FOR_DEVICE) ==
            "ASTRA_DEVICE_STATUS_WAITING_FOR_DEVICE", "Missing wait status name");
        std::cout << "Device event tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}