// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "astra_device_impl_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "astra_boot_image.hpp"
#include "astra_console.hpp"
#include "image.hpp"
#include "utils.hpp"

class AstraDeviceSL16XXImpl final : public AstraDeviceImpl {
public:
    AstraDeviceSL16XXImpl(std::unique_ptr<USBDevice> device, const std::string &tempDir,
        bool bootOnly, const std::string &bootCommand)
        : AstraDeviceImpl(std::move(device), tempDir, bootOnly, bootCommand)
    {
        m_sizeRequestImageFilename = "07_IMAGE";
    }

    ~AstraDeviceSL16XXImpl() override
    {
        Close();
    }

    int Boot(std::shared_ptr<AstraBootImage> bootImage, AstraDeviceBootStage bootStage) override
    {
        ASTRA_LOG;

        if (bootImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing boot image" << endLog;
            return -1;
        }

        m_ubootConsole = bootImage->GetUbootConsole();
        m_requestedImageName.clear();
        m_imageRequestReady.store(false);

        int ret = m_usbDevice->Open(std::bind(&AstraDeviceSL16XXImpl::USBEventHandler, this,
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open device" << endLog;
            return ret;
        }

        m_deviceName = "device:" + m_usbDevice->GetUSBPath();
        log(ASTRA_LOG_LEVEL_INFO) << "Device name: " << m_deviceName << endLog;

        std::string modifiedDeviceName = m_deviceName;
        modifiedDeviceName.erase(
            std::remove(modifiedDeviceName.begin(), modifiedDeviceName.end(), ':'),
            modifiedDeviceName.end());
        std::replace(modifiedDeviceName.begin(), modifiedDeviceName.end(), '.', '_');

        m_deviceDir = m_tempDir + "/" + modifiedDeviceName;
        std::filesystem::create_directories(m_deviceDir);

        m_console = std::make_unique<AstraConsole>(modifiedDeviceName, m_deviceDir);

        std::ofstream imageFile(m_deviceDir + "/" + m_usbPathImageFilename);
        if (!imageFile) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open 06_IMAGE file" << endLog;
            return -1;
        }
        imageFile << m_usbDevice->GetUSBPath();
        imageFile.close();

        Image usbPathImage(m_deviceDir + "/" + m_usbPathImageFilename, ASTRA_IMAGE_TYPE_BOOT);
        m_sizeRequestImage = std::make_unique<Image>(
            m_deviceDir + "/" + m_sizeRequestImageFilename, ASTRA_IMAGE_TYPE_UPDATE_EMMC);

        m_status = ASTRA_DEVICE_STATUS_OPENED;

        // Build shared image list (boot subimages + uEnv.txt if needed).
        BuildBootImageList(bootImage, bootStage);

        // Append SL16XX-specific service images.
        {
            std::lock_guard<std::mutex> lock(m_imageMutex);
            m_images.push_back(usbPathImage);
            m_images.push_back(*m_sizeRequestImage);
        }

        m_status = ASTRA_DEVICE_STATUS_BOOT_START;

        // Start the shared image-request loop thread.
        StartImageRequestThread();

        ret = m_usbDevice->EnableInterrupts();
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to start device" << endLog;
            return ret;
        }

        return 0;
    }

    int Update(std::shared_ptr<FlashImage> flashImage) override
    {
        ASTRA_LOG;

        if (flashImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing flash image" << endLog;
            return -1;
        }

        // Append update images to the shared list; the running loop will serve them.
        AppendUpdateImages(flashImage);

        if (!m_uEnvSupport && m_ubootConsole == ASTRA_UBOOT_CONSOLE_USB) {
            if (m_console != nullptr && m_console->WaitForPrompt()) {
                SendToConsole(flashImage->GetFlashCommand() + "\n");
            }
        }

        return 0;
    }

    int WaitForCompletion() override
    {
        ASTRA_LOG;

        if (m_uEnvSupport || m_ubootConsole == ASTRA_UBOOT_CONSOLE_UART) {
            for (;;) {
                std::unique_lock<std::mutex> lock(m_deviceEventMutex);
                m_deviceEventCV.wait(lock);
                if (m_bootOnly) {
                    if (m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE) {
                        // Device successfully reset after boot.
                        ReportStatus(m_status, 100, "", "Success");
                    }
                } else {
                    if (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                        // Device successfully reset after update.
                        ReportStatus(m_status, 100, "", "Success");
                    }
                }

                if (!m_running.load()) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Device event received: shutting down" << endLog;
                    break;
                }
            }
        } else if (m_ubootConsole == ASTRA_UBOOT_CONSOLE_USB) {
            if (m_console != nullptr && m_console->WaitForPrompt()) {
                if (m_resetWhenComplete) {
                    SendToConsole("reset\n");
                }

                // Update does not require reset, but console is back at U-Boot prompt.
                if (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                    ReportStatus(m_status, 100, "", "Success");
                }
            }
        }

        return 0;
    }

    int SendToConsole(const std::string &data) override
    {
        ASTRA_LOG;

        int ret = m_usbDevice->WriteInterruptData(reinterpret_cast<const uint8_t *>(data.c_str()), data.size());
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send data to console" << endLog;
            return ret;
        }

        return 0;
    }

    int ReceiveFromConsole(std::string &data) override
    {
        ASTRA_LOG;

        if (m_console == nullptr) {
            return -1;
        }

        data = m_console->Get();
        return 0;
    }

    void Close() override
    {
        ASTRA_LOG;

        std::lock_guard<std::mutex> lock(m_sl16CloseMutex);
        if (m_sl16Shutdown.exchange(true)) {
            return;
        }

        // Wake WaitForImageRequest before joining.
        m_running.store(false);
        m_deviceEventCV.notify_all();
        m_imageRequestCV.notify_all();

        if (m_imageRequestThread.joinable()) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Joining image request thread" << endLog;
            m_imageRequestThread.join();
        }

        {
            std::lock_guard<std::mutex> imgLock(m_imageMutex);
            m_images.clear();
        }

        if (m_console != nullptr) {
            m_console->Shutdown();
        }

        AstraDeviceImpl::Close();
    }

