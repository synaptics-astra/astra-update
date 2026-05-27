// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "astra_device_impl_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "astra_boot_image.hpp"
#include "fastboot_device.hpp"
#include "usb_cdc_device.hpp"

namespace {

constexpr uint8_t kHostSync1 = 0x5B;
constexpr uint8_t kHostSync2 = 0x5A;
constexpr size_t kHostHeaderSize = 8;
constexpr size_t kOpHeaderSize = 32;

constexpr uint8_t kServiceIdBoot = 0x33;
constexpr uint8_t kHostApiServiceId = 0x0D;

constexpr uint8_t kOpcodeVersion = 0x0A;
constexpr uint8_t kOpcodeRunImage = 0x0B;
constexpr uint8_t kOpcodeExec0C = 0x0C;
constexpr uint8_t kOpcodeUpload = 0x12;

constexpr uint8_t kOpcodeUploadKey = 0x01;
constexpr uint8_t kOpcodeUploadSpk = 0x02;
constexpr uint8_t kOpcodeUploadM52Bl = 0x04;

constexpr uint8_t kHostApiOpcodeGeneric = 0x12;
constexpr uint8_t kHostApiOpcodeVersion = 0x0A;
constexpr uint8_t kHostApiOpcodeExec = 0x0C;

constexpr uint32_t kAddrSmLoad = 0xB4A00000;
constexpr uint32_t kAddrAcLoad = 0xBA100000;

constexpr uint32_t kImgTypeBl = 0x00020017;
constexpr uint32_t kImgTypeSm = 0x00000012;
constexpr uint32_t kImgTypeOptee = 0x00020014;

constexpr size_t kStreamChunkSize = 3 * 1024 * 1024;

enum M52BLReturnCode {
    M52BL_RC_SUCCESS             = 0x00000000,
    M52BL_RC_FAILURE             = 0x00000001,
    M52BL_RC_INVALID_HEADER      = 0x00000002,
    M52BL_RC_MEMORY_OUT_OF_RANGE = 0x00000003,
    M52BL_RC_RX_INCOMPLETE       = 0x00000004,
    M52BL_RC_VERIFY_FAILED       = 0x00000005,
    M52BL_RC_LOAD_ON_MEM_FAILED  = 0x00000006,
    M52BL_RC_INVALID_OPCODE      = 0x00000007,
};

enum class SL26XXDeviceMode {
    SL26XX_DEVICE_MODE_UNKNOWN,
    SL26XX_DEVICE_MODE_BOOTROM,
    SL26XX_DEVICE_MODE_M52BL,
    SL26XX_DEVICE_MODE_SYSMGR,
    SL26XX_DEVICE_MODE_FASTBOOT,
};

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void AppendU32LE(std::vector<uint8_t> &buffer, uint32_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint32_t ReadU32LE(const uint8_t *ptr)
{
    return static_cast<uint32_t>(ptr[0]) |
        (static_cast<uint32_t>(ptr[1]) << 8) |
        (static_cast<uint32_t>(ptr[2]) << 16) |
        (static_cast<uint32_t>(ptr[3]) << 24);
}

// Returns true if s looks like the 32-char hex UUID we write into uEnv.txt.
bool IsAstraUuid(const std::string &s)
{
    if (s.size() != 32) {
        return false;
    }
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}


std::string VersionToString(uint32_t version)
{
    const uint32_t major = (version >> 16) & 0xFFFFU;
    const uint32_t minor = version & 0xFFFFU;

    std::ostringstream out;
    out << major << "." << minor;
    return out.str();
}

std::string DeviceModeToString(SL26XXDeviceMode mode)
{
    switch (mode) {
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_BOOTROM:
        return "bootrom";
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_M52BL:
        return "m52bl";
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_SYSMGR:
        return "sysmgr";
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_FASTBOOT:
        return "fastboot";
    default:
        return "unknown";
    }
}

} // namespace

class AstraDeviceSL26XXImpl final : public AstraDeviceImpl {
public:
    AstraDeviceSL26XXImpl(std::unique_ptr<USBDevice> device, const std::string &tempDir,
        bool bootOnly, const std::string &bootCommand)
        : AstraDeviceImpl(std::move(device), tempDir, bootOnly, bootCommand)
    {}

    ~AstraDeviceSL26XXImpl() override
    {
        Close();
    }

    int Boot(std::shared_ptr<AstraBootImage> bootImage, AstraDeviceBootStage bootStage) override
    {
        ASTRA_LOG;

        if (bootImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing boot image for SL26XX update boot flow" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Missing boot image");
            return -1;
        }

        if (m_usbDevice == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing USB device" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Missing USB device");
            return -1;
        }

        if (m_deviceOpened) {
            return 0;
        }

        int ret = m_usbDevice->Open(std::bind(&AstraDeviceSL26XXImpl::USBEventHandler, this,
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open SL26XX device" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to open device");
            return ret;
        }

        m_deviceOpened = true;
        m_deviceName = "device:" + m_usbDevice->GetUSBPath();
        m_status = ASTRA_DEVICE_STATUS_OPENED;

        m_status = ASTRA_DEVICE_STATUS_BOOT_START;
        ReportStatus(ASTRA_DEVICE_STATUS_BOOT_START, 0, "", "Starting SL26XX boot");

        {
            std::lock_guard<std::mutex> lock(m_rxMutex);
            m_deviceDisconnected = false;
            m_rxBuffer.clear();
        }
        m_expectResetDisconnect = false;

