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

PYBIND11_MODULE(astra_update, m) {
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
        .def(py::init<bool, AstraLogLevel, const std::string&, const std::string&, const std::string&, bool>(),
            py::arg("continuous") = false,
            py::arg("log_level") = ASTRA_LOG_LEVEL_WARNING,
            py::arg("log_path") = "",
            py::arg("temp_dir") = "",
            py::arg("filter_ports") = "",
            py::arg("usb_debug") = false,
            "Initialize AstraDeviceManager with automatic response queue callback")
        .def("boot", &AstraDeviceManager::Boot,
            py::arg("boot_images_path"),
            py::arg("boot_command") = "",
            py::arg("boot_stage") = ASTRA_DEVICE_BOOT_STAGE_AUTO,
            "Boot devices with the specified boot images")
        .def("update", &AstraDeviceManager::Update,
            py::arg("flash_image"),
            py::arg("boot_image_path"),
            "Update devices with the specified flash image")
        .def("shutdown", &AstraDeviceManager::Shutdown,
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
