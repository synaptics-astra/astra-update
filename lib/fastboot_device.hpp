// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "usb_device.hpp"

/**
 * FastBootDevice wraps a USBDevice that speaks the Android fastboot wire
 * protocol over bulk USB.  It does NOT own the USBDevice; the caller is
 * responsible for keeping it alive for the lifetime of this object.
 *
 * The device must already have been opened (USBDevice::Open called) before
 * constructing this object.
 */
class FastBootDevice {
public:
    explicit FastBootDevice(USBDevice *usbDevice);
    ~FastBootDevice();

    /**
     * Open the device for fastboot communication.  Must be called before any
     * other method.
     *
     * @param disconnectCallback  Called with (true) when the USB device
     *                            disconnects while the fastboot session is
     *                            active.  May be nullptr.
     * @return true on success, false on failure.
     */
    bool Open(std::function<void()> disconnectCallback = nullptr);
    void Close();

    /**
     * Query a variable from the device.
     * Sends "getvar:<name>" and waits for OKAY response.
     *
     * @param name     Variable name.
     * @param value    Receives the variable value on success.
     * @param timeoutMs  Read timeout in milliseconds.
     * @return true on success.
     */
    bool GetVar(const std::string &name, std::string &value, int timeoutMs = 5000);

    /**
     * Download (stage) a file to the device.
     * Sends "download:<size-as-8-hex>" then bulk-writes the file data.
     *
     * @param path        Absolute path to the file to send.
     * @param progressCb  Optional progress callback(bytesSent, totalBytes).
     * @param timeoutMs   Per-response timeout in milliseconds.
     * @return true on success.
     */
    bool StageFile(const std::string &path,
        std::function<void(size_t, size_t)> progressCb = nullptr,
        int timeoutMs = 30000);

    /**
     * Send an OEM command ("oem <command>").
     * @return true if the device responds with OKAY.
     */
    bool Oem(const std::string &command, int timeoutMs = 10000);

    /**
     * Send an OEM command without waiting for a response.
     * Use when the device resets immediately after receiving the command
     * (e.g. fb_exit, which causes U-Boot to exit its staging loop and
     * reset the USB before it can send OKAY).
     * @return true if the command was sent successfully.
     */
    bool OemNoWait(const std::string &command);

    /** @return true if the underlying USB device has disconnected. */
    bool IsDisconnected() const { return m_disconnected; }

    /**
     * Probe the fastboot serial number of a freshly-arrived USB device
     * without permanently opening it.  Opens the device, sends
     * "getvar:serialno", reads the reply, then leaves the device open
     * (the caller or the next FastBootDevice::Open() will install the
     * real event callback).  Does NOT call USBDevice::Close().
     *
     * @param device  A USBDevice that has NOT yet had Open() called on it.
     * @param out     Receives the serial string on success.
     * @return true if the serial was read successfully.
     */
    static bool ProbeSerial(USBDevice *device, std::string &out);

private:
    USBDevice *m_usbDevice = nullptr;
    bool m_opened = false;
    bool m_disconnected = false;
    std::function<void()> m_disconnectCallback;

    static constexpr size_t kCmdBufferSize = 64;
    static constexpr size_t kRespBufferSize = 64;
    static constexpr size_t kDownloadChunkSize = 1 * 1024 * 1024; // 1 MiB

    /**
     * Send a raw ASCII fastboot command (no more than 64 bytes).
     * @return true on success.
     */
    bool SendCommand(const std::string &command);

    /**
     * Read one fastboot response packet.
     * Fastboot responses are at most 64 bytes: 4-byte status + message.
     *
     * @param status   Receives the 4-char status code: "OKAY", "FAIL",
     *                 "INFO", or "DATA".
     * @param message  Receives the rest of the response (may be empty).
     * @param timeoutMs  Read timeout.
     * @return true on success; false on read error or disconnect.
     */
    bool ReadResponse(std::string &status, std::string &message, int timeoutMs = 5000);

    /**
     * Send a command and collect responses, looping past INFO packets.
     * Stops on OKAY, FAIL, DATA, or error.
     *
     * @param command    Fastboot command string to send.
     * @param dataSize   Receives the size field when the response is DATA.
     * @param timeoutMs  Per-response read timeout.
     * @return The final status string ("OKAY", "FAIL", "DATA", or "" on
     *         error).
     */
    std::string ExecuteCommand(const std::string &command, uint32_t &dataSize, int timeoutMs);

    void USBEventHandler(USBDevice::USBEvent event, uint8_t *buf, size_t size);
};
