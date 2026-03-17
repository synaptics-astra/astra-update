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
    {}

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

        m_uEnvSupport = bootImage->GetUEnvSupport();
        m_ubootConsole = bootImage->GetUbootConsole();
        m_finalBootImage = bootImage->GetFinalBootImage();
        m_requestedImageName.clear();
        m_finalUpdateImage.clear();
        m_imageCount = 0;
        m_imageRequestThreadReady.store(false);

        int ret = m_usbDevice->Open(std::bind(&AstraDeviceSL16XXImpl::USBEventHandler, this,
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open device" << endLog;
            return ret;
        }

        m_deviceName = "device:" + m_usbDevice->GetUSBPath();
        log(ASTRA_LOG_LEVEL_INFO) << "Device name: " << m_deviceName << endLog;

        std::string modifiedDeviceName = m_deviceName;
        std::remove(modifiedDeviceName.begin(), modifiedDeviceName.end(), ':');
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
        m_sizeRequestImage = std::make_unique<Image>(m_deviceDir + "/" + m_sizeRequestImageFilename,
            ASTRA_IMAGE_TYPE_UPDATE_EMMC);

        m_status = ASTRA_DEVICE_STATUS_OPENED;

        std::vector<Image> bootImageSubimages = bootImage->GetImages();

        {
            std::lock_guard<std::mutex> lock(m_imageMutex);
            m_images.insert(m_images.end(), bootImageSubimages.begin(), bootImageSubimages.end());

            auto it = std::find_if(m_images.begin(), m_images.end(), [this](const Image &img) {
                return img.GetName() == m_uEnvFilename;
            });

            // If uEnv.txt is not in the image list and uEnv is supported
            // then create a uEnv image in the temp directory using the boot command.
            if (it == m_images.end() && m_uEnvSupport) {
                if (bootStage == ASTRA_DEVICE_BOOT_STAGE_LINUX) {
                    bootStage = ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER;
                    log(ASTRA_LOG_LEVEL_INFO) << "Booting Linux requires a Linux specific uEnv.txt file in the image directory. Booting to the bootloader since this file is missing." << endLog;
                }

                log(ASTRA_LOG_LEVEL_DEBUG) << "Adding uEnv.txt to image list" << endLog;
                Image uEnvImage(m_deviceDir + "/" + m_uEnvFilename, ASTRA_IMAGE_TYPE_BOOT);

                if (m_bootCommand.empty()) {
                    // If m_bootCommand is empty then booting is complete after
                    // uEnv.txt is loaded, even if additional boot images exist.
                    m_finalBootImage = m_uEnvFilename;
                }

                WriteUEnvFile(m_bootCommand);
                m_images.push_back(uEnvImage);
            }

            if (!m_bootOnly && bootImage->IsLinuxBoot()) {
                // Update can use a boot image with Linux kernel/ramdisk, but they
                // are not used during update and should not gate completion.
                m_finalBootImage = m_uEnvFilename;
            }

            m_images.push_back(usbPathImage);
            m_images.push_back(*m_sizeRequestImage);
        }

        m_status = ASTRA_DEVICE_STATUS_BOOT_START;

        m_running.store(true);
        m_imageRequestThread = std::thread(std::bind(&AstraDeviceSL16XXImpl::ImageRequestThread, this));

        {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Waiting for imageRequest thread to be ready" << endLog;
            std::unique_lock<std::mutex> lock(m_imageRequestThreadReadyMutex);
            m_imageRequestThreadReadyCV.wait(lock, [this] {
                return m_imageRequestThreadReady.load();
            });
        }

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

        m_finalUpdateImage = flashImage->GetFinalImage();
        m_resetWhenComplete = flashImage->GetResetWhenComplete();

        {
            std::lock_guard<std::mutex> lock(m_imageMutex);
            m_images.insert(m_images.end(), flashImage->GetImages().begin(), flashImage->GetImages().end());
        }

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
                        SendStatus(m_status, 100, "", "Success");
                    }
                } else {
                    if (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                        // Device successfully reset after update.
                        SendStatus(m_status, 100, "", "Success");
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
                    SendStatus(m_status, 100, "", "Success");
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

        m_running.store(false);
        m_deviceEventCV.notify_all();
        m_imageRequestCV.notify_all();

        if (m_imageRequestThread.joinable()) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Joining image request thread" << endLog;
            m_imageRequestThread.join();
        }

        if (m_console != nullptr) {
            m_console->Shutdown();
        }

        m_images.clear();

        AstraDeviceImpl::Close();
    }

