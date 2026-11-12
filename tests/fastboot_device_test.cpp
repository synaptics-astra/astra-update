// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "fastboot_device.hpp"
#include "scripted_usb_device.hpp"
#include "test_support.hpp"

#include <iomanip>
#include <sstream>

namespace {

std::string Text(const std::vector<uint8_t> &bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

std::string HexSize(size_t size)
{
    std::ostringstream result;
    result << std::hex << std::setw(8) << std::setfill('0') << size;
    return result.str();
}

void QueueOkay(test::ScriptedUSBDevice &device, const std::string &message = "")
{
    device.QueueRead("OKAY" + message);
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("open-close-reopen-and-disconnect-callback", [] {
        test::ScriptedUSBDevice usb;
        int disconnects = 0;
        FastBootDevice fastboot(&usb);
        REQUIRE(fastboot.Open([&] { ++disconnects; }));
        REQUIRE(fastboot.Open());
        REQUIRE(usb.OpenCount() == 1);
        REQUIRE(!fastboot.IsDisconnected());
        usb.Emit(USBDevice::USB_DEVICE_EVENT_INTERRUPT);
        REQUIRE(!fastboot.IsDisconnected() && disconnects == 0);
        usb.Emit(USBDevice::USB_DEVICE_EVENT_NO_DEVICE);
        REQUIRE(fastboot.IsDisconnected() && disconnects == 1);
        fastboot.Close();
        fastboot.Close();
        REQUIRE(usb.CloseCount() == 0);
        REQUIRE(fastboot.Open([&] { ++disconnects; }));
        REQUIRE(usb.OpenCount() == 2 && !fastboot.IsDisconnected());
        usb.Emit(USBDevice::USB_DEVICE_EVENT_TRANSFER_CANCELED);
        usb.Emit(USBDevice::USB_DEVICE_EVENT_TRANSFER_ERROR);
        REQUIRE(fastboot.IsDisconnected() && disconnects == 3);
    });
    suite.Add("open-failure-and-retry", [] {
        test::ScriptedUSBDevice usb;
        usb.SetOpenResult(-1);
        FastBootDevice fastboot(&usb);
        REQUIRE(!fastboot.Open());
        REQUIRE(!fastboot.IsDisconnected());
        usb.SetOpenResult(0);
        REQUIRE(fastboot.Open());
        REQUIRE(usb.OpenCount() == 2);
    });
    suite.Add("getvar-info-okay-and-timeout-forwarding", [] {
        test::ScriptedUSBDevice usb;
        usb.QueueRead("INFOloading");
        usb.QueueRead("OKAYserial-123");
        FastBootDevice fastboot(&usb);
        std::string value = "unchanged";
        REQUIRE(fastboot.GetVar("serialno", value, 3210));
        REQUIRE(value == "serial-123");
        const auto writes = usb.Writes();
        REQUIRE(writes.size() == 1 && Text(writes[0]) == "getvar:serialno");
        REQUIRE(usb.ReadTimeouts() == std::vector<int>({3210, 3210}));
    });
    suite.Add("getvar-fail-short-read-and-unknown-status", [] {
        for (const std::string response : {"FAILnot-found", "OK", "WHATunknown"}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead(response);
            FastBootDevice fastboot(&usb);
            std::string value = "unchanged";
            REQUIRE(!fastboot.GetVar("missing", value));
            REQUIRE(value == "unchanged");
        }
    });
    suite.Add("getvar-read-error-and-zero-length-response", [] {
        for (const auto response : {test::ScriptedUSBDevice::ReadResult{{}, -7, 0},
            test::ScriptedUSBDevice::ReadResult{{}, 0, 0}}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead(response);
            FastBootDevice fastboot(&usb);
            std::string value;
            REQUIRE(!fastboot.GetVar("serialno", value));
        }
    });
    for (const auto length : {size_t{56}, size_t{57}, size_t{58}}) {
        suite.Add("command-boundary-" + std::to_string(length + 7), [=] {
            test::ScriptedUSBDevice usb;
            QueueOkay(usb, "value");
            FastBootDevice fastboot(&usb);
            std::string value;
            REQUIRE(fastboot.GetVar(std::string(length, 'x'), value) == (length != 58));
            const auto writes = usb.Writes();
            REQUIRE(writes.size() == (length == 58 ? 0u : 1u));
            if (!writes.empty()) {
                REQUIRE(writes.front().size() == length + 7);
            }
        });
    }
    suite.Add("command-short-write-rejected", [] {
        for (const test::ScriptedUSBDevice::WriteResult writeResult : {
            test::ScriptedUSBDevice::WriteResult{-1, 0},
            test::ScriptedUSBDevice::WriteResult{0, 7}}) {
            test::ScriptedUSBDevice usb;
            usb.QueueWriteResult(writeResult);
            FastBootDevice fastboot(&usb);
            std::string value;
            REQUIRE(!fastboot.GetVar("serialno", value));
            REQUIRE(usb.ReadTimeouts().empty());
        }
    });
    suite.Add("response-over-buffer-and-negative-length-rejected", [] {
        for (const auto response : {
            test::ScriptedUSBDevice::ReadResult{std::vector<uint8_t>(65, 'A'), 0, 65},
            test::ScriptedUSBDevice::ReadResult{{'O', 'K', 'A', 'Y'}, 0, -1}}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead(response);
            FastBootDevice fastboot(&usb);
            std::string value;
            REQUIRE(!fastboot.GetVar("serialno", value));
        }
    });
    suite.Add("oem-okay-info-and-failure", [] {
        test::ScriptedUSBDevice usb;
        usb.QueueRead("INFOworking");
        QueueOkay(usb);
        usb.QueueRead("FAILrejected");
        FastBootDevice fastboot(&usb);
        REQUIRE(fastboot.Oem("flash"));
        REQUIRE(!fastboot.Oem("bad"));
        const auto writes = usb.Writes();
        REQUIRE(writes.size() == 2);
        REQUIRE(Text(writes[0]) == "oem flash" && Text(writes[1]) == "oem bad");
    });
    suite.Add("oem-no-wait-and-reboot", [] {
        test::ScriptedUSBDevice usb;
        FastBootDevice fastboot(&usb);
        REQUIRE(fastboot.OemNoWait("fb_exit"));
        REQUIRE(usb.ReadTimeouts().empty());
        QueueOkay(usb);
        REQUIRE(fastboot.Reboot(123));
        REQUIRE(usb.ReadTimeouts() == std::vector<int>({123}));
        const auto writes = usb.Writes();
        REQUIRE(writes.size() == 2);
        REQUIRE(Text(writes[0]) == "oem fb_exit" && Text(writes[1]) == "reboot");
    });
    suite.Add("reboot-requires-acknowledgement", [] {
        for (const std::string response : {"FAILdenied", "OK", ""}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead(response);
            FastBootDevice fastboot(&usb);
            REQUIRE(!fastboot.Reboot());
        }
    });
    suite.Add("oem-and-no-wait-length-limits", [] {
        test::ScriptedUSBDevice usb;
        QueueOkay(usb);
        FastBootDevice fastboot(&usb);
        REQUIRE(fastboot.OemNoWait(std::string(60, 'x')));
        REQUIRE(!fastboot.OemNoWait(std::string(61, 'x')));
        REQUIRE(fastboot.Oem(std::string(60, 'y')));
        REQUIRE(!fastboot.Oem(std::string(61, 'y')));
        REQUIRE(usb.Writes().size() == 2);
    });
    suite.Add("probe-serial-leaves-open-device-owned-by-caller", [] {
        test::ScriptedUSBDevice usb;
        usb.QueueRead("OKAYboard-serial");
        std::string serial;
        REQUIRE(FastBootDevice::ProbeSerial(&usb, serial));
        REQUIRE(serial == "board-serial");
        REQUIRE(usb.OpenCount() == 1 && usb.CloseCount() == 0);
        REQUIRE(Text(usb.Writes().front()) == "getvar:serialno");
    });
    suite.Add("probe-serial-open-failure", [] {
        test::ScriptedUSBDevice usb;
        usb.SetOpenResult(-1);
        std::string serial;
        REQUIRE(!FastBootDevice::ProbeSerial(&usb, serial));
        REQUIRE(usb.Writes().empty() && usb.CloseCount() == 0);
    });
    suite.Add("stage-empty-file", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("empty.bin", "");
        test::ScriptedUSBDevice usb;
        usb.QueueRead("DATA00000000");
        QueueOkay(usb);
        FastBootDevice fastboot(&usb);
        std::vector<std::pair<size_t, size_t>> progress;
        REQUIRE(fastboot.StageFile(path.string(), [&](size_t sent, size_t total) {
            progress.emplace_back(sent, total);
        }, 998));
        REQUIRE(usb.Writes().size() == 1);
        REQUIRE(Text(usb.Writes().front()) == "download:00000000");
        REQUIRE(progress.empty());
        REQUIRE(usb.ReadTimeouts() == std::vector<int>({998, 998}));
    });
    for (const size_t size : {4 * 1024 * 1024 - 1, 4 * 1024 * 1024, 4 * 1024 * 1024 + 1}) {
        suite.Add("stage-chunk-boundary-" + std::to_string(size), [=] {
            test::TempDirectory directory;
            const std::string payload(size, 'P');
            const auto path = directory.Write("payload.bin", payload);
            test::ScriptedUSBDevice usb;
            usb.QueueRead("DATA" + HexSize(size));
            QueueOkay(usb);
            FastBootDevice fastboot(&usb);
            std::vector<std::pair<size_t, size_t>> progress;
            REQUIRE(fastboot.StageFile(path.string(), [&](size_t sent, size_t total) {
                progress.emplace_back(sent, total);
            }));
            const auto writes = usb.Writes();
            REQUIRE(writes.size() == (size > 4 * 1024 * 1024 ? 3u : 2u));
            REQUIRE(Text(writes.front()) == "download:" + HexSize(size));
            std::string transferred;
            for (size_t i = 1; i < writes.size(); ++i) {
                transferred.append(writes[i].begin(), writes[i].end());
            }
            REQUIRE(transferred == payload);
            REQUIRE(progress.size() == writes.size() - 1);
            REQUIRE(progress.back() == std::make_pair(size, size));
            for (size_t i = 1; i < progress.size(); ++i) {
                REQUIRE(progress[i].first > progress[i - 1].first);
            }
        });
    }
    suite.Add("stage-missing-file-does-not-send-command", [] {
        test::TempDirectory directory;
        test::ScriptedUSBDevice usb;
        FastBootDevice fastboot(&usb);
        REQUIRE(!fastboot.StageFile((directory.Path() / "missing.bin").string()));
        REQUIRE(usb.Writes().empty());
    });
    suite.Add("stage-device-rejects-download-command", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "payload");
        for (const std::string response : {"FAILdenied", "OKAYunexpected", "INFOonly"}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead(response);
            FastBootDevice fastboot(&usb);
            REQUIRE(!fastboot.StageFile(path.string()));
            REQUIRE(usb.Writes().size() == 1);
        }
    });
    const std::vector<std::string> malformedData = {
        "", "0", "0000000", "000000001", "0000000g", "0000000 ", "0000000x"
    };
    for (size_t i = 0; i < malformedData.size(); ++i) {
        suite.Add("stage-malformed-data-size-" + std::to_string(i), [value = malformedData[i]] {
            test::TempDirectory directory;
            const auto path = directory.Write("file.bin", "payload");
            test::ScriptedUSBDevice usb;
            usb.QueueRead("DATA" + value);
            FastBootDevice fastboot(&usb);
            REQUIRE(!fastboot.StageFile(path.string()));
            REQUIRE(usb.Writes().size() == 1);
        });
    }
    suite.Add("stage-accepted-size-mismatch", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "payload");
        test::ScriptedUSBDevice usb;
        usb.QueueRead("DATA00000008");
        FastBootDevice fastboot(&usb);
        REQUIRE(!fastboot.StageFile(path.string()));
        REQUIRE(usb.Writes().size() == 1);
    });
    suite.Add("stage-data-write-negative-and-short", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "payload");
        for (const test::ScriptedUSBDevice::WriteResult writeResult : {
            test::ScriptedUSBDevice::WriteResult{-1, 0},
            test::ScriptedUSBDevice::WriteResult{0, 2}}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead("DATA00000007");
            usb.SetOnWrite([&usb, writeResult](const std::vector<uint8_t> &write) {
                if (Text(write).find("download:") == 0) {
                    usb.QueueWriteResult(writeResult);
                }
            });
            FastBootDevice fastboot(&usb);
            REQUIRE(!fastboot.StageFile(path.string()));
            REQUIRE(usb.Writes().size() == 2);
        }
    });
    suite.Add("stage-disconnect-during-data-write", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "payload");
        test::ScriptedUSBDevice usb;
        usb.QueueRead("DATA00000007");
        usb.SetOnWrite([&usb](const std::vector<uint8_t> &write) {
            if (Text(write).find("download:") == 0) {
                usb.QueueWriteResult({-1, 0});
            } else {
                usb.Emit(USBDevice::USB_DEVICE_EVENT_NO_DEVICE);
            }
        });
        FastBootDevice fastboot(&usb);
        REQUIRE(fastboot.Open());
        REQUIRE(!fastboot.StageFile(path.string()));
        REQUIRE(fastboot.IsDisconnected());
        REQUIRE(usb.Writes().size() == 2);
    });
    suite.Add("stage-final-ack-fail-short-and-read-error", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "data");
        for (const auto &response : {test::ScriptedUSBDevice::ReadResult{
                std::vector<uint8_t>({'F', 'A', 'I', 'L', 'n', 'o'}), 0, -1},
            test::ScriptedUSBDevice::ReadResult{{'O', 'K', 'A'}, 0, 3},
            test::ScriptedUSBDevice::ReadResult{{'W', 'H', 'A', 'T'}, 0, -1},
            test::ScriptedUSBDevice::ReadResult{{}, -1, 0}}) {
            test::ScriptedUSBDevice usb;
            usb.QueueRead("DATA00000004");
            usb.QueueRead(response);
            FastBootDevice fastboot(&usb);
            REQUIRE(!fastboot.StageFile(path.string()));
        }
    });
    suite.Add("stage-file-removed-after-stat-fails-to-open", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "payload");
        test::ScriptedUSBDevice usb;
        usb.QueueRead("DATA00000007");
        usb.SetOnWrite([&](const std::vector<uint8_t> &write) {
            if (Text(write).find("download:") == 0) {
                std::filesystem::remove(path);
            }
        });
        FastBootDevice fastboot(&usb);
        REQUIRE(!fastboot.StageFile(path.string()));
        REQUIRE(usb.Writes().size() == 1);
    });
    suite.Add("stage-file-shrunk-after-stat-rejects-truncation", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "payload");
        test::ScriptedUSBDevice usb;
        usb.QueueRead("DATA00000007");
        usb.SetOnWrite([&](const std::vector<uint8_t> &write) {
            if (Text(write).find("download:") == 0) {
                std::filesystem::resize_file(path, 2);
            }
        });
        FastBootDevice fastboot(&usb);
        REQUIRE(!fastboot.StageFile(path.string()));
        REQUIRE(usb.Writes().size() == 2);
        REQUIRE(Text(usb.Writes().back()) == "pa");
    });
    return suite.Run(argc, argv);
}
