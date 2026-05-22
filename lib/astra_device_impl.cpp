// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "astra_device_impl_internal.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "astra_boot_image.hpp"

// ---------------------------------------------------------------------------
// BuildBootImageList
// Populates m_images from bootImage subimages and (if needed) a synthesised
// uEnv.txt.  Must be called after m_deviceDir is set when uEnv synthesis may
// be needed.
// ---------------------------------------------------------------------------
void AstraDeviceImpl::BuildBootImageList(std::shared_ptr<AstraBootImage> bootImage,
    AstraDeviceBootStage bootStage)
{
    ASTRA_LOG;

    m_uEnvSupport  = bootImage->GetUEnvSupport();
    m_finalBootImage = bootImage->GetFinalBootImage();
    m_finalUpdateImage.clear();

    std::vector<Image> subimages = bootImage->GetImages();

    std::lock_guard<std::mutex> lock(m_imageMutex);
    m_images.insert(m_images.end(), subimages.begin(), subimages.end());

    auto it = std::find_if(m_images.begin(), m_images.end(), [this](const Image &img) {
        return img.GetName() == m_uEnvFilename;
    });

    if (it == m_images.end() && m_uEnvSupport) {
        if (bootStage == ASTRA_DEVICE_BOOT_STAGE_LINUX) {
            bootStage = ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER;
            log(ASTRA_LOG_LEVEL_INFO) << "Booting Linux requires a Linux specific uEnv.txt "
                "file in the image directory. Booting to the bootloader since this file is missing."
                << endLog;
        }

        log(ASTRA_LOG_LEVEL_DEBUG) << "Adding uEnv.txt to image list" << endLog;
        Image uEnvImage(m_deviceDir + "/" + m_uEnvFilename, ASTRA_IMAGE_TYPE_BOOT);

        if (m_bootCommand.empty()) {
            // Boot is complete once uEnv.txt is loaded.
            m_finalBootImage = m_uEnvFilename;
        }

        WriteUEnvFile(m_bootCommand);
        m_images.push_back(uEnvImage);
    }

    if (!m_bootOnly && bootImage->IsLinuxBoot()) {
        // Linux-boot images include kernel/ramdisk that are not used during
        // update; uEnv.txt is the real gating image.
        m_finalBootImage = m_uEnvFilename;
    }
}

// ---------------------------------------------------------------------------
// AppendUpdateImages
// ---------------------------------------------------------------------------
void AstraDeviceImpl::AppendUpdateImages(std::shared_ptr<FlashImage> flashImage)
{
    m_finalUpdateImage  = flashImage->GetFinalImage();
    m_resetWhenComplete = flashImage->GetResetWhenComplete();

    std::lock_guard<std::mutex> lock(m_imageMutex);
    const std::vector<Image> &imgs = flashImage->GetImages();
    m_images.insert(m_images.end(), imgs.begin(), imgs.end());
}

// ---------------------------------------------------------------------------
// WriteUEnvFile
// ---------------------------------------------------------------------------
bool AstraDeviceImpl::WriteUEnvFile(const std::string &bootCommand)
{
    ASTRA_LOG;

    // Generate a UUID (32 hex chars, no dashes) on the first call.
    // Subsequent calls (Sessions 2+) reuse the UUID that was previously
    // read from U-Boot's getvar:serialno and stored in m_updateSessionUuid.
    if (m_updateSessionUuid.empty()) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(16) << dist(gen)
            << std::setw(16) << dist(gen);
        m_updateSessionUuid = oss.str();
        log(ASTRA_LOG_LEVEL_DEBUG) << "WriteUEnvFile: generated UUID " << m_updateSessionUuid << endLog;
    }

    // Prepend "setenv serial# <uuid>;" so U-Boot's fastboot gadget reports
    // this UUID via getvar:serialno — used for reconnect matching.
    std::string uEnv = "bootcmd=setenv serial# " + m_updateSessionUuid + "; " + bootCommand;
    std::ofstream uEnvFile(m_deviceDir + "/" + m_uEnvFilename);
    if (!uEnvFile) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open uEnv.txt file" << endLog;
        return false;
    }

    uEnvFile << uEnv;
    uEnvFile.close();
    return true;
}

