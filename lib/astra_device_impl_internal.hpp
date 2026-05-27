// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

#include "astra_device.hpp"
#include "astra_device_manager.hpp"
#include "astra_log.hpp"
#include "image.hpp"
#include "usb_device.hpp"

class AstraDeviceImpl {
public:
    AstraDeviceImpl(std::unique_ptr<USBDevice> device, const std::string &tempDir,
        bool bootOnly, const std::string &bootCommand)
        : m_usbDevice{std::move(device)}, m_tempDir{tempDir}, m_bootOnly{bootOnly}, m_bootCommand{bootCommand}
    {
        ASTRA_LOG;
    }

    virtual ~AstraDeviceImpl()
    {
        ASTRA_LOG;
        Close();
    }

    virtual void SetStatusCallback(std::function<void(AstraDeviceManagerResponse)> statusCallback)
    {
        ASTRA_LOG;
        m_statusCallback = statusCallback;
    }

    virtual int Boot(std::shared_ptr<AstraBootImage> bootImage, AstraDeviceBootStage bootStage) = 0;
    virtual int Update(std::shared_ptr<FlashImage> flashImage) = 0;
    virtual int WaitForCompletion() = 0;
    virtual int SendToConsole(const std::string &data) = 0;
    virtual int ReceiveFromConsole(std::string &data) = 0;

    /**
     * Rebind this impl to a newly-arrived USB device.
     * Called by the manager when a fastboot device reconnects with a
     * matching UUID serial.  The default implementation is a no-op;
     * SL26XX overrides it.
     */
    virtual void Rebind(std::unique_ptr<USBDevice> /*device*/) {}

    /**
     * Install registration callbacks so the impl can register/unregister
     * its UUID with the manager's fastboot-serial registry.
     */
    void SetRegistrationCallbacks(
        std::function<void(const std::string &)> registerFn,
        std::function<void(const std::string &)> unregisterFn)
    {
        m_registerFastbootSerial   = std::move(registerFn);
        m_unregisterFastbootSerial = std::move(unregisterFn);
    }

    virtual std::string GetDeviceName()
    {
        return m_deviceName;
    }

    virtual std::string GetUSBPath()
    {
        if (m_usbDevice == nullptr) {
            return "";
        }
        return m_usbDevice->GetUSBPath();
    }

    virtual AstraDeviceStatus GetDeviceStatus()
    {
        return m_status.load();
    }

    virtual void Close()
    {
        ASTRA_LOG;

        std::lock_guard<std::mutex> lock(m_closeMutex);
        if (m_shutdown.exchange(true)) {
            return;
        }

        if (m_usbDevice != nullptr) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Closing USB device" << endLog;
            m_usbDevice->Close();
        }

        m_status = ASTRA_DEVICE_STATUS_CLOSED;
    }