        if (bootStage == ASTRA_DEVICE_BOOT_STAGE_AUTO) {
            // SL26XX boots through sysmgr to U-Boot; default to the bootloader stage
            // for both boot-only and update modes.
            bootStage = ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER;
        }

        ret = m_usbDevice->EnableInterrupts();
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to enable interrupts for SL26XX" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to enable interrupts");
            return ret;
        }

        const uint16_t devVid = m_usbDevice->GetVendorId();
        const uint16_t devPid = m_usbDevice->GetProductId();
        const uint8_t numInterfaces = m_usbDevice->GetNumInterfaces();
        const uint16_t sysMgrVid = bootImage->GetSysMgrVendorId();
        const uint16_t sysMgrPid = bootImage->GetSysMgrProductId();
        const uint16_t fastbootVid = bootImage->GetFastbootVendorId();
        const uint16_t fastbootPid = bootImage->GetFastbootProductId();

        SL26XXDeviceMode resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_UNKNOWN;
        if (fastbootVid != 0 && devVid == fastbootVid && devPid == fastbootPid) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_FASTBOOT;
        } else if (sysMgrVid != 0 && devVid == sysMgrVid && devPid == sysMgrPid) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_SYSMGR;
        } else if (numInterfaces > 1) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_BOOTROM;
        } else if (numInterfaces == 1) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_M52BL;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX mode resolved: " << DeviceModeToString(resolvedMode)
                                  << " numInterfaces=" << static_cast<int>(numInterfaces)
                                  << " vid=0x" << std::hex << devVid << " pid=0x" << devPid << std::dec << endLog;

        if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_BOOTROM) {
            if (!RunSpkBootSequence(*bootImage)) {
                m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to run SL26XX key/spk/m52bl boot sequence");
                return -1;
            }

            // M52BL has been loaded. Device will disconnect and re-enumerate now.
            m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
            return 1;
        } else if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_M52BL) {
            uint32_t blVersion = 0;
            if (GetBootloaderVersion(blVersion)) {
                log(ASTRA_LOG_LEVEL_INFO) << "SL26XX M52BL version: " << VersionToString(blVersion)
                                          << " (0x" << std::hex << std::uppercase << blVersion
                                          << std::dec << ")" << endLog;
            } else {
                log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX M52BL version query failed" << endLog;
            }

            if (bootStage == ASTRA_DEVICE_BOOT_STAGE_M52BL) {
                // Target stage reached: device is already in M52BL.
                m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE, 100, "", "Device already in M52BL");
                return 0;
            }

            if (!RunSmBootSequence(*bootImage)) {
                m_expectResetDisconnect = false;
                m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to run SL26XX SysMgr boot sequence");
                return -1;
            }

            // SysMgr has been loaded. Device will disconnect and re-enumerate now.
            m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
            return 1;
        } else if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_SYSMGR) {
            // Device is already in SysMgr — no boot sequence needed.
            uint32_t smVersion = 0;
            if (GetSysMgrVersion(smVersion)) {
                log(ASTRA_LOG_LEVEL_INFO) << "SL26XX SysMgr version: " << VersionToString(smVersion)
                                          << " (0x" << std::hex << std::uppercase << smVersion
                                          << std::dec << ")" << endLog;
            } else {
                log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX SysMgr version query failed" << endLog;
            }

            if (bootStage == ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER) {
                if (!RunAcoreSequence(*bootImage)) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                    ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to run SL26XX A-Core boot sequence");
                    return -1;
                }
                if (fastbootVid != 0) {
                    // U-Boot will re-enumerate as a fastboot device and request uEnv.txt.
                    // BOOT_COMPLETE is reported from the image loop once uEnv.txt is served.
                    m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
                    return 1;
                }
                // No fastboot device expected — boot is complete.
                m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE, 100, "", "SL26XX A-Core bootloader sequence complete");
                return 0;
            }

            // SYSMGR explicitly requested — SysMgr is the terminal stage.
            m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE, 100, "", "Device already in SysMgr");
            return 0;
        } else if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_FASTBOOT) {
            // Device is in fastboot mode; open the fastboot transport and start the
            // shared image-serving loop.  Boot and update images are served from the
            // same loop — Update() simply appends flash images while the loop runs.
            m_fastbootDevice = std::make_unique<FastBootDevice>(m_usbDevice.get());
            if (!m_fastbootDevice->Open([this]() {
                    // Disconnect during an active session: only stop the image loop if
                    // we are NOT in rebind-mode (rebind-mode waits for a reconnect).
                    if (!m_rebindArmed.load()) {
                        m_running.store(false);
                        m_deviceEventCV.notify_all();
                    }
                })) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open fastboot device" << endLog;
                m_fastbootDevice.reset();
                m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to open fastboot device");
                return -1;
            }

            // If the device already carries our UUID as its serial# (i.e. it
            // has booted at least once from a uEnv.txt we wrote), arm rebind-mode
            // so the image-serving loop survives the fb_exit disconnects.
            {
                std::string serialno;
                if (m_fastbootDevice->GetVar("serialno", serialno) && IsAstraUuid(serialno)) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: UUID serial '" << serialno
                        << "' found, arming rebind-mode" << endLog;
                    m_updateSessionUuid = serialno;
                    m_rebindArmed.store(true);
                    if (m_registerFastbootSerial) {
                        m_registerFastbootSerial(serialno);
                    }
                }
            }

            m_deviceDir = m_tempDir;
            BuildBootImageList(bootImage, bootStage);
            m_status = ASTRA_DEVICE_STATUS_BOOT_START;
            StartImageRequestThread();
            return 0;
        }

        m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
        ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Unable to resolve SL26XX device mode");
        return -1;
    }

    int Update(std::shared_ptr<FlashImage> flashImage) override
    {
        ASTRA_LOG;

        if (flashImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing flash image for SL26XX fastboot update" << endLog;
            return -1;
        }

        // Append flash images to the shared list; the running loop will serve them.
        AppendUpdateImages(flashImage);
        return 0;
    }

    int WaitForCompletion() override
    {
        ASTRA_LOG;

        if (m_imageRequestThread.joinable()) {
            // Fastboot mode: wait for the shared image-serving loop to finish.
            std::unique_lock<std::mutex> lock(m_deviceEventMutex);
            m_deviceEventCV.wait(lock, [this] { return !m_running.load(); });

            if (m_bootOnly) {
                if (m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE) {
                    ReportStatus(m_status, 100, "", "Success");
                }
            } else {
                if (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                    ReportStatus(m_status, 100, "", "Success");
                }
            }
        }

        return (m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL ||
                m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL) ? -1 : 0;
    }

    int SendToConsole(const std::string &data) override
    {
        ASTRA_LOG;
        (void)data;

        log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX console transport is not implemented" << endLog;
        return -1;
    }

    int ReceiveFromConsole(std::string &data) override
    {
        ASTRA_LOG;
        data.clear();

        log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX console transport is not implemented" << endLog;
        return -1;
    }

    void Close() override
    {
        ASTRA_LOG;

        {
            std::lock_guard<std::mutex> lock(m_closeMutex);
            if (m_shutdown.exchange(true)) {
                return;
            }
        }

        // Mark transport as disconnected so any blocked rx waiters wake.
        // (WakeImageRequestThread() below also notifies m_rxCV, but the
        // m_deviceDisconnected flag is what their predicate checks.)
        {
            std::lock_guard<std::mutex> rxLock(m_rxMutex);
            m_deviceDisconnected = true;
            m_rxBuffer.clear();
            m_rxCV.notify_all();
        }
        m_expectResetDisconnect = false;

        // Unregister from the manager's fastboot-serial registry.
        if (!m_updateSessionUuid.empty() && m_unregisterFastbootSerial) {
            m_unregisterFastbootSerial(m_updateSessionUuid);
        }

        // Wake the image-request loop (m_rebindCV via WakeImageRequestThread,
        // m_deviceEventCV from the helper) and join it before tearing down
        // m_fastbootDevice / m_usbDevice.  The loop may still be calling into
        // m_fastbootDevice until it observes m_running=false and exits.
        StopImageRequestThread();

        // Close the underlying USB device (stops and joins its callback
        // thread) BEFORE destroying FastBootDevice -- the callback thread
        // may still be draining its queue and calling
        // FastBootDevice::USBEventHandler; resetting m_fastbootDevice first
        // would leave that thread with a dangling 'this'.
        if (m_usbDevice != nullptr) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Closing USB device" << endLog;
            m_usbDevice->Close();
        }
        m_status = ASTRA_DEVICE_STATUS_CLOSED;

        m_fastbootDevice.reset();
        m_deviceOpened = false;
    }