// ---------------------------------------------------------------------------
// StartImageRequestThread
// Sets m_running, starts the thread, then blocks until the thread signals
// that it is ready to accept image requests.
// ---------------------------------------------------------------------------
int AstraDeviceImpl::StartImageRequestThread()
{
    ASTRA_LOG;

    m_imageCount = 0;
    m_imageRequestThreadReady.store(false);
    m_running.store(true);
    m_imageRequestThread = std::thread(&AstraDeviceImpl::ImageRequestThreadFunc, this);

    log(ASTRA_LOG_LEVEL_DEBUG) << "Waiting for image request thread to be ready" << endLog;
    std::unique_lock<std::mutex> lock(m_imageRequestThreadReadyMutex);
    m_imageRequestThreadReadyCV.wait(lock, [this] {
        return m_imageRequestThreadReady.load();
    });

    return 0;
}

// ---------------------------------------------------------------------------
// StopImageRequestThread
// Signals m_running = false, wakes the thread (via virtual hook and deviceEventCV),
// then joins and clears m_images.
// ---------------------------------------------------------------------------
void AstraDeviceImpl::StopImageRequestThread()
{
    ASTRA_LOG;

    m_running.store(false);
    m_deviceEventCV.notify_all();
    WakeImageRequestThread();

    if (m_imageRequestThread.joinable()) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Joining image request thread" << endLog;
        m_imageRequestThread.join();
    }

    std::lock_guard<std::mutex> lock(m_imageMutex);
    m_images.clear();
}

// ---------------------------------------------------------------------------
// ImageRequestThreadFunc  (thin wrapper so the lambda / bind is cleaner)
// ---------------------------------------------------------------------------
void AstraDeviceImpl::ImageRequestThreadFunc()
{
    RunImageRequestLoop();
}

