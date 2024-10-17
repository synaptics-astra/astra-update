#include <iostream>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <filesystem>
#include <condition_variable>
#include "astra_device.hpp"
#include "astra_device_manager.hpp"
#include "boot_image_collection.hpp"
#include "usb_transport.hpp"
#include "image.hpp"
#include "astra_log.hpp"
#include "utils.hpp"

#if PLATFORM_WINDOWS
#include "win_usb_transport.hpp"
#endif

class AstraDeviceManager::AstraDeviceManagerImpl {
public:
    AstraDeviceManagerImpl(std::function<void(AstraDeviceManagerResponse)> responseCallback,
        bool runContinuously,
        AstraLogLevel minLogLevel, const std::string &logPath,
        const std::string &tempDir, bool usbDebug)
        : m_responseCallback{responseCallback}, m_runContinuously{runContinuously}, m_usbDebug{usbDebug}
    {
        if (tempDir.empty()) {
            m_tempDir = MakeTempDirectory();
            if (m_tempDir.empty()) {
                m_tempDir = "./";
            }
            m_removeTempOnClose = true;
        } else {
            m_tempDir = tempDir;
            std::filesystem::create_directories(m_tempDir);
        }

        m_modifiedLogPath = logPath;
        if (logPath == "") {
            m_modifiedLogPath = m_tempDir + "/astra_device_manager.log";
        }
        AstraLogStore::getInstance().Open(m_modifiedLogPath, minLogLevel);

        ASTRA_LOG;
    }

    void Update(std::shared_ptr<FlashImage> flashImage, std::string bootImagesPath)
    {
        ASTRA_LOG;

        m_flashImage = flashImage;
        m_bootCommand = flashImage->GetFlashCommand();

        m_managerMode = ASTRA_DEVICE_MANAGER_MODE_UPDATE;

        BootImageCollection bootImageCollection = BootImageCollection(bootImagesPath);
        bootImageCollection.Load();

        if (m_flashImage->GetBootImageId().empty()) {
            // No boot images specified.
            // Try to find the best boot image based on other properties
            if (m_flashImage->GetChipName().empty()) {
                throw std::runtime_error("Chip name and boot bootImage ID missing!");
            }

            std::vector<std::shared_ptr<AstraBootImage>> bootImages = bootImageCollection.GetBootImagesForChip(m_flashImage->GetChipName(),
            m_flashImage->GetSecureBootVersion(), m_flashImage->GetMemoryLayout(), m_flashImage->GetBoardName());
            if (bootImages.size() == 0) {
                throw std::runtime_error("No boot image found for chip: " + m_flashImage->GetChipName());
            } else if (bootImages.size() > 1) {
                m_bootImage = bootImages[0];
                for (const auto& bootImage : bootImages) {
                    log(ASTRA_LOG_LEVEL_INFO) << "Boot Image: " << bootImage->GetChipName() << " " << bootImage->GetBoardName() << endLog;
                    if (bootImage->GetUbootVariant() == ASTRA_UBOOT_VARIANT_SYNAPTICS && bootImage->GetUEnvSupport()) {
                        // Boot bootImages with Synaptics u-boot variant is preferred
                        m_bootImage = bootImage;
                        break;
                    } else if (bootImage->GetUEnvSupport()) {
                        // Boot bootImages with uEnv support is preferred
                        m_bootImage = bootImage;
                    } else if (!m_bootImage->GetUEnvSupport() && bootImage->GetUbootConsole() == ASTRA_UBOOT_CONSOLE_USB) {
                        // Boot bootImages with USB console is preferred over UART
                        // But only if there is no uEnv support
                        m_bootImage = bootImage;
                    }
                }
            } else {
                // Try the only option
                m_bootImage = bootImages[0];
            }
        } else {
            // Exact boot bootImages specified
            m_bootImage = std::make_shared<AstraBootImage>(bootImageCollection.GetBootImage(m_flashImage->GetBootImageId()));
        }

        Init();
    }

