// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "scripted_usb_device.hpp"
#include "test_support.hpp"

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("queued-event-owns-data-and-runs-off-producer-thread", [] {
        test::CallbackWorkerUSBDevice usb;
        std::mutex mutex;
        std::condition_variable changed;
        bool received = false;
        USBDevice::USBEvent observedEvent = USBDevice::USB_DEVICE_EVENT_NO_DEVICE;
        std::vector<uint8_t> observed;
        std::thread::id callbackThread;
        const auto producerThread = std::this_thread::get_id();
        usb.Open([&](USBDevice::USBEvent event, uint8_t *data, size_t size) {
            std::lock_guard<std::mutex> lock(mutex);
            observedEvent = event;
            observed.assign(data, data + size);
            callbackThread = std::this_thread::get_id();
            received = true;
            changed.notify_all();
        });
        std::vector<uint8_t> source{1, 2, 3, 0xFE};
        usb.StartWorker();
        usb.QueueEvent(USBDevice::USB_DEVICE_EVENT_INTERRUPT, source);
        source.assign(source.size(), 0);
        {
            std::unique_lock<std::mutex> lock(mutex);
            REQUIRE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return received; }));
        }
        usb.StopWorker();
        REQUIRE(observedEvent == USBDevice::USB_DEVICE_EVENT_INTERRUPT);
        REQUIRE(observed == std::vector<uint8_t>({1, 2, 3, 0xFE}));
        REQUIRE(callbackThread != producerThread);
    });
    suite.Add("fifo-actions-callback-outside-queue-lock", [] {
        test::CallbackWorkerUSBDevice usb;
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<int> sequence;
        std::thread::id actionThread;
        const auto producerThread = std::this_thread::get_id();
        usb.Open([&](USBDevice::USBEvent, uint8_t *, size_t) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                sequence.push_back(1);
            }
            usb.QueueAction([&] {
                std::lock_guard<std::mutex> lock(mutex);
                sequence.push_back(4);
                changed.notify_all();
            });
        });
        usb.StartWorker();
        usb.QueueEvent(USBDevice::USB_DEVICE_EVENT_INTERRUPT, {});
        usb.QueueAction([&] {
            std::lock_guard<std::mutex> lock(mutex);
            actionThread = std::this_thread::get_id();
            sequence.push_back(2);
        });
        usb.QueueAction([&] {
            std::lock_guard<std::mutex> lock(mutex);
            sequence.push_back(3);
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            REQUIRE(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return sequence.size() == 4;
            }));
        }
        usb.StopWorker();
        REQUIRE(sequence == std::vector<int>({1, 2, 3, 4}));
        REQUIRE(actionThread != producerThread);
    });
    suite.Add("worker-start-stop-is-idempotent-and-restartable", [] {
        test::CallbackWorkerUSBDevice usb;
        usb.StartWorker();
        usb.StartWorker();
        usb.StopWorker();
        usb.StopWorker();
        usb.StartWorker();
        usb.StopWorker();
    });
    suite.Add("shutdown-drains-queued-work-before-returning", [] {
        test::CallbackWorkerUSBDevice usb;
        std::mutex mutex;
        std::vector<int> completed;
        usb.StartWorker();
        for (int i = 0; i < 32; ++i) {
            usb.QueueAction([&, i] {
                std::lock_guard<std::mutex> lock(mutex);
                completed.push_back(i);
            });
        }
        usb.StopWorker();
        REQUIRE(completed.size() == 32);
        for (int i = 0; i < 32; ++i) {
            REQUIRE(completed[static_cast<size_t>(i)] == i);
        }
        usb.QueueAction([&] {
            std::lock_guard<std::mutex> lock(mutex);
            completed.push_back(32);
        });
        REQUIRE(completed.size() == 32);
    });
    suite.Add("empty-actions-are-ignored", [] {
        test::CallbackWorkerUSBDevice usb;
        usb.StartWorker();
        usb.QueueAction(nullptr);
        usb.StopWorker();
    });
    return suite.Run(argc, argv);
}