private:
    // SL16XX-specific close guard.
    std::atomic<bool> m_sl16Shutdown{false};
    std::mutex m_sl16CloseMutex;

    // Interrupt / image-request signalling.
    std::condition_variable m_imageRequestCV;
    std::mutex m_imageRequestMutex;
    std::atomic<bool> m_imageRequestReady{false};
    uint8_t m_imageType = 0;
    std::string m_requestedImageName;

    // Bulk-write buffer.
    static constexpr int m_imageBufferSize = (1 * 1024 * 1024) + 4;
    uint8_t m_imageBuffer[m_imageBufferSize];

    // Console.
    std::unique_ptr<AstraConsole> m_console;
    AstraUbootConsole m_ubootConsole = ASTRA_UBOOT_CONSOLE_USB;

    // SL16XX service image filenames.
    const std::string m_usbPathImageFilename = "06_IMAGE";
    // m_sizeRequestImageFilename (base) is set to "07_IMAGE" in the factory.
    std::unique_ptr<Image> m_sizeRequestImage;

    // Magic string that identifies image-request interrupt packets.
    const std::string m_imageRequestString = "i*m*g*r*q*";

    // -----------------------------------------------------------------------
    // Virtual hook: WaitForImageRequest
    // Uses m_imageRequestCV set by HandleInterrupt (interrupt thread).
    // -----------------------------------------------------------------------
    bool WaitForImageRequest(std::string &name, uint8_t &imageType,
        std::chrono::milliseconds timeout) override
    {
        ASTRA_LOG;

        bool notified = false;
        {
            std::unique_lock<std::mutex> lock(m_imageRequestMutex);
            log(ASTRA_LOG_LEVEL_DEBUG) << "WaitForImageRequest: waiting" << endLog;

            notified = m_imageRequestCV.wait_for(lock, timeout, [this] {
                bool prev = m_imageRequestReady.load();
                if (prev) {
                    m_imageRequestReady.store(false);
                }
                return prev || !m_running.load();
            });
        }

        if (!m_running.load()) {
            return false; // shutdown
        }

        if (!notified) {
            return false; // timeout
        }

        name      = m_requestedImageName;
        imageType = m_imageType;
        return true;
    }

    void HandleInterrupt(uint8_t *buf, size_t size)
    {
        ASTRA_LOG;

        log(ASTRA_LOG_LEVEL_DEBUG) << "Interrupt received: size:" << size << endLog;

        std::string message(reinterpret_cast<char *>(buf), size);

        auto it = message.find(m_imageRequestString);
        if (it != std::string::npos) {
            if (m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE && !m_bootOnly) {
                m_status = ASTRA_DEVICE_STATUS_UPDATE_START;
            }

            it += m_imageRequestString.size();
            m_imageType = buf[it];
            log(ASTRA_LOG_LEVEL_DEBUG) << "Image type: " << std::hex << m_imageType << std::dec << endLog;

            std::string imageName = message.substr(it + 1);

            // Strip off trailing NUL bytes.
            size_t end = imageName.find_last_not_of('\0');
            if (end != std::string::npos) {
                m_requestedImageName = imageName.substr(0, end + 1);
            } else {
                m_requestedImageName = imageName;
            }

            log(ASTRA_LOG_LEVEL_DEBUG) << "Requested image name: '" << m_requestedImageName << "'" << endLog;

            m_imageRequestReady.store(true);
            m_imageRequestCV.notify_one();
        } else if (m_console != nullptr) {
            m_console->Append(message);
        }
    }

    void USBEventHandler(USBDevice::USBEvent event, uint8_t *buf, size_t size)
    {
        ASTRA_LOG;

        if (event == USBDevice::USB_DEVICE_EVENT_INTERRUPT) {
            HandleInterrupt(buf, size);
        } else if (event == USBDevice::USB_DEVICE_EVENT_NO_DEVICE ||
            event == USBDevice::USB_DEVICE_EVENT_TRANSFER_CANCELED ||
            event == USBDevice::USB_DEVICE_EVENT_TRANSFER_ERROR)
        {
            // When using SU-Boot, gen3_miniloader.bin.usb causes reset/reconnect.
            if (m_requestedImageName == "gen3_miniloader.bin.usb") {
                log(ASTRA_LOG_LEVEL_INFO) << "Device disconnected: after sending gen3_miniloader.bin.usb" << endLog;
            } else {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Device disconnected: shutting down" << endLog;

                if (m_status == ASTRA_DEVICE_STATUS_UPDATE_PROGRESS) {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
                } else if (m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                }

                if (m_status != ASTRA_DEVICE_STATUS_UPDATE_COMPLETE &&
                    m_status != ASTRA_DEVICE_STATUS_BOOT_COMPLETE)
                {
                    // Completed statuses are emitted from WaitForCompletion.
                    ReportStatus(m_status, 0, "", "Device disconnected");
                }
            }

            m_running.store(false);
            m_deviceEventCV.notify_all();
        }
    }

    int UpdateImageSizeRequestFile(uint32_t fileSize)
    {
        ASTRA_LOG;

        if (m_imageType <= 0x79) {
            return 0;
        }

        std::string imageName = m_sizeRequestImage->GetPath();
        FILE *sizeFile = fopen(imageName.c_str(), "wb");
        if (!sizeFile) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open " << imageName << " file" << endLog;
            return -1;
        }

        log(ASTRA_LOG_LEVEL_DEBUG) << "Writing image size to 07_IMAGE: " << fileSize << endLog;

        if (fwrite(&fileSize, sizeof(fileSize), 1, sizeFile) != 1) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write image size to file" << endLog;
            fclose(sizeFile);
            return -1;
        }

        fflush(sizeFile);
        fclose(sizeFile);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Virtual hook: SendImagePayload
    // Sends the SL16XX size-header then bulk-streams image data.
    // START / COMPLETE / FAIL status events are emitted by RunImageRequestLoop;
    // this method only emits IMAGE_SEND_PROGRESS.
    // -----------------------------------------------------------------------
    int SendImagePayload(Image &image) override
    {
        ASTRA_LOG;

        int totalTransferred = 0;
        int transferred = 0;

        int ret = image.Load();
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to load image" << endLog;
            return ret;
        }

        const int imageHeaderSize = sizeof(uint32_t) * 2;
        uint32_t imageSizeLE = HostToLE(image.GetSize());
        std::memset(m_imageBuffer, 0, imageHeaderSize);
        std::memcpy(m_imageBuffer, &imageSizeLE, sizeof(imageSizeLE));

        const int totalTransferSize = image.GetSize() + imageHeaderSize;

        ret = m_usbDevice->Write(m_imageBuffer, imageHeaderSize, &transferred);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write image header" << endLog;
            return ret;
        }
        totalTransferred += transferred;

        if (!ShouldSuppressImageStatus(image.GetName())) {
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS,
                (static_cast<double>(totalTransferred) / totalTransferSize) * 100.0,
                image.GetName());
        }

        log(ASTRA_LOG_LEVEL_DEBUG) << "Total transfer size: " << totalTransferSize << endLog;

        while (totalTransferred < totalTransferSize) {
            int dataBlockSize = image.GetDataBlock(m_imageBuffer, m_imageBufferSize);
            if (dataBlockSize < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get data block" << endLog;
                return -1;
            }

            ret = m_usbDevice->Write(m_imageBuffer, dataBlockSize, &transferred);
            if (ret < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write image data" << endLog;
                return ret;
            }
            totalTransferred += transferred;

            if (!ShouldSuppressImageStatus(image.GetName())) {
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS,
                    (static_cast<double>(totalTransferred) / totalTransferSize) * 100.0,
                    image.GetName());
            }
        }

        if (totalTransferred != totalTransferSize) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to transfer entire image" << endLog;
            return -1;
        }

        return 0;
    }

    // -----------------------------------------------------------------------
    // Virtual hook: OnImageSent
    // Writes the image size into 07_IMAGE (SL16XX size-request protocol).
    // -----------------------------------------------------------------------
    void OnImageSent(const Image &image, bool success) override
    {
        if (!success) {
            return;
        }
        UpdateImageSizeRequestFile(image.GetSize());
    }

    // -----------------------------------------------------------------------
    // Virtual hook: ShouldSuppressImageStatus
    // Suppresses status events for the 07_IMAGE (size-request) image.
    // -----------------------------------------------------------------------
    bool ShouldSuppressImageStatus(const std::string &imageName) override
    {
        return imageName == m_sizeRequestImageFilename;
    }

    // -----------------------------------------------------------------------
    // Virtual hook: WakeImageRequestThread
    // Notifies m_imageRequestCV so WaitForImageRequest returns immediately.
    // -----------------------------------------------------------------------
    void WakeImageRequestThread() override
    {
        m_imageRequestCV.notify_all();
    }
};

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL16XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand)
{
    return std::make_unique<AstraDeviceSL16XXImpl>(std::move(device), tempDir, bootOnly, bootCommand);
}