    void Boot(std::string bootImagePath, std::string bootCommand)
    {
        ASTRA_LOG;

        m_managerMode = ASTRA_DEVICE_MANAGER_MODE_BOOT;
        m_bootCommand = bootCommand;

        AstraBootImage bootImage{bootImagePath};
        if (!bootImage.Load()) {
            throw std::runtime_error("Failed to load boot image");
        }

        m_bootImage = std::make_shared<AstraBootImage>(bootImage);

        Init();
    }

    bool Shutdown()
    {
        ASTRA_LOG;

        std::lock_guard<std::mutex> lock(m_devicesMutex);
        for (auto& device : m_devices) {
            device->Close();
        }
        m_devices.clear();
        m_transport->Shutdown();
        AstraLogStore::getInstance().Close();

        if (m_removeTempOnClose) {
            try {
                std::filesystem::remove_all(m_tempDir);
            } catch (const std::exception& e) {
                log(ASTRA_LOG_LEVEL_WARNING) << "Failed to remove temp directory: " << e.what() << endLog;
            }
        }

        return m_failureReported;
    }


    std::string GetLogFile() const
    {
        return m_modifiedLogPath;
    }

private:
    std::unique_ptr<USBTransport> m_transport;
    std::function<void(AstraDeviceManagerResponse)> m_responseCallback;
    std::shared_ptr<AstraBootImage> m_bootImage;
    std::shared_ptr<FlashImage> m_flashImage;
    std::string m_bootCommand;
    std::string m_tempDir;
    AstraDeviceManangerMode m_managerMode;
    bool m_removeTempOnClose = false;
    bool m_runContinuously = false;
    bool m_deviceFound = false;
    bool m_usbDebug = false;
    bool m_failureReported = false;
    std::string m_modifiedLogPath;

    std::vector<std::shared_ptr<AstraDevice>> m_devices;
    std::mutex m_devicesMutex;

    void Init()
    {
        ASTRA_LOG;

        if (m_bootImage == nullptr) {
            throw std::runtime_error("Boot image not found");
        }

        std::string bootImageDescription = "Boot Image: " + m_bootImage->GetChipName() + " " + m_bootImage->GetBoardName() + " (" + m_bootImage->GetID() + ")\n";
        bootImageDescription += "    Secure Boot: " + AstraSecureBootVersionToString(m_bootImage->GetSecureBootVersion()) + "\n";
        bootImageDescription += "    Memory Layout: " + AstraMemoryLayoutToString(m_bootImage->GetMemoryLayout()) + "\n";
        bootImageDescription += "    U-Boot Console: " + std::string(m_bootImage->GetUbootConsole() == ASTRA_UBOOT_CONSOLE_UART ? "UART" : "USB") + "\n";
        bootImageDescription += "    uEnt.txt Support: " + std::string(m_bootImage->GetUEnvSupport() ? "enabled" : "disabled") + "\n";
        bootImageDescription += "    U-Boot Variant: " + std::string(m_bootImage->GetUbootVariant() == ASTRA_UBOOT_VARIANT_UBOOT ? "U-Boot" : "Synaptics U-Boot");
        ResponseCallback({ManagerResponse{ASTRA_DEVICE_MANAGER_STATUS_INFO, bootImageDescription}});

        uint16_t vendorId = m_bootImage->GetVendorId();
        uint16_t productId = m_bootImage->GetProductId();

#if PLATFORM_WINDOWS
        m_transport = std::make_unique<WinUSBTransport>(m_usbDebug);
#else
        m_transport = std::make_unique<USBTransport>(m_usbDebug);
#endif

        if (m_transport->Init(vendorId, productId,
                std::bind(&AstraDeviceManagerImpl::DeviceAddedCallback, this, std::placeholders::_1)) < 0)
        {
            throw std::runtime_error("Failed to initialize USB transport");
        }

        log(ASTRA_LOG_LEVEL_DEBUG) << "USB transport initialized successfully" << endLog;

        std::ostringstream os;
        os << "Waiting for Astra Device (" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << vendorId << ":" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << productId << ")";
        ResponseCallback({ManagerResponse{ASTRA_DEVICE_MANAGER_STATUS_START, os.str()}});
    }