protected:
    void ReportStatus(AstraDeviceStatus status, double progress, const std::string &imageName,
        const std::string &message = "")
    {
        ASTRA_LOG;

        log(ASTRA_LOG_LEVEL_INFO) << "Device status: " << AstraDevice::AstraDeviceStatusToString(status)
            << " Progress: " << progress << " Image: " << imageName << " Message: " << message << endLog;

        if (m_statusCallback) {
            m_statusCallback({DeviceResponse{m_deviceName, status, progress, imageName, message}});
        }
    }

    // -----------------------------------------------------------------------
    // Shared image-request loop infrastructure
    // Derived classes call BuildBootImageList / AppendUpdateImages / Start/Stop,
    // and implement the three transport-specific hooks below.
    // -----------------------------------------------------------------------

    // Build m_images from boot image sub-images; synthesise uEnv.txt if needed.
    // Must be called after m_deviceDir is set (for uEnv.txt write path).
    void BuildBootImageList(std::shared_ptr<AstraBootImage> bootImage,
        AstraDeviceBootStage bootStage);

    // Append flash-image sub-images to m_images; update final-image tracking.
    void AppendUpdateImages(std::shared_ptr<FlashImage> flashImage);

    // Write a uEnv.txt to m_deviceDir with "bootcmd=<bootCommand>".
    bool WriteUEnvFile(const std::string &bootCommand);

    // Start the image-request thread.  Waits until the thread signals ready.
    int StartImageRequestThread();

    // Signal shutdown and join the image-request thread; clears m_images.
    // Derived Close() must also call WakeImageRequestThread() (or equivalent)
    // BEFORE calling StopImageRequestThread() so the thread wakes promptly.
    void StopImageRequestThread();

    // -----------------------------------------------------------------------
    // Transport-specific hooks (override in derived classes)
    // -----------------------------------------------------------------------

    // Block until the next image-request arrives.
    // Returns true if a request was received, false on timeout / shutdown.
    // imageType: transport-specific type byte (0 when unused, e.g. fastboot).
    virtual bool WaitForImageRequest(std::string &name, uint8_t &imageType,
        std::chrono::milliseconds timeout)
    {
        (void)name; (void)imageType; (void)timeout;
        return false;
    }

    // Send the image payload to the device.  Returns 0 on success, < 0 on failure.
    // Called while m_imageMutex is held; do not re-enter the lock.
    virtual int SendImagePayload(Image &image)
    {
        (void)image;
        return -1;
    }

    // Called after each image send attempt; allows post-send bookkeeping.
    // success == true  : send completed normally.
    // success == false : send failed (derived may want to report this differently).
    // Default: no-op.
    virtual void OnImageSent(const Image &image, bool success)
    {
        (void)image; (void)success;
    }

    // Return true if status events for the given image name should be suppressed.
    // SL16XX uses this to suppress 07_IMAGE (size-request) status events.
    // Default: never suppress.
    virtual bool ShouldSuppressImageStatus(const std::string &imageName)
    {
        (void)imageName;
        return false;
    }

    // Wake any CV that WaitForImageRequest is sleeping on so the thread can see
    // m_running == false and exit promptly.  Called by StopImageRequestThread().
    // Default: no-op (works for poll-based implementations with short timeouts).
    virtual void WakeImageRequestThread() {}

    // -----------------------------------------------------------------------
    // Shared state (initialised / used by the image-request loop)
    // -----------------------------------------------------------------------

    std::vector<Image> m_images;
    std::mutex m_imageMutex;

    std::string m_finalBootImage;
    std::string m_finalUpdateImage;

    // Set empty to disable size-request image logic (SL16XX sets "07_IMAGE").
    std::string m_sizeRequestImageFilename;

    bool m_uEnvSupport = false;
    bool m_resetWhenComplete = false;

    std::atomic<bool> m_running{false};

    std::condition_variable m_deviceEventCV;
    std::mutex m_deviceEventMutex;

    std::thread m_imageRequestThread;
    std::condition_variable m_imageRequestThreadReadyCV;
    std::mutex m_imageRequestThreadReadyMutex;
    std::atomic<bool> m_imageRequestThreadReady{false};

    int m_imageCount = 0;

    // Directory used for synthesised images (uEnv.txt, SL16XX-specific files).
    std::string m_deviceDir;
    const std::string m_uEnvFilename = "uEnv.txt";

    // UUID stamped into U-Boot's serial# env via bootcmd=setenv serial# <uuid>; ...
    // Empty until first WriteUEnvFile call (Session 1); set to the probed value on
    // Sessions 2+ (so the same UUID is re-written and U-Boot keeps the same serialno).
    std::string m_updateSessionUuid;

    // Callbacks injected by the manager so the impl can register/unregister
    // its UUID in the fastboot-serial rebind registry.
    std::function<void(const std::string &)> m_registerFastbootSerial;
    std::function<void(const std::string &)> m_unregisterFastbootSerial;


    // -----------------------------------------------------------------------
    // Existing base state
    // -----------------------------------------------------------------------

    std::unique_ptr<USBDevice> m_usbDevice;
    // Atomic: read/written from the image-request loop, USB-event/interrupt
    // thread (HandleInterrupt / USBEventHandler), and the manager thread.
    // Plain enum reads/writes from multiple threads are UB under the C++
    // memory model; std::atomic<EnumType> gives well-defined behaviour while
    // preserving operator= / operator T() so existing call sites compile
    // unchanged.
    std::atomic<AstraDeviceStatus> m_status{ASTRA_DEVICE_STATUS_ADDED};
    std::function<void(AstraDeviceManagerResponse)> m_statusCallback;
    std::string m_deviceName;
    std::string m_tempDir;
    bool m_bootOnly = false;
    std::string m_bootCommand;
    AstraDeviceBootStage m_bootStage = ASTRA_DEVICE_BOOT_STAGE_AUTO;

    // Shared close-guard used by both the base Close() and derived overrides.
    // Derived Close() implementations should take m_closeMutex and gate on
    // m_shutdown.exchange(true) rather than introducing their own duplicate
    // mutex/atomic pair.
    std::mutex m_closeMutex;
    std::atomic<bool> m_shutdown{false};

private:
    void ImageRequestThreadFunc();
    void RunImageRequestLoop();
};

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL16XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand);

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL26XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand);