private:
    std::atomic<bool> m_sl16Shutdown{false};
    std::mutex m_sl16CloseMutex;

    bool m_uEnvSupport = false;
    bool m_resetWhenComplete = false;

    std::atomic<bool> m_running{false};

    std::mutex m_imageMutex;
    std::vector<Image> m_images;

    std::condition_variable m_deviceEventCV;
    std::mutex m_deviceEventMutex;

    std::thread m_imageRequestThread;
    std::condition_variable m_imageRequestCV;
    std::mutex m_imageRequestMutex;
    std::atomic<bool> m_imageRequestReady = false;
    uint8_t m_imageType = 0;
    std::string m_requestedImageName;

    std::condition_variable m_imageRequestThreadReadyCV;
    std::mutex m_imageRequestThreadReadyMutex;
    std::atomic<bool> m_imageRequestThreadReady = false;

    const std::string m_imageRequestString = "i*m*g*r*q*";
    static constexpr int m_imageBufferSize = (1 * 1024 * 1024) + 4;
    uint8_t m_imageBuffer[m_imageBufferSize];

    std::string m_finalBootImage;
    std::string m_finalUpdateImage;

    std::unique_ptr<AstraConsole> m_console;
    AstraUbootConsole m_ubootConsole = ASTRA_UBOOT_CONSOLE_USB;

    std::string m_deviceDir;
    const std::string m_usbPathImageFilename = "06_IMAGE";
    const std::string m_sizeRequestImageFilename = "07_IMAGE";
    const std::string m_uEnvFilename = "uEnv.txt";
    std::unique_ptr<Image> m_sizeRequestImage;

    int m_imageCount = 0;

    void SendStatus(AstraDeviceStatus status, double progress, const std::string &imageName,
        const std::string &message = "")
    {
        ASTRA_LOG;

        if (imageName == m_sizeRequestImageFilename) {
            return;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "Device status: " << AstraDevice::AstraDeviceStatusToString(status)
            << " Progress: " << progress << " Image: " << imageName << " Message: " << message << endLog;

        if (m_statusCallback) {
            m_statusCallback({DeviceResponse{m_deviceName, status, progress, imageName, message}});
        }
    }

    void ImageRequestThread()
    {
        ASTRA_LOG;

        int ret = HandleImageRequests();
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to handle image request" << endLog;
        }
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
                    SendStatus(m_status, 0, "", "Device disconnected");
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

    int SendImage(Image *image)
    {
        ASTRA_LOG;

        int totalTransferred = 0;
        int transferred = 0;

        int ret = image->Load();
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to load image" << endLog;
            return ret;
        }

        SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, image->GetName());

        const int imageHeaderSize = sizeof(uint32_t) * 2;
        uint32_t imageSizeLE = HostToLE(image->GetSize());
        std::memset(m_imageBuffer, 0, imageHeaderSize);
        std::memcpy(m_imageBuffer, &imageSizeLE, sizeof(imageSizeLE));

        const int totalTransferSize = image->GetSize() + imageHeaderSize;

        ret = m_usbDevice->Write(m_imageBuffer, imageHeaderSize, &transferred);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write image" << endLog;
            SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, image->GetName(), "Failed to write image");
            return ret;
        }

        totalTransferred += transferred;

        SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS,
            (static_cast<double>(totalTransferred) / totalTransferSize) * 100.0,
            image->GetName());

        log(ASTRA_LOG_LEVEL_DEBUG) << "Total transfer size: " << totalTransferSize << endLog;
        log(ASTRA_LOG_LEVEL_DEBUG) << "Total transferred: " << totalTransferred << endLog;

        while (totalTransferred < totalTransferSize) {
            int dataBlockSize = image->GetDataBlock(m_imageBuffer, m_imageBufferSize);
            if (dataBlockSize < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get data block" << endLog;
                SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, image->GetName(), "Failed to get data block");
                return -1;
            }

            ret = m_usbDevice->Write(m_imageBuffer, dataBlockSize, &transferred);
            if (ret < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write image" << endLog;
                SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, image->GetName(), "Failed to write image");
                return ret;
            }

            totalTransferred += transferred;

            SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS,
                (static_cast<double>(totalTransferred) / totalTransferSize) * 100.0,
                image->GetName());
        }

        if (totalTransferred != totalTransferSize) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to transfer entire image" << endLog;
            SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, image->GetName(), "Failed to transfer entire image");
            return -1;
        }

        ret = UpdateImageSizeRequestFile(image->GetSize());
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to update image size request file" << endLog;
        }

        SendStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, image->GetName());
        return 0;
    }

    int HandleImageRequests()
    {
        ASTRA_LOG;

        int ret = 0;

        log(ASTRA_LOG_LEVEL_DEBUG) << "Signal image request thread ready" << endLog;

        m_imageRequestThreadReady.store(true);
        m_imageRequestThreadReadyCV.notify_all();

        bool waitForSizeRequest = false;

        while (true) {
            bool notified = false;

            {
                std::unique_lock<std::mutex> lock(m_imageRequestMutex);
                log(ASTRA_LOG_LEVEL_DEBUG) << "before m_imageRequestCV.wait()" << endLog;

                auto timeout = std::chrono::seconds(10);
                notified = m_imageRequestCV.wait_for(lock, timeout, [this] {
                    bool previousValue = m_imageRequestReady.load();
                    if (previousValue) {
                        m_imageRequestReady.store(false);
                    }
                    return previousValue || !m_running.load();
                });
            }

            log(ASTRA_LOG_LEVEL_DEBUG) << "after m_imageRequestCV.wait()" << endLog;
            if (!m_running.load()) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Image Request received when AstraDevice is not running: "
                    << m_requestedImageName << endLog;
                return 0;
            }

            if (!notified) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Timeout waiting for image request: device status: "
                    << AstraDevice::AstraDeviceStatusToString(m_status) << endLog;

                if (m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                    SendStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "",
                        "Timeout during boot, press RESET while holding USB_BOOT to try again");
                    return -1;
                }

                if (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                    // Update is complete, but device has not disconnected yet.
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Update complete: shutting down image request thread" << endLog;
                    m_running.store(false);
                    m_deviceEventCV.notify_all();
                    return 0;
                }

                if (m_status == ASTRA_DEVICE_STATUS_BOOT_START) {
                    // Boot failed to start; device may be in an invalid state.
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Update failed to start" << endLog;
                    m_running.store(false);
                    m_deviceEventCV.notify_all();
                    return -1;
                }

                continue;
            }

            if (m_requestedImageName.find('/') != std::string::npos) {
                size_t pos = m_requestedImageName.find('/');
                std::string imageNamePrefix = m_requestedImageName.substr(0, pos);
                m_requestedImageName = m_requestedImageName.substr(pos + 1);
                log(ASTRA_LOG_LEVEL_DEBUG) << "Requested image name prefix: '" << imageNamePrefix
                    << "', requested image name: '" << m_requestedImageName << "'" << endLog;
            }

            {
                std::lock_guard<std::mutex> lock(m_imageMutex);
                auto it = std::find_if(m_images.begin(), m_images.end(),
                    [requestedImageName = m_requestedImageName](const Image &img) {
                        return img.GetName() == requestedImageName;
                    });

                Image *image = nullptr;
                if (it == m_images.end()) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Requested image not found: " << m_requestedImageName << endLog;
                    if (m_status == ASTRA_DEVICE_STATUS_BOOT_START || m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                        SendStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, m_requestedImageName,
                            m_requestedImageName + " image not found");
                    } else if (m_status == ASTRA_DEVICE_STATUS_UPDATE_START || m_status == ASTRA_DEVICE_STATUS_UPDATE_PROGRESS) {
                        SendStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, m_requestedImageName,
                            m_requestedImageName + " image not found");
                    } else {
                        log(ASTRA_LOG_LEVEL_WARNING) << "Requested image not found: " << m_requestedImageName
                            << " while in " << AstraDevice::AstraDeviceStatusToString(m_status) << endLog;
                    }
                    return -1;
                }

                image = &(*it);

                if (m_status == ASTRA_DEVICE_STATUS_BOOT_START) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
                } else if (m_status == ASTRA_DEVICE_STATUS_UPDATE_START) {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_PROGRESS;
                }

                ret = SendImage(image);
                log(ASTRA_LOG_LEVEL_DEBUG) << "After send image: " << image->GetName() << endLog;

                if (ret < 0) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send image" << endLog;
                    if (m_status == ASTRA_DEVICE_STATUS_BOOT_START || m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                        m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                    } else if (m_status == ASTRA_DEVICE_STATUS_UPDATE_START || m_status == ASTRA_DEVICE_STATUS_UPDATE_PROGRESS) {
                        m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
                    }
                    SendStatus(m_status, 0, image->GetName(), "Failed to send image");
                    return ret;
                }

                log(ASTRA_LOG_LEVEL_DEBUG) << "Image sent successfully: " << image->GetName()
                    << " final boot image '" << m_finalBootImage
                    << "' final update image: '" << m_finalUpdateImage << "'" << endLog;

                if (!m_finalBootImage.empty() && image->GetName().find(m_finalBootImage) != std::string::npos) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Final boot image sent" << endLog;
                    if (!m_bootOnly) {
                        // Boot complete status is emitted from WaitForCompletion in boot-only mode.
                        m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                        SendStatus(m_status, 100, "", "Success");
                    } else {
                        waitForSizeRequest = true;
                    }
                } else if (!m_finalUpdateImage.empty() && image->GetName().find(m_finalUpdateImage) != std::string::npos) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Final update image sent" << endLog;
                    if (image->GetImageType() == ASTRA_IMAGE_TYPE_UPDATE_EMMC ||
                        image->GetImageType() == ASTRA_IMAGE_TYPE_UPDATE_SPI)
                    {
                        // eMMC/SPI update asks for a size request image after payload.
                        waitForSizeRequest = true;
                    } else {
                        m_status = ASTRA_DEVICE_STATUS_UPDATE_COMPLETE;
                    }
                } else if (waitForSizeRequest && image->GetName() == m_sizeRequestImageFilename) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Size request image sent" << endLog;
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_COMPLETE;
                    waitForSizeRequest = false;
                }

                ++m_imageCount;
                log(ASTRA_LOG_LEVEL_DEBUG) << "Image count: " << m_imageCount << endLog;
            }
        }

        return 0;
    }

    bool WriteUEnvFile(const std::string &bootCommand)
    {
        ASTRA_LOG;

        std::string uEnv = "bootcmd=" + bootCommand;
        std::ofstream uEnvFile(m_deviceDir + "/" + m_uEnvFilename);
        if (!uEnvFile) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open uEnv.txt file" << endLog;
            return false;
        }

        uEnvFile << uEnv;
        uEnvFile.close();
        return true;
    }
};

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL16XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand)
{
    return std::make_unique<AstraDeviceSL16XXImpl>(std::move(device), tempDir, bootOnly, bootCommand);
}