    void ResponseCallback(AstraDeviceManagerResponse response)
    {
        // If a failure is reported then retain the temp directory containing logs
        if (response.IsDeviceManagerResponse()) {
            if (response.GetDeviceManagerResponse().m_managerStatus == ASTRA_DEVICE_MANAGER_STATUS_FAILURE) {
                m_removeTempOnClose = false;
                m_failureReported = true;
            }
        } else if (response.IsDeviceResponse()) {
            if (response.GetDeviceResponse().m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL ||
                response.GetDeviceResponse().m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL)
            {
                m_removeTempOnClose = false;
                m_failureReported = true;
            }
        }
        m_responseCallback(response);
    }

    void AstraDeviceThread(std::shared_ptr<AstraDevice> astraDevice)
    {
        ASTRA_LOG;

        log(ASTRA_LOG_LEVEL_DEBUG) << "Booting device device" << endLog;

        if (astraDevice) {
            astraDevice->SetStatusCallback(m_responseCallback);

            log(ASTRA_LOG_LEVEL_DEBUG) << "Calling boot" << endLog;
            int ret = astraDevice->Boot(m_bootImage);
            if (ret < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to boot device" << endLog;
                ResponseCallback({ DeviceResponse{astraDevice->GetDeviceName(), ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to Boot Device"}});
                return;
            }

            if (m_managerMode == ASTRA_DEVICE_MANAGER_MODE_UPDATE) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "calling from Update" << endLog;
                ret = astraDevice->Update(m_flashImage);
                if (ret < 0) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Failed to update device" << endLog;
                    return;
                }
            }

            log(ASTRA_LOG_LEVEL_DEBUG) << "calling from WaitForCompletion" << endLog;
            ret = astraDevice->WaitForCompletion();
            if (ret < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to wait for completion" << endLog;
                return;
            }

            log(ASTRA_LOG_LEVEL_DEBUG) << "returned from WaitForCompletion" << endLog;
            AstraDeviceStatus status = astraDevice->GetDeviceStatus();
            log(ASTRA_LOG_LEVEL_DEBUG) << "Device status: " << AstraDevice::AstraDeviceStatusToString(status) << endLog;
            if (status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE && !m_runContinuously) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Shutting down Astra Device Manager" << endLog;
                ResponseCallback({ManagerResponse{ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN, "Astra Device Manager shutting down"}});
            } else if (m_managerMode == ASTRA_DEVICE_MANAGER_MODE_BOOT  &&  status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE && !m_runContinuously) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Shutting down Astra Device Manager" << endLog;
                ResponseCallback({ManagerResponse{ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN, "Astra Device Manager shutting down"}});
            }

            astraDevice->Close();
        }
    }

    void DeviceAddedCallback(std::unique_ptr<USBDevice> device)
    {
        ASTRA_LOG;

        log(ASTRA_LOG_LEVEL_DEBUG) << "Device added AstraDeviceManagerImpl::DeviceAddedCallback" << endLog;
        std::shared_ptr<AstraDevice> astraDevice = std::make_shared<AstraDevice>(std::move(device), m_tempDir, m_bootCommand);

        std::lock_guard<std::mutex> lock(m_devicesMutex);
        m_deviceFound = true;
        m_devices.push_back(astraDevice);

        std::thread(std::bind(&AstraDeviceManagerImpl::AstraDeviceThread, this, astraDevice)).detach();
    }

};

AstraDeviceManager::AstraDeviceManager(std::function<void(AstraDeviceManagerResponse)> responseCallback,
    bool runContinuously,
    AstraLogLevel minLogLevel, const std::string &logPath,
    const std::string &tempDir, bool usbDebug)
    : pImpl{std::make_unique<AstraDeviceManagerImpl>(responseCallback,
        runContinuously, minLogLevel, logPath, tempDir, usbDebug)}
{}

AstraDeviceManager::~AstraDeviceManager() = default;

void AstraDeviceManager::Update(std::shared_ptr<FlashImage> flashImage, std::string bootImagePath)
{
    pImpl->Update(flashImage, bootImagePath);
}

void AstraDeviceManager::Boot(std::string bootImagesPath, std::string bootCommand)
{
    pImpl->Boot(bootImagesPath, bootCommand);
}

bool AstraDeviceManager::Shutdown()
{
    return pImpl->Shutdown();
}

std::string AstraDeviceManager::GetLogFile() const
{
    return pImpl->GetLogFile();
}