// ---------------------------------------------------------------------------
// RunImageRequestLoop
// The shared image-serving loop.  Mirrors SL16XX HandleImageRequests but uses
// the three virtual hooks (WaitForImageRequest / SendImagePayload / OnImageSent)
// so both SL16XX (interrupt-driven) and SL26XX (fastboot poll-driven) can plug in.
// ---------------------------------------------------------------------------
void AstraDeviceImpl::RunImageRequestLoop()
{
    ASTRA_LOG;

    log(ASTRA_LOG_LEVEL_DEBUG) << "Signal image request thread ready" << endLog;
    m_imageRequestThreadReady.store(true);
    m_imageRequestThreadReadyCV.notify_all();

    bool waitForSizeRequest = false;

    while (true) {
        std::string requestedImageName;
        uint8_t imageType = 0;

        const auto timeout = std::chrono::seconds(10);
        bool gotRequest = WaitForImageRequest(requestedImageName, imageType, timeout);

        if (!m_running.load()) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Image request loop: shutting down" << endLog;
            m_deviceEventCV.notify_all();
            return;
        }

        if (!gotRequest) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Timeout waiting for image request: status: "
                << AstraDevice::AstraDeviceStatusToString(m_status) << endLog;

            if (m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "",
                    "Timeout during boot, press RESET while holding USB_BOOT to try again");
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return;
            }

            if (m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Update complete: shutting down image request thread" << endLog;
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return;
            }

            if (m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE && m_bootOnly) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Boot-only complete: shutting down image request thread" << endLog;
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return;
            }

            if (m_status == ASTRA_DEVICE_STATUS_BOOT_START) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Boot failed to start" << endLog;
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return;
            }

            continue;
        }

        // Strip a leading directory component (e.g. "boot/uEnv.txt" → "uEnv.txt").
        if (requestedImageName.find('/') != std::string::npos) {
            size_t pos = requestedImageName.find('/');
            std::string prefix = requestedImageName.substr(0, pos);
            requestedImageName = requestedImageName.substr(pos + 1);
            log(ASTRA_LOG_LEVEL_DEBUG) << "Image name prefix: '" << prefix
                << "', name: '" << requestedImageName << "'" << endLog;
        }

        {
            std::lock_guard<std::mutex> lock(m_imageMutex);

            auto it = std::find_if(m_images.begin(), m_images.end(),
                [&requestedImageName](const Image &img) {
                    return img.GetName() == requestedImageName;
                });

            if (it == m_images.end()) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Requested image not found: " << requestedImageName << endLog;
                if (m_status == ASTRA_DEVICE_STATUS_BOOT_START ||
                    m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS)
                {
                    ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, requestedImageName,
                        requestedImageName + " image not found");
                } else if (m_status == ASTRA_DEVICE_STATUS_UPDATE_START ||
                           m_status == ASTRA_DEVICE_STATUS_UPDATE_PROGRESS)
                {
                    ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, requestedImageName,
                        requestedImageName + " image not found");
                } else {
                    log(ASTRA_LOG_LEVEL_WARNING) << "Requested image not found: " << requestedImageName
                        << " while in " << AstraDevice::AstraDeviceStatusToString(m_status) << endLog;
                }
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return;
            }

            Image &image = *it;

            if (m_status == ASTRA_DEVICE_STATUS_BOOT_START) {
                m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
            } else if (m_status == ASTRA_DEVICE_STATUS_UPDATE_START) {
                m_status = ASTRA_DEVICE_STATUS_UPDATE_PROGRESS;
            }

            if (!ShouldSuppressImageStatus(image.GetName())) {
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, image.GetName());
            }

            int ret = SendImagePayload(image);
            log(ASTRA_LOG_LEVEL_DEBUG) << "After SendImagePayload: " << image.GetName() << endLog;

            if (ret < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send image: " << image.GetName() << endLog;
                if (m_status == ASTRA_DEVICE_STATUS_BOOT_START ||
                    m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS)
                {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                } else if (m_status == ASTRA_DEVICE_STATUS_UPDATE_START ||
                           m_status == ASTRA_DEVICE_STATUS_UPDATE_PROGRESS)
                {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
                }
                if (!ShouldSuppressImageStatus(image.GetName())) {
                    ReportStatus(m_status, 0, image.GetName(), "Failed to send image");
                }
                OnImageSent(image, false);
                m_running.store(false);
                m_deviceEventCV.notify_all();
                return;
            }

            if (!ShouldSuppressImageStatus(image.GetName())) {
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, image.GetName());
            }

            OnImageSent(image, true);

            log(ASTRA_LOG_LEVEL_DEBUG) << "Image sent: " << image.GetName()
                << "  finalBoot='" << m_finalBootImage
                << "'  finalUpdate='" << m_finalUpdateImage << "'" << endLog;

            // ---- completion tracking ----
            if (!m_finalBootImage.empty() &&
                image.GetName().find(m_finalBootImage) != std::string::npos)
            {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Final boot image sent" << endLog;
                if (!m_bootOnly) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                    ReportStatus(m_status, 100, "", "Success");
                } else {
                    // In boot-only mode the size-request image (07_IMAGE for SL16XX) gates
                    // completion; skip if no size-request image is configured.
                    waitForSizeRequest = !m_sizeRequestImageFilename.empty();
                    if (!waitForSizeRequest) {
                        // No size-request handshake; boot is complete once the final image is served.
                        m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                    }
                }
            } else if (!m_finalUpdateImage.empty() &&
                       image.GetName().find(m_finalUpdateImage) != std::string::npos)
            {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Final update image sent" << endLog;
                if (!m_sizeRequestImageFilename.empty() &&
                    (image.GetImageType() == ASTRA_IMAGE_TYPE_UPDATE_EMMC ||
                     image.GetImageType() == ASTRA_IMAGE_TYPE_UPDATE_SPI))
                {
                    waitForSizeRequest = true;
                } else {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_COMPLETE;
                }
            } else if (waitForSizeRequest &&
                       !m_sizeRequestImageFilename.empty() &&
                       image.GetName() == m_sizeRequestImageFilename)
            {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Size-request image sent" << endLog;
                m_status = ASTRA_DEVICE_STATUS_UPDATE_COMPLETE;
                waitForSizeRequest = false;
            }

            ++m_imageCount;
            log(ASTRA_LOG_LEVEL_DEBUG) << "Image count: " << m_imageCount << endLog;
        }
    }
}