private:
    std::unique_ptr<FastBootDevice> m_fastbootDevice;

    bool m_deviceOpened = false;

    // Rebind-mode state: armed once the device carries our UUID as serialno.
    // m_rebindReady is set by Rebind() and cleared by WaitForRebind().
    // m_fbExitPending is set after sending fb_exit so that WaitForImageRequest
    // skips all USB communication until the disconnect actually occurs.
    std::atomic<bool> m_rebindArmed{false};
    std::atomic<bool> m_rebindReady{false};
    std::atomic<bool> m_fbExitPending{false};
    std::mutex m_rebindMutex;
    std::condition_variable m_rebindCV;

    std::mutex m_rxMutex;
    std::condition_variable m_rxCV;
    std::deque<uint8_t> m_rxBuffer;
    bool m_deviceDisconnected = false;
    std::atomic<bool> m_expectResetDisconnect{false};

    void USBEventHandler(USBDevice::USBEvent event, uint8_t *buf, size_t size)
    {
        if (event == USBDevice::USB_DEVICE_EVENT_INTERRUPT) {
            if (buf != nullptr && size > 0) {
                std::lock_guard<std::mutex> lock(m_rxMutex);
                for (size_t i = 0; i < size; ++i) {
                    m_rxBuffer.push_back(buf[i]);
                }
                m_rxCV.notify_all();
            }
            return;
        }

        if (event == USBDevice::USB_DEVICE_EVENT_NO_DEVICE ||
            event == USBDevice::USB_DEVICE_EVENT_TRANSFER_CANCELED ||
            event == USBDevice::USB_DEVICE_EVENT_TRANSFER_ERROR)
        {
            {
                std::lock_guard<std::mutex> lock(m_rxMutex);
                m_deviceDisconnected = true;
                m_rxCV.notify_all();
            }

            const bool suppressDisconnectFailure = m_expectResetDisconnect.load();

            if (m_status != ASTRA_DEVICE_STATUS_BOOT_COMPLETE &&
                m_status != ASTRA_DEVICE_STATUS_UPDATE_COMPLETE)
            {
                if (suppressDisconnectFailure &&
                    (m_status == ASTRA_DEVICE_STATUS_BOOT_START || m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS))
                {
                    return;
                }

                if (m_status == ASTRA_DEVICE_STATUS_BOOT_START || m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                } else {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
                }

                ReportStatus(m_status, 0, "", "Device disconnected");
            }
        }
    }

    void ClearRxBuffer()
    {
        std::lock_guard<std::mutex> lock(m_rxMutex);
        m_rxBuffer.clear();
    }

    bool WaitForDeviceDisconnect(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_rxMutex);
        return m_rxCV.wait_for(lock, timeout, [this]() {
            return m_deviceDisconnected;
        });
    }

    bool ReadExactBytes(size_t bytesToRead, std::vector<uint8_t> &out,
        std::chrono::milliseconds timeout)
    {
        out.clear();

        std::unique_lock<std::mutex> lock(m_rxMutex);
        const bool ready = m_rxCV.wait_for(lock, timeout, [this, bytesToRead]() {
            return m_rxBuffer.size() >= bytesToRead || m_deviceDisconnected;
        });

        if (!ready || m_rxBuffer.size() < bytesToRead) {
            return false;
        }

        out.reserve(bytesToRead);
        for (size_t i = 0; i < bytesToRead; ++i) {
            out.push_back(m_rxBuffer.front());
            m_rxBuffer.pop_front();
        }

        return true;
    }

    bool WriteAll(const uint8_t *data, size_t size)
    {
        USBDevice *usbDevice = m_usbDevice.get();
        if (usbDevice == nullptr) {
            return false;
        }

        size_t offset = 0;
        while (offset < size) {
            int transferred = 0;
            const int ret = usbDevice->Write(const_cast<uint8_t *>(data + offset), size - offset, &transferred);
            if (ret < 0 || transferred <= 0) {
                return false;
            }

            offset += static_cast<size_t>(transferred);
        }

        return true;
    }

    int ReadResponseCode(bool rawMode, std::chrono::milliseconds timeout)
    {
        ASTRA_LOG;

        std::vector<uint8_t> responseHeader;
        if (!ReadExactBytes(kHostHeaderSize, responseHeader, timeout)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Timed out waiting for protocol response header" << endLog;
            return -1;
        }

        if (responseHeader[0] != kHostSync1 || responseHeader[1] != kHostSync2) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Invalid protocol sync bytes in response header" << endLog;
            return -1;
        }

        if (rawMode) {
            return static_cast<int>(ReadU32LE(&responseHeader[4]));
        }

        const uint32_t payloadSize = ReadU32LE(&responseHeader[4]);
        if (payloadSize == 0) {
            return 0;
        }

        if (payloadSize > (4U * 1024U * 1024U)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Protocol response payload too large: " << payloadSize << endLog;
            return -1;
        }

        std::vector<uint8_t> payload;
        if (!ReadExactBytes(payloadSize, payload, timeout)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Timed out waiting for protocol response payload" << endLog;
            return -1;
        }

        if (payload.size() < sizeof(uint32_t)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Protocol response payload too small" << endLog;
            return -1;
        }

        return static_cast<int>(ReadU32LE(payload.data()));
    }

    int SendPacket(uint8_t serviceId, uint8_t opcode, const std::vector<uint8_t> &payload,
        uint8_t hostOpcode, uint32_t addr, uint32_t imageType, bool isLast,
        std::chrono::milliseconds timeout, std::optional<uint32_t> numWordsOverride = std::nullopt,
        bool rawMode = false, bool waitForResponse = true)
    {
        ASTRA_LOG;

        std::vector<uint8_t> paddedPayload = payload;
        const size_t padding = (4 - (paddedPayload.size() % 4)) % 4;
        if (padding > 0) {
            paddedPayload.insert(paddedPayload.end(), padding, 0xFF);
        }

        uint32_t numWords = 0;
        if (numWordsOverride.has_value()) {
            numWords = *numWordsOverride;
        } else {
            numWords = static_cast<uint32_t>(paddedPayload.size() / 4);
        }

        std::vector<uint8_t> innerPacket;
        innerPacket.reserve(kOpHeaderSize + paddedPayload.size());
        innerPacket.push_back(kHostSync1);
        innerPacket.push_back(kHostSync2);
        innerPacket.push_back(serviceId);
        innerPacket.push_back(opcode);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, numWords);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, addr);
        AppendU32LE(innerPacket, imageType);
        AppendU32LE(innerPacket, isLast ? 1U : 0U);
        AppendU32LE(innerPacket, 0);
        innerPacket.insert(innerPacket.end(), paddedPayload.begin(), paddedPayload.end());

        std::vector<uint8_t> finalPacket;
        if (rawMode) {
            finalPacket = std::move(innerPacket);
        } else {
            finalPacket.reserve(kHostHeaderSize + innerPacket.size());
            finalPacket.push_back(kHostSync1);
            finalPacket.push_back(kHostSync2);
            finalPacket.push_back(kHostApiServiceId);
            finalPacket.push_back(hostOpcode);
            AppendU32LE(finalPacket, static_cast<uint32_t>(innerPacket.size()));
            finalPacket.insert(finalPacket.end(), innerPacket.begin(), innerPacket.end());
        }

        ClearRxBuffer();

        if (!WriteAll(finalPacket.data(), finalPacket.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write protocol packet" << endLog;
            return -1;
        }

        if (!waitForResponse) {
            return 0;
        }

        return ReadResponseCode(rawMode, timeout);
    }

    int SendSpkCommand(uint8_t opcode, const uint8_t *payload, uint32_t payloadSize,
        std::chrono::milliseconds timeout)
    {
        ASTRA_LOG;

        std::vector<uint8_t> packet;
        packet.reserve(kOpHeaderSize + payloadSize);
        packet.push_back(kHostSync1);
        packet.push_back(kHostSync2);
        packet.push_back(kServiceIdBoot);
        packet.push_back(opcode);
        AppendU32LE(packet, payloadSize);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);

        if (payloadSize > 0 && payload != nullptr) {
            packet.insert(packet.end(), payload, payload + payloadSize);
        }

        ClearRxBuffer();

        if (!WriteAll(packet.data(), packet.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write SPK bootstrap packet, opcode=0x"
                << std::hex << std::uppercase << static_cast<uint32_t>(opcode) << std::dec << endLog;
            return -1;
        }

        return ReadResponseCode(true, timeout);
    }

    bool GetBootloaderVersion(uint32_t &version)
    {
        ASTRA_LOG;

        version = 0;

        std::vector<uint8_t> packet;
        packet.reserve(kOpHeaderSize);
        packet.push_back(kHostSync1);
        packet.push_back(kHostSync2);
        packet.push_back(kServiceIdBoot);
        packet.push_back(kOpcodeVersion);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);

        ClearRxBuffer();

        if (!WriteAll(packet.data(), packet.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send SL26XX BL version request" << endLog;
            return false;
        }

        std::vector<uint8_t> responseHeader;
        if (!ReadExactBytes(kHostHeaderSize, responseHeader, std::chrono::seconds(2))) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Timed out waiting for SL26XX BL version response" << endLog;
            return false;
        }

        if (responseHeader[0] != kHostSync1 || responseHeader[1] != kHostSync2) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Invalid sync bytes in SL26XX BL version response" << endLog;
            return false;
        }

        // BL VERSION typically encodes version in response header bytes [4..7].
        version = ReadU32LE(&responseHeader[4]);

        // Some ROM variants append an extra 4-byte word after the response header.
        std::vector<uint8_t> trailing;
        (void)ReadExactBytes(sizeof(uint32_t), trailing, std::chrono::milliseconds(50));

        return true;
    }

    bool GetSysMgrVersion(uint32_t &version)
    {
        ASTRA_LOG;

        version = 0;

        const int rc = SendPacket(kServiceIdBoot, kOpcodeVersion, {}, kHostApiOpcodeVersion,
                                  0, 0, false, std::chrono::seconds(2));
        if (rc < 0) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Failed to query SL26XX SysMgr version" << endLog;
            return false;
        }

        version = static_cast<uint32_t>(rc);
        return true;
    }

    bool SendSpkFile(const std::filesystem::path &path, const std::string &imageName, uint8_t opcode)
    {
        ASTRA_LOG;

        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open SPK bootstrap file: " << path.string() << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Failed to open image");
            return false;
        }

        const std::streamoff endPos = input.tellg();
        if (endPos < 0 || static_cast<uint64_t>(endPos) > std::numeric_limits<uint32_t>::max()) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Invalid SPK bootstrap file size for " << imageName << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Invalid image size");
            return false;
        }

        const uint32_t size = static_cast<uint32_t>(endPos);
        input.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer;
        buffer.resize(size);
        if (size > 0) {
            input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
            if (input.gcount() != static_cast<std::streamsize>(size)) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to read SPK bootstrap image: " << path.string() << endLog;
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Failed to read image");
                return false;
            }
        }

        ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, imageName);
        const int rc = SendSpkCommand(opcode, buffer.data(), size, std::chrono::seconds(5));
        if (rc != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SPK bootstrap upload failed for " << imageName << ", rc=" << rc << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Bootstrap upload failed");
            return false;
        }

        ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, imageName);
        return true;
    }

    const Image *FindKeyBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "key.bin"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("key") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    const Image *FindSpkBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "spk.bin"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("spk") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    const Image *FindM52BlBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "m52bl.bin"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("m52") != std::string::npos && imageName.find("bl") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    bool RunSpkBootSequence(const AstraBootImage &bootImage)
    {
        ASTRA_LOG;

        const Image *keyImage = FindKeyBootImage(bootImage);
        const Image *spkImage = FindSpkBootImage(bootImage);
        const Image *m52BlImage = FindM52BlBootImage(bootImage);

        if (keyImage == nullptr || spkImage == nullptr || m52BlImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing key/spk/m52bl image in boot image set" << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX run-spk: uploading " << keyImage->GetName() << ", "
                                  << spkImage->GetName() << ", and " << m52BlImage->GetName() << endLog;

        if (!SendSpkFile(keyImage->GetPath(), keyImage->GetName(), kOpcodeUploadKey)) {
            return false;
        }

        if (!SendSpkFile(spkImage->GetPath(), spkImage->GetName(), kOpcodeUploadSpk)) {
            return false;
        }

        if (!SendSpkFile(m52BlImage->GetPath(), m52BlImage->GetName(), kOpcodeUploadM52Bl)) {
            return false;
        }

        return true;
    }

    const Image *FindSysMgrBootImage(const AstraBootImage &bootImage)
    {
        // Match the same primary filename used by usb_boot_tool.py --op run-sm --sm sysmgr.subimg.
        static const std::array<const char *, 1> kPreferredSysMgrNames = {
            "sysmgr.subimg"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredSysMgrNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("sysmgr") != std::string::npos)
            {
                return &image;
            }
        }

        return nullptr;
    }

    bool RunSmBootSequence(const AstraBootImage &bootImage)
    {
        ASTRA_LOG;

        const Image *sysMgrImage = FindSysMgrBootImage(bootImage);
        if (sysMgrImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Unable to find SysMgr image in boot image set" << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX run-sm: uploading " << sysMgrImage->GetName()
                                  << " to 0x" << std::hex << std::uppercase << kAddrSmLoad
                                  << " and sending Run Image command" << std::dec << endLog;

        if (!UploadFile(sysMgrImage->GetPath(), sysMgrImage->GetName(), kImgTypeSm, kAddrSmLoad, true)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to upload SysMgr image: " << sysMgrImage->GetName() << endLog;
            return false;
        }

        m_expectResetDisconnect = true;

        const int runRc = SendPacket(kServiceIdBoot, kOpcodeRunImage, {}, kHostApiOpcodeGeneric,
            kAddrSmLoad, 0, false, std::chrono::seconds(5), std::nullopt, true, false);
        if (runRc != 0) {
            m_expectResetDisconnect = false;
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send Run Image command, rc=" << runRc << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "Run Image command sent, waiting for device to re-enumerate as SysMgr" << endLog;
        return true;
    }

    const Image *FindBlBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "bl.subimg"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("bl") != std::string::npos &&
                imageName.find("m52") == std::string::npos &&
                imageName.find("spk") == std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    const Image *FindTzkBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "tzk.subimg"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("tzk") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    bool RunAcoreSequence(const AstraBootImage &bootImage)
    {
        ASTRA_LOG;

        const Image *blImage = FindBlBootImage(bootImage);
        const Image *tzkImage = FindTzkBootImage(bootImage);

        if (blImage == nullptr || tzkImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing BL or TZK image for A-Core sequence" << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX run-acore: uploading " << blImage->GetName()
                                  << " and " << tzkImage->GetName() << endLog;

        if (!UploadFile(blImage->GetPath(), blImage->GetName(), kImgTypeBl)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to upload BL image: " << blImage->GetName() << endLog;
            return false;
        }

        const int execRc1 = SendPacket(kServiceIdBoot, kOpcodeExec0C, {}, kHostApiOpcodeExec,
            0, 0, false, std::chrono::seconds(5));
        if (execRc1 != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "BL exec command failed, rc=" << execRc1 << endLog;
            return false;
        }

        if (!UploadFile(tzkImage->GetPath(), tzkImage->GetName(), kImgTypeOptee)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to upload TZK image: " << tzkImage->GetName() << endLog;
            return false;
        }

        // A-Core takes control immediately after the TZK exec — SysMgr never sends a response.
        m_expectResetDisconnect = true;
        const int execRc2 = SendPacket(kServiceIdBoot, kOpcodeExec0C, {}, kHostApiOpcodeExec,
            0, 0, false, std::chrono::seconds(5), std::nullopt, false, false);
        if (execRc2 != 0) {
            m_expectResetDisconnect = false;
            log(ASTRA_LOG_LEVEL_ERROR) << "TZK exec command failed, rc=" << execRc2 << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX A-Core sequence complete, waiting for fastboot re-enumeration" << endLog;
        return true;
    }

    bool UploadData(const uint8_t *data, uint64_t size, const std::string &imageName,
        uint32_t imageType, uint32_t loadAddress = kAddrAcLoad, bool rawMode = false,
        bool reportStatus = true, uint64_t totalSize = 0, uint64_t byteOffset = 0)
    {
        ASTRA_LOG;

        if (size > std::numeric_limits<uint32_t>::max()) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Upload size too large for protocol: " << size << endLog;
            if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Image too large");
            return false;
        }

        if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, imageName);

        const int setupRc = SendPacket(kServiceIdBoot, kOpcodeUpload, {}, kHostApiOpcodeGeneric,
            loadAddress, imageType, false, std::chrono::seconds(5), static_cast<uint32_t>(size), rawMode);
        if (setupRc != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX upload setup failed for " << imageName << ", rc=" << setupRc << endLog;
            if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Upload setup failed");
            return false;
        }

        // When totalSize is provided we always emit progress scaled to the whole image,
        // regardless of reportStatus (which only gates the start/complete events).
        const uint64_t scaleSize = (totalSize > 0) ? totalSize : size;
        const bool emitProgress = reportStatus || (totalSize > 0);

        uint64_t sent = 0;
        while (sent < size) {
            const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(kStreamChunkSize, size - sent));
            if (!WriteAll(data + sent, chunkSize)) {
                log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX upload write failed for " << imageName << endLog;
                if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Upload write failed");
                return false;
            }

            sent += chunkSize;
            if (emitProgress) {
                const double progress = (scaleSize == 0) ? 100.0
                    : (static_cast<double>(byteOffset + sent) / static_cast<double>(scaleSize)) * 100.0;
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS, progress, imageName);
            }
        }

        const int verifyRc = ReadResponseCode(rawMode, std::chrono::seconds(20));
        if (verifyRc != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX upload verification failed for " << imageName << ", rc=" << verifyRc << endLog;
            if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Upload verify failed");
            return false;
        }

        if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, imageName);
        return true;
    }

    bool UploadFile(const std::filesystem::path &path, const std::string &imageName, uint32_t imageType,
        uint32_t loadAddress = kAddrAcLoad, bool rawMode = false)
    {
        ASTRA_LOG;

        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open upload file: " << path.string() << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Failed to open image");
            return false;
        }

        const std::streamoff endPos = input.tellg();
        if (endPos < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to query file size: " << path.string() << endLog;
            return false;
        }

        const uint64_t size = static_cast<uint64_t>(endPos);
        input.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer;
        buffer.resize(static_cast<size_t>(size));
        if (size > 0) {
            input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
            if (input.gcount() != static_cast<std::streamsize>(size)) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to read file data: " << path.string() << endLog;
                return false;
            }
        }

        return UploadData(buffer.data(), size, imageName, imageType, loadAddress, rawMode);
    }

    bool UploadBuffer(const std::vector<uint8_t> &buffer, const std::string &imageName, uint32_t imageType,
        uint32_t loadAddress = kAddrAcLoad, bool rawMode = false, bool reportStatus = true,
        uint64_t totalSize = 0, uint64_t byteOffset = 0)
    {
        return UploadData(buffer.data(), buffer.size(), imageName, imageType, loadAddress, rawMode, reportStatus, totalSize, byteOffset);
    }

    // -----------------------------------------------------------------------
    // Virtual hook: WakeImageRequestThread
    // Notify all CVs so a thread blocked in WaitForImageRequest, WaitForRebind,
    // or any rx-buffer wait wakes up promptly when m_running is cleared by
    // Close() / shutdown.
    // -----------------------------------------------------------------------
    void WakeImageRequestThread() override
    {
        m_rebindCV.notify_all();
        std::lock_guard<std::mutex> lock(m_rxMutex);
        m_rxCV.notify_all();
    }

    // -----------------------------------------------------------------------
    // Virtual hook: Rebind
    // Called by the manager when a fastboot device reconnects with a matching
    // UUID serial.  Swaps the USB device and wakes the waiting image loop.
    // -----------------------------------------------------------------------
    void Rebind(std::unique_ptr<USBDevice> newDevice) override
    {
        ASTRA_LOG;

        if (m_shutdown.load()) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX Rebind: impl already closed, ignoring" << endLog;
            return;
        }

        log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: rebinding to new USB device" << endLog;

        // Close the old USB device FIRST — this stops and joins the callback
        // thread (started by EnableInterrupts() / Open()).  The callback thread
        // holds a std::bind to FastBootDevice::USBEventHandler with 'this'
        // pointing at the current m_fastbootDevice; we must join the thread
        // before resetting m_fastbootDevice or we get a use-after-free.
        if (m_usbDevice) {
            m_usbDevice->Close();
        }

        // Now safe to tear down the old FastBootDevice wrapper.
        m_fastbootDevice.reset();

        // Take ownership of the new USB device.
        m_usbDevice = std::move(newDevice);

        // Reconstruct FastBootDevice over the new USB device.
        m_fastbootDevice = std::make_unique<FastBootDevice>(m_usbDevice.get());
        if (!m_fastbootDevice->Open([this]() {
                if (!m_rebindArmed.load()) {
                    m_running.store(false);
                    m_deviceEventCV.notify_all();
                }
            })) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX fastboot: failed to open new device after rebind" << endLog;
            m_fastbootDevice.reset();
            m_running.store(false);
            m_deviceEventCV.notify_all();
            m_rebindCV.notify_all();
            return;
        }

        // Signal WaitForImageRequest that a new device is ready.
        {
            std::lock_guard<std::mutex> lock(m_rebindMutex);
            m_rebindReady.store(true);
        }
        m_rebindCV.notify_all();
        log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: rebind complete" << endLog;
    }

    // -----------------------------------------------------------------------
    // WaitForRebind
    // Blocks until a new USB device is rebound (m_rebindReady), the impl is
    // shut down (m_running cleared), or the timeout elapses.
    // Returns true if a rebind arrived; false on timeout or shutdown.
    // -----------------------------------------------------------------------
    bool WaitForRebind()
    {
        ASTRA_LOG;

        constexpr auto kTimeout = std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(m_rebindMutex);
        bool ok = m_rebindCV.wait_for(lock, kTimeout,
            [this] { return m_rebindReady.load() || !m_running.load(); });

        if (!ok || !m_running.load()) {
            if (!ok) {
                log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX fastboot: timeout waiting for reconnect" << endLog;
            }
            return false;
        }
        m_rebindReady.store(false);
        log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: rebind received, resuming image loop" << endLog;
        return true;
    }

    // -----------------------------------------------------------------------
    // Virtual hook: WaitForImageRequest
    // Polls the SL26XX fastboot fb_command variable.  Blocks internally until
    // a "stage <filename>" command arrives, the device disconnects, m_running
    // is cleared, or the caller-supplied timeout elapses.
    // -----------------------------------------------------------------------
    bool WaitForImageRequest(std::string &name, uint8_t &imageType,
        std::chrono::milliseconds timeout) override
    {
        ASTRA_LOG;

        auto deadline = std::chrono::steady_clock::now() + timeout;

        while (m_running.load() && std::chrono::steady_clock::now() < deadline) {
            if (!m_fastbootDevice || m_fastbootDevice->IsDisconnected()) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot device disconnected" << endLog;
                m_fbExitPending.store(false);
                if (m_rebindArmed.load() && WaitForRebind()) {
                    // Rebind succeeded; reset the deadline and retry.
                    deadline = std::chrono::steady_clock::now() + timeout;
                    continue;
                }
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return false;
            }

            // fb_exit was sent.  In rebind-mode we cannot rely on
            // IsDisconnected() because bulk-only fastboot has no pending
            // USB transfers, so the no-device event is never fired and
            // m_disconnected is never set.  Go directly to WaitForRebind()
            // — but only while the update is still in progress.  Once
            // UPDATE_COMPLETE (or BOOT_COMPLETE in boot-only mode) is set,
            // the fb_exit is the final disconnect; disarm rebind and let
            // the loop exit cleanly.
            if (m_fbExitPending.load()) {
                if (m_rebindArmed.load()) {
                    const bool updateDone =
                        (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) ||
                        (m_bootOnly && m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE);
                    if (updateDone) {
                        // Final fb_exit — no more images to serve.
                        log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: update done, stopping after final fb_exit" << endLog;
                        m_rebindArmed.store(false);
                        m_fbExitPending.store(false);
                        m_running.store(false);
                        m_deviceEventCV.notify_all();
                        return false;
                    }
                    log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: fb_exit pending, waiting for rebind" << endLog;
                    m_fbExitPending.store(false);
                    if (WaitForRebind()) {
                        deadline = std::chrono::steady_clock::now() + timeout;
                        continue;
                    }
                    m_running.store(false);
                    m_deviceEventCV.notify_all();
                    return false;
                }
                // Non-rebind mode: fall through to the GetVar attempt below.
                // The transfer will fail immediately with NO_DEVICE once the
                // device disconnects after fb_exit, which sets m_running=false
                // and allows RunImageRequestLoop to exit cleanly without
                // reporting a spurious BOOT_FAIL.
                m_fbExitPending.store(false);
            }

            std::string fbCommand;
            if (!m_fastbootDevice->GetVar("fb_command", fbCommand)) {
                // GetVar failure means the USB connection dropped.
                if (m_rebindArmed.load()) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot: GetVar failed (rebind-mode), waiting for reconnect" << endLog;
                    if (WaitForRebind()) {
                        deadline = std::chrono::steady_clock::now() + timeout;
                        continue;
                    }
                } else if (m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "SL26XX fastboot disconnected after boot phase, waiting for reconnect" << endLog;
                } else {
                    log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX failed to get fb_command" << endLog;
                }
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return false;
            }

            if (!fbCommand.empty()) {
                // Parse "stage <filename>".
                std::istringstream iss(fbCommand);
                std::vector<std::string> parts;
                std::string token;
                while (iss >> token) {
                    parts.push_back(token);
                }

                if (parts.empty() || parts[0] != "stage" || parts.size() < 2) {
                    log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX unexpected fb_command: " << fbCommand << endLog;
                    m_running.store(false);
                    m_deviceEventCV.notify_all();
                    return false;
                }

                // Mirror SL16XX HandleInterrupt: transition to UPDATE when a request
                // arrives after boot completes.
                if (m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE && !m_bootOnly) {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_START;
                }

                name      = parts[1];
                imageType = 0; // SL26XX does not use interrupt-based image types
                log(ASTRA_LOG_LEVEL_INFO) << "SL26XX stage request: " << name << endLog;
                return true;
            }

            // fb_command is empty — nothing pending yet; retry after a short sleep.
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        return false;
    }

    // -----------------------------------------------------------------------
    // Virtual hook: SendImagePayload
    // Sends the requested file via fastboot StageFile.
    // START / COMPLETE / FAIL events are managed by RunImageRequestLoop.
    // -----------------------------------------------------------------------
    int SendImagePayload(Image &image) override
    {
        ASTRA_LOG;

        if (!m_fastbootDevice) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX fastboot device not available" << endLog;
            return -1;
        }

        const bool ok = m_fastbootDevice->StageFile(image.GetPath(),
            [this, &image](size_t sent, size_t total) {
                const double pct = (total > 0)
                    ? static_cast<double>(sent) / static_cast<double>(total) * 100.0
                    : 100.0;
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS, pct, image.GetName());
            });

        return ok ? 0 : -1;
    }

    // -----------------------------------------------------------------------
    // Virtual hook: OnImageSent
    // Notifies SL26XX firmware of the transfer result via OEM fastboot commands.
    // -----------------------------------------------------------------------
    void OnImageSent(const Image &image, bool success) override
    {
        (void)image;
        if (!m_fastbootDevice) {
            return;
        }
        m_fastbootDevice->Oem("run:setenv fb_ret " + std::string(success ? "OKAY" : "FAIL"));
        // Send fb_exit without waiting for a response: U-Boot exits its staging
        // loop and resets the USB connection before it can send OKAY back.
        // Set m_fbExitPending so WaitForImageRequest does not attempt any further
        // USB transfers until the disconnect is detected and (if in rebind-mode)
        // a new device instance arrives.
        m_fbExitPending.store(true);
        m_fastbootDevice->OemNoWait("run:setenv fb_exit 1");
    }
};

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL26XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand)
{
    return std::make_unique<AstraDeviceSL26XXImpl>(std::move(device), tempDir, bootOnly, bootCommand);
}

