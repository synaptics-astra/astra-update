// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <queue>
#include <mutex>
#include <memory>

#include "astra_device_manager.hpp"
#include "astra_device.hpp"
#include "flash_image.hpp"
#include "astra_boot_image.hpp"

namespace py = pybind11;

// Thread-safe response queue for bridging C++ callbacks to Python
class ResponseQueue {
public:
    static ResponseQueue& GetInstance() {
        static ResponseQueue instance;
        return instance;
    }

    void Put(const AstraDeviceManagerResponse& response) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(response);
    }

    bool Get(AstraDeviceManagerResponse& response) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        response = queue_.front();
        queue_.pop();
        return true;
    }

    bool IsEmpty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t Size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    ResponseQueue() = default;
    std::queue<AstraDeviceManagerResponse> queue_;
    std::mutex mutex_;
};

// Callback that bridges C++ responses into the Python-accessible queue
void ResponseQueueCallback(AstraDeviceManagerResponse response) {
    ResponseQueue::GetInstance().Put(response);
}

PYBIND11_MODULE(_astra_update, m) {
    m.doc() = "Astra Update Library Python Bindings";

    // Enums
    py::enum_<AstraLogLevel>(m, "LogLevel")
        .value("DEBUG", ASTRA_LOG_LEVEL_DEBUG)
        .value("INFO", ASTRA_LOG_LEVEL_INFO)
        .value("WARNING", ASTRA_LOG_LEVEL_WARNING)
        .value("ERROR", ASTRA_LOG_LEVEL_ERROR)
        .export_values();

    py::enum_<AstraDeviceBootStage>(m, "DeviceBootStage")
        .value("AUTO", ASTRA_DEVICE_BOOT_STAGE_AUTO)
        .value("BOOTLOADER", ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER)
        .value("LINUX", ASTRA_DEVICE_BOOT_STAGE_LINUX)
        .value("M52BL", ASTRA_DEVICE_BOOT_STAGE_M52BL)
        .value("SYSMGR", ASTRA_DEVICE_BOOT_STAGE_SYSMGR)
        .export_values();

    py::enum_<AstraDeviceStatus>(m, "DeviceStatus")
        .value("ADDED", ASTRA_DEVICE_STATUS_ADDED)
        .value("OPENED", ASTRA_DEVICE_STATUS_OPENED)
        .value("CLOSED", ASTRA_DEVICE_STATUS_CLOSED)
        .value("BOOT_START", ASTRA_DEVICE_STATUS_BOOT_START)
        .value("BOOT_PROGRESS", ASTRA_DEVICE_STATUS_BOOT_PROGRESS)
        .value("BOOT_COMPLETE", ASTRA_DEVICE_STATUS_BOOT_COMPLETE)
        .value("BOOT_FAIL", ASTRA_DEVICE_STATUS_BOOT_FAIL)
        .value("UPDATE_START", ASTRA_DEVICE_STATUS_UPDATE_START)
        .value("UPDATE_PROGRESS", ASTRA_DEVICE_STATUS_UPDATE_PROGRESS)
        .value("UPDATE_COMPLETE", ASTRA_DEVICE_STATUS_UPDATE_COMPLETE)
        .value("UPDATE_FAIL", ASTRA_DEVICE_STATUS_UPDATE_FAIL)
        .value("IMAGE_SEND_START", ASTRA_DEVICE_STATUS_IMAGE_SEND_START)
        .value("IMAGE_SEND_PROGRESS", ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS)
        .value("IMAGE_SEND_COMPLETE", ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE)
        .value("IMAGE_SEND_FAIL", ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL)
        .export_values();

    py::enum_<AstraDeviceSeries>(m, "DeviceSeries")
        .value("SL16XX", ASTRA_SERIES_SL16XX)
        .value("SL26XX", ASTRA_SERIES_SL26XX)
        .value("SR1XX", ASTRA_SERIES_SR1XX)
        .export_values();

    py::enum_<AstraDeviceManagerStatus>(m, "DeviceManagerStatus")
        .value("START", ASTRA_DEVICE_MANAGER_STATUS_START)
        .value("INFO", ASTRA_DEVICE_MANAGER_STATUS_INFO)
        .value("FAILURE", ASTRA_DEVICE_MANAGER_STATUS_FAILURE)
        .value("SHUTDOWN", ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN)
        .export_values();

    py::enum_<FlashImageType>(m, "FlashImageType")
        .value("UNKNOWN", FLASH_IMAGE_TYPE_UNKNOWN)
        .value("SPI", FLASH_IMAGE_TYPE_SPI)
        .value("NAND", FLASH_IMAGE_TYPE_NAND)
        .value("EMMC", FLASH_IMAGE_TYPE_EMMC);

    py::enum_<AstraUbootVersion>(m, "UbootVersion")
        .value("UNKNOWN", ASTRA_UBOOT_VERSION_UNKNOWN)
        .value("V2019_10", ASTRA_UBOOT_VERSION_2019_10)
        .value("V2025_01", ASTRA_UBOOT_VERSION_2025_01);

    py::enum_<AstraSecureBootVersion>(m, "SecureBootVersion")
        .value("GEN2", ASTRA_SECURE_BOOT_V2)
        .value("GENX", ASTRA_SECURE_BOOT_V3);

    py::enum_<AstraMemoryLayout>(m, "MemoryLayout")
        .value("GB_1", ASTRA_MEMORY_LAYOUT_1GB)
        .value("GB_2", ASTRA_MEMORY_LAYOUT_2GB)
        .value("GB_3", ASTRA_MEMORY_LAYOUT_3GB)
        .value("GB_4", ASTRA_MEMORY_LAYOUT_4GB)
        .value("MB_512", ASTRA_MEMORY_LAYOUT_512MB);

    py::enum_<AstraMemoryDDRType>(m, "MemoryDDRType")
        .value("NOT_SPECIFIED", ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED)
        .value("DDR3", ASTRA_MEMORY_DDR_TYPE_DDR3)
        .value("DDR4", ASTRA_MEMORY_DDR_TYPE_DDR4)
        .value("LPDDR4", ASTRA_MEMORY_DDR_TYPE_LPDDR4)
        .value("LPDDR4X", ASTRA_MEMORY_DDR_TYPE_LPDDR4X)
        .value("DDR4X16", ASTRA_MEMORY_DDR_TYPE_DDR4X16);

    py::enum_<AstraImageType>(m, "ImageType")
        .value("BOOT", ASTRA_IMAGE_TYPE_BOOT)
        .value("UPDATE_EMMC", ASTRA_IMAGE_TYPE_UPDATE_EMMC)
        .value("UPDATE_SPI", ASTRA_IMAGE_TYPE_UPDATE_SPI)
        .value("UPDATE_NAND", ASTRA_IMAGE_TYPE_UPDATE_NAND);

    py::enum_<AstraTransportType>(m, "TransportType")
        .value("USB", ASTRA_TRANSPORT_USB)
        .value("USB_CDC", ASTRA_TRANSPORT_USB_CDC);

    py::enum_<AstraUbootConsole>(m, "UbootConsole")
        .value("UART", ASTRA_UBOOT_CONSOLE_UART)
        .value("USB", ASTRA_UBOOT_CONSOLE_USB);

    py::enum_<AstraUbootVariant>(m, "UbootVariant")
        .value("UNKNOWN", ASTRA_UBOOT_VARIANT_UNKNOWN)
        .value("UBOOT", ASTRA_UBOOT_VARIANT_UBOOT)
        .value("SYNAPTICS", ASTRA_UBOOT_VARIANT_SYNAPTICS);

    py::class_<Image>(m, "Image")
        .def("load", &Image::Load, py::call_guard<py::gil_scoped_release>())
        .def("get_name", &Image::GetName)
        .def("get_path", &Image::GetPath)
        .def("get_size", &Image::GetSize)
        .def("get_image_type", &Image::GetImageType);

    // DeviceResponse struct
    py::class_<DeviceResponse>(m, "DeviceResponse")
        .def(py::init<>())
        .def_readwrite("device_name", &DeviceResponse::m_deviceName)
        .def_readwrite("status", &DeviceResponse::m_status)
        .def_readwrite("progress", &DeviceResponse::m_progress)
        .def_readwrite("image_name", &DeviceResponse::m_imageName)
        .def_readwrite("message", &DeviceResponse::m_message);

    // ManagerResponse struct
    py::class_<ManagerResponse>(m, "ManagerResponse")
        .def(py::init<>())
        .def_readwrite("manager_status", &ManagerResponse::m_managerStatus)
        .def_readwrite("manager_message", &ManagerResponse::m_managerMessage);

    // AstraDeviceManagerResponse class
    py::class_<AstraDeviceManagerResponse>(m, "DeviceManagerResponse")
        .def(py::init<ManagerResponse>())
        .def(py::init<DeviceResponse>())
        .def("is_device_manager_response", &AstraDeviceManagerResponse::IsDeviceManagerResponse)
        .def("is_device_response", &AstraDeviceManagerResponse::IsDeviceResponse)
        .def("get_device_manager_response", &AstraDeviceManagerResponse::GetDeviceManagerResponse)
        .def("get_device_response", &AstraDeviceManagerResponse::GetDeviceResponse);

    // AstraDevice static methods
    py::class_<AstraDevice>(m, "AstraDevice")
        .def_static("status_to_string", &AstraDevice::AstraDeviceStatusToString)
        .def_static("series_to_string", &AstraDevice::AstraDeviceSeriesToString)
        .def_static("boot_stage_from_string", &AstraDevice::BootStageFromString);

    // AstraDeviceManager class
    py::class_<AstraDeviceManager, std::shared_ptr<AstraDeviceManager>>(m, "AstraDeviceManager")
        .def(py::init([](bool continuous,
                         AstraLogLevel logLevel,
                         const std::string& logPath,
                         const std::string& tempDir,
                         const std::string& filterPorts,
                        bool usbDebug,
                        bool keepImageRequestLoopAfterBoot,
                        bool leaveFastbootIdle) {
                return std::make_shared<AstraDeviceManager>(
                    ResponseQueueCallback, continuous, logLevel, logPath, tempDir, filterPorts,
                    usbDebug, keepImageRequestLoopAfterBoot, leaveFastbootIdle);
             }),
            py::arg("continuous") = false,
            py::arg("log_level") = ASTRA_LOG_LEVEL_WARNING,
            py::arg("log_path") = "",
            py::arg("temp_dir") = "",
            py::arg("filter_ports") = "",
            py::arg("usb_debug") = false,
            py::arg("keep_image_request_loop_after_boot") = false,
            py::arg("leave_fastboot_idle") = false,
            "Initialize AstraDeviceManager with automatic response queue callback")
        .def("boot", &AstraDeviceManager::Boot,
            py::arg("boot_images_path"),
            py::arg("boot_command") = "",
            py::arg("boot_stage") = ASTRA_DEVICE_BOOT_STAGE_AUTO,
            py::call_guard<py::gil_scoped_release>(),
            "Boot devices with the specified boot images")
        .def("update", &AstraDeviceManager::Update,
            py::arg("flash_image"),
            py::arg("boot_image_path"),
            py::call_guard<py::gil_scoped_release>(),
            "Update devices with the specified flash image")
        .def("shutdown", &AstraDeviceManager::Shutdown,
            py::call_guard<py::gil_scoped_release>(),
            "Shutdown the device manager")
        .def("get_log_file", &AstraDeviceManager::GetLogFile,
            "Get the path to the log file")
        .def_static("get_version", &AstraDeviceManager::GetVersion,
            "Get the version of the device manager library")
        .def("get_response", [](AstraDeviceManager& self) -> py::object {
            AstraDeviceManagerResponse response = AstraDeviceManagerResponse(ManagerResponse());
            if (ResponseQueue::GetInstance().Get(response)) {
                return py::cast(response);
            }
            return py::none();
        }, "Get a response from the queue (non-blocking), returns None if empty")
        .def("get_responses", [](AstraDeviceManager& self) -> py::list {
            py::list responses;
            AstraDeviceManagerResponse response = AstraDeviceManagerResponse(ManagerResponse());
            while (ResponseQueue::GetInstance().Get(response)) {
                responses.append(response);
            }
            return responses;
        }, "Get all responses from the queue (non-blocking)")
        .def("has_responses", [](AstraDeviceManager& self) -> bool {
            return !ResponseQueue::GetInstance().IsEmpty();
        }, "Check if there are any responses in the queue")
        .def("response_queue_size", [](AstraDeviceManager& self) -> size_t {
            return ResponseQueue::GetInstance().Size();
        }, "Get the number of responses in the queue");

    // FlashImage static factory methods
    py::class_<FlashImage, std::shared_ptr<FlashImage>>(m, "FlashImage")
        .def("load", &FlashImage::Load, py::call_guard<py::gil_scoped_release>())
        .def("get_load_error", &FlashImage::GetLoadError)
        .def("get_boot_image_id", &FlashImage::GetBootImageId)
        .def("get_chip_name", &FlashImage::GetChipName)
        .def("get_board_name", &FlashImage::GetBoardName)
        .def("get_flash_command", &FlashImage::GetFlashCommand)
        .def("get_final_image", &FlashImage::GetFinalImage)
        .def("get_secure_boot_version", &FlashImage::GetSecureBootVersion)
        .def("get_memory_layout", &FlashImage::GetMemoryLayout)
        .def("get_memory_ddr_type", &FlashImage::GetMemoryDDRType)
        .def("get_flash_image_type", &FlashImage::GetFlashImageType)
        .def("get_reset_when_complete", &FlashImage::GetResetWhenComplete)
        .def("get_uboot_version", &FlashImage::GetUbootVersion)
        .def("get_images", &FlashImage::GetImages)
        .def("set_uboot_version", &FlashImage::SetUbootVersion)
        .def_static("flash_image_factory",
            [](const std::string& imagePath,
               std::map<std::string, std::string> config,
               const std::string& manifest = "") -> std::shared_ptr<FlashImage> {
                return FlashImage::FlashImageFactory(imagePath, config, manifest);
            },
            py::arg("image_path"),
            py::arg("config") = std::map<std::string, std::string>{},
            py::arg("manifest") = "",
            "Factory method to create a FlashImage from file.\n"
            "config is a dict of key/value overrides, e.g.:\n"
            "  board, chip, image_type, boot_image, secure_boot, memory_layout, ddr_type, reset");

    py::class_<AstraBootImage, std::shared_ptr<AstraBootImage>>(m, "AstraBootImage")
        .def(py::init<std::string>(), py::arg("path") = "")
        .def("load", &AstraBootImage::Load, py::call_guard<py::gil_scoped_release>())
        .def("get_vendor_id", &AstraBootImage::GetVendorId)
        .def("get_product_id", &AstraBootImage::GetProductId)
        .def("get_vendor_product_id_pairs", &AstraBootImage::GetVendorProductIdPairs)
        .def("get_sysmgr_vendor_id", &AstraBootImage::GetSysMgrVendorId)
        .def("get_sysmgr_product_id", &AstraBootImage::GetSysMgrProductId)
        .def("get_fastboot_vendor_id", &AstraBootImage::GetFastbootVendorId)
        .def("get_fastboot_product_id", &AstraBootImage::GetFastbootProductId)
        .def("get_chip_name", &AstraBootImage::GetChipName)
        .def("get_board_name", &AstraBootImage::GetBoardName)
        .def("get_id", &AstraBootImage::GetID)
        .def("get_uenv_support", &AstraBootImage::GetUEnvSupport)
        .def("get_nand_support", &AstraBootImage::GetNandSupport)
        .def("get_secure_boot_version", &AstraBootImage::GetSecureBootVersion)
        .def("get_uboot_console", &AstraBootImage::GetUbootConsole)
        .def("get_memory_layout", &AstraBootImage::GetMemoryLayout)
        .def("get_memory_ddr_type", &AstraBootImage::GetMemoryDDRType)
        .def("get_transport_type", &AstraBootImage::GetTransportType)
        .def("get_images", &AstraBootImage::GetImages)
        .def("get_uboot_variant", &AstraBootImage::GetUbootVariant)
        .def("get_uboot_version", &AstraBootImage::GetUbootVersion)
        .def("get_final_boot_image", &AstraBootImage::GetFinalBootImage)
        .def("is_linux_boot", &AstraBootImage::IsLinuxBoot)
        .def("get_default_boot_stage", &AstraBootImage::GetDefaultBootStage);

    // ResponseQueue access for Python
    py::class_<ResponseQueue>(m, "ResponseQueue")
        .def_static("get_instance", &ResponseQueue::GetInstance, py::return_value_policy::reference)
        .def("get", [](ResponseQueue& self) -> py::object {
            AstraDeviceManagerResponse response = AstraDeviceManagerResponse(ManagerResponse());
            if (self.Get(response)) {
                return py::cast(response);
            }
            return py::none();
        }, "Get a response from the queue (non-blocking)")
        .def("is_empty", &ResponseQueue::IsEmpty)
        .def("size", &ResponseQueue::Size);
}
