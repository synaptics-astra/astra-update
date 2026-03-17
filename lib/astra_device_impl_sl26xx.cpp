// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "astra_device_impl_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "astra_boot_image.hpp"
#include "usb_cdc_device.hpp"
#include <zlib.h>

namespace {

constexpr uint8_t kHostSync1 = 0x5B;
constexpr uint8_t kHostSync2 = 0x5A;
constexpr size_t kHostHeaderSize = 8;
constexpr size_t kOpHeaderSize = 32;

constexpr uint8_t kServiceIdBoot = 0x33;
constexpr uint8_t kHostApiServiceId = 0x0D;

constexpr uint8_t kOpcodeVersion = 0x0A;
constexpr uint8_t kOpcodeRunImage = 0x0B;
constexpr uint8_t kOpcodeExec0C = 0x0C;
constexpr uint8_t kOpcodeEmmcOp = 0x0F;
constexpr uint8_t kOpcodeUpload = 0x12;

constexpr uint8_t kOpcodeUploadKey = 0x01;
constexpr uint8_t kOpcodeUploadSpk = 0x02;
constexpr uint8_t kOpcodeUploadM52Bl = 0x04;

constexpr uint8_t kHostApiOpcodeGeneric = 0x12;
constexpr uint8_t kHostApiOpcodeEmmc = 0x0F;
constexpr uint8_t kHostApiOpcodeVersion = 0x0A;
constexpr uint8_t kHostApiOpcodeExec = 0x0C;

constexpr uint32_t kAddrSmLoad = 0xB4A00000;
constexpr uint32_t kAddrAcLoad = 0xBA100000;

constexpr uint32_t kImgTypeBl = 0x00020017;
constexpr uint32_t kImgTypeSm = 0x00000012;
constexpr uint32_t kImgTypeGpt = 0x00000010;
constexpr uint32_t kImgTypeOptee = 0x00020014;

constexpr uint32_t kBlockSize = 512;
constexpr uint64_t kMbSize = 1024ULL * 1024ULL;
constexpr uint64_t kChunkSizeMb = 32;
constexpr uint64_t kLargeFileThresholdMb = 100;
constexpr size_t kStreamChunkSize = 3 * 1024 * 1024;

constexpr uint32_t kPartEntries = 128;
constexpr uint32_t kPartEntrySize = 128;
constexpr uint32_t kGptTableSize = 0x4000;
constexpr uint32_t kGptHeaderSize = 92;
constexpr uint32_t kGptRevision = 0x00010000;

const std::array<uint8_t, 16> kPartTypeGuidCanonical = {
    0xEB, 0xD0, 0xA0, 0xA2, 0xB9, 0xE5, 0x44, 0x33,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
};

enum M52BLReturnCode {
    M52BL_RC_SUCCESS             = 0x00000000,
    M52BL_RC_FAILURE             = 0x00000001,
    M52BL_RC_INVALID_HEADER      = 0x00000002,
    M52BL_RC_MEMORY_OUT_OF_RANGE = 0x00000003,
    M52BL_RC_RX_INCOMPLETE       = 0x00000004,
    M52BL_RC_VERIFY_FAILED       = 0x00000005,
    M52BL_RC_LOAD_ON_MEM_FAILED  = 0x00000006,
    M52BL_RC_INVALID_OPCODE      = 0x00000007,
};

enum class SL26XXDeviceMode {
    SL26XX_DEVICE_MODE_UNKNOWN,
    SL26XX_DEVICE_MODE_BOOTROM,
    SL26XX_DEVICE_MODE_M52BL,
    SL26XX_DEVICE_MODE_SYSMGR,
};

struct EmmcPartitionDesc {
    std::string name;
    uint64_t startMb;
    uint64_t sizeMb;
};

std::string Trim(const std::string &value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> SplitByComma(const std::string &line)
{
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
        parts.push_back(Trim(token));
    }
    return parts;
}

void AppendU32LE(std::vector<uint8_t> &buffer, uint32_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendU64LE(std::vector<uint8_t> &buffer, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

uint32_t ReadU32LE(const uint8_t *ptr)
{
    return static_cast<uint32_t>(ptr[0]) |
        (static_cast<uint32_t>(ptr[1]) << 8) |
        (static_cast<uint32_t>(ptr[2]) << 16) |
        (static_cast<uint32_t>(ptr[3]) << 24);
}

uint32_t ComputeCRC32(const uint8_t *data, size_t length)
{
    static std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t = {};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (size_t j = 0; j < 8; ++j) {
                c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::array<uint8_t, 16> MakeRandomUuidCanonical()
{
    std::array<uint8_t, 16> uuid = {};

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);

    for (auto &b : uuid) {
        b = static_cast<uint8_t>(dist(gen));
    }

    // UUIDv4
    uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0F) | 0x40);
    uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3F) | 0x80);
    return uuid;
}

std::array<uint8_t, 16> UuidToGptBytes(const std::array<uint8_t, 16> &uuidCanonical)
{
    std::array<uint8_t, 16> out = {};

    out[0] = uuidCanonical[3];
    out[1] = uuidCanonical[2];
    out[2] = uuidCanonical[1];
    out[3] = uuidCanonical[0];

    out[4] = uuidCanonical[5];
    out[5] = uuidCanonical[4];

    out[6] = uuidCanonical[7];
    out[7] = uuidCanonical[6];

    for (size_t i = 8; i < 16; ++i) {
        out[i] = uuidCanonical[i];
    }

    return out;
}

std::vector<uint8_t> BuildProtectiveMbr()
{
    std::vector<uint8_t> mbr(kBlockSize, 0);

    const size_t entryOffset = 0x1BE;
    mbr[entryOffset + 0] = 0x00;
    mbr[entryOffset + 1] = 0x00;
    mbr[entryOffset + 2] = 0x02;
    mbr[entryOffset + 3] = 0x00;
    mbr[entryOffset + 4] = 0xEE;
    mbr[entryOffset + 5] = 0xFF;
    mbr[entryOffset + 6] = 0xFF;
    mbr[entryOffset + 7] = 0xFF;

    const uint32_t firstLba = 1;
    const uint32_t lbaSize = 0xFFFFFFFFU;
    mbr[entryOffset + 8] = static_cast<uint8_t>(firstLba & 0xFF);
    mbr[entryOffset + 9] = static_cast<uint8_t>((firstLba >> 8) & 0xFF);
    mbr[entryOffset + 10] = static_cast<uint8_t>((firstLba >> 16) & 0xFF);
    mbr[entryOffset + 11] = static_cast<uint8_t>((firstLba >> 24) & 0xFF);

    mbr[entryOffset + 12] = static_cast<uint8_t>(lbaSize & 0xFF);
    mbr[entryOffset + 13] = static_cast<uint8_t>((lbaSize >> 8) & 0xFF);
    mbr[entryOffset + 14] = static_cast<uint8_t>((lbaSize >> 16) & 0xFF);
    mbr[entryOffset + 15] = static_cast<uint8_t>((lbaSize >> 24) & 0xFF);

    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    return mbr;
}

std::vector<uint8_t> BuildPartitionEntry(const std::string &name, uint64_t startLba, uint64_t endLba)
{
    std::vector<uint8_t> entry(kPartEntrySize, 0);

    const std::array<uint8_t, 16> partTypeGuid = UuidToGptBytes(kPartTypeGuidCanonical);
    const std::array<uint8_t, 16> partGuid = UuidToGptBytes(MakeRandomUuidCanonical());

    std::copy(partTypeGuid.begin(), partTypeGuid.end(), entry.begin());
    std::copy(partGuid.begin(), partGuid.end(), entry.begin() + 16);

    for (size_t i = 0; i < 8; ++i) {
        entry[32 + i] = static_cast<uint8_t>((startLba >> (i * 8)) & 0xFF);
        entry[40 + i] = static_cast<uint8_t>((endLba >> (i * 8)) & 0xFF);
    }

    const size_t nameOffset = 56;
    const size_t nameBytesMax = 72;
    const size_t charsToWrite = std::min(name.size(), nameBytesMax / 2);
    for (size_t i = 0; i < charsToWrite; ++i) {
        entry[nameOffset + (i * 2)] = static_cast<uint8_t>(name[i]);
        entry[nameOffset + (i * 2) + 1] = 0;
    }

    return entry;
}

std::vector<uint8_t> BuildPrimaryGpt(const std::vector<EmmcPartitionDesc> &partitions)
{
    std::vector<uint8_t> partBytes(kPartEntries * kPartEntrySize, 0);

    uint64_t previousEndLba = 0;
    uint64_t maxUsedLba = 0;
    const uint64_t lbasPerMb = kMbSize / kBlockSize;

    for (size_t idx = 0; idx < partitions.size() && idx < kPartEntries; ++idx) {
        const EmmcPartitionDesc &part = partitions[idx];
        uint64_t startLba = 0;
        if (part.startMb > 0) {
            startLba = part.startMb * lbasPerMb;
        } else {
            startLba = previousEndLba + 1;
        }

        const uint64_t sizeLbas = part.sizeMb * lbasPerMb;
        const uint64_t endLba = startLba + sizeLbas - 1;

        previousEndLba = endLba;
        maxUsedLba = std::max(maxUsedLba, endLba);

        const std::vector<uint8_t> entry = BuildPartitionEntry(part.name, startLba, endLba);
        std::copy(entry.begin(), entry.end(), partBytes.begin() + (idx * kPartEntrySize));
    }

    const uint32_t partArrayCrc = ComputeCRC32(partBytes.data(), partBytes.size());

    std::vector<uint8_t> header(kBlockSize, 0);
    const char signature[] = "EFI PART";
    std::copy(signature, signature + 8, header.begin());

    header[8] = static_cast<uint8_t>(kGptRevision & 0xFF);
    header[9] = static_cast<uint8_t>((kGptRevision >> 8) & 0xFF);
    header[10] = static_cast<uint8_t>((kGptRevision >> 16) & 0xFF);
    header[11] = static_cast<uint8_t>((kGptRevision >> 24) & 0xFF);

    header[12] = static_cast<uint8_t>(kGptHeaderSize & 0xFF);
    header[13] = static_cast<uint8_t>((kGptHeaderSize >> 8) & 0xFF);
    header[14] = static_cast<uint8_t>((kGptHeaderSize >> 16) & 0xFF);
    header[15] = static_cast<uint8_t>((kGptHeaderSize >> 24) & 0xFF);

    const uint64_t currentLba = 1;
    const uint64_t backupLba = 0;
    const uint64_t firstUsableLba = 34;

    for (size_t i = 0; i < 8; ++i) {
        header[24 + i] = static_cast<uint8_t>((currentLba >> (i * 8)) & 0xFF);
        header[32 + i] = static_cast<uint8_t>((backupLba >> (i * 8)) & 0xFF);
        header[40 + i] = static_cast<uint8_t>((firstUsableLba >> (i * 8)) & 0xFF);
        header[48 + i] = static_cast<uint8_t>((maxUsedLba >> (i * 8)) & 0xFF);
    }

    const std::array<uint8_t, 16> diskGuid = UuidToGptBytes(MakeRandomUuidCanonical());
    std::copy(diskGuid.begin(), diskGuid.end(), header.begin() + 56);

    const uint64_t partEntryLba = 2;
    for (size_t i = 0; i < 8; ++i) {
        header[72 + i] = static_cast<uint8_t>((partEntryLba >> (i * 8)) & 0xFF);
    }

    header[80] = static_cast<uint8_t>(kPartEntries & 0xFF);
    header[81] = static_cast<uint8_t>((kPartEntries >> 8) & 0xFF);
    header[82] = static_cast<uint8_t>((kPartEntries >> 16) & 0xFF);
    header[83] = static_cast<uint8_t>((kPartEntries >> 24) & 0xFF);

    header[84] = static_cast<uint8_t>(kPartEntrySize & 0xFF);
    header[85] = static_cast<uint8_t>((kPartEntrySize >> 8) & 0xFF);
    header[86] = static_cast<uint8_t>((kPartEntrySize >> 16) & 0xFF);
    header[87] = static_cast<uint8_t>((kPartEntrySize >> 24) & 0xFF);

    header[88] = static_cast<uint8_t>(partArrayCrc & 0xFF);
    header[89] = static_cast<uint8_t>((partArrayCrc >> 8) & 0xFF);
    header[90] = static_cast<uint8_t>((partArrayCrc >> 16) & 0xFF);
    header[91] = static_cast<uint8_t>((partArrayCrc >> 24) & 0xFF);

    const uint32_t headerCrc = ComputeCRC32(header.data(), kGptHeaderSize);
    header[16] = static_cast<uint8_t>(headerCrc & 0xFF);
    header[17] = static_cast<uint8_t>((headerCrc >> 8) & 0xFF);
    header[18] = static_cast<uint8_t>((headerCrc >> 16) & 0xFF);
    header[19] = static_cast<uint8_t>((headerCrc >> 24) & 0xFF);

    if (partBytes.size() < kGptTableSize) {
        partBytes.resize(kGptTableSize, 0);
    }

    std::vector<uint8_t> gpt = BuildProtectiveMbr();
    gpt.insert(gpt.end(), header.begin(), header.end());
    gpt.insert(gpt.end(), partBytes.begin(), partBytes.end());

    return gpt;
}

uint32_t GetImageTypeFromPartitionName(const std::string &partitionName)
{
    const std::string name = ToLower(partitionName);

    if (name.find("sysmgr") != std::string::npos) {
        return kImgTypeSm;
    }

    if (name.find("bl") != std::string::npos && name.find("m52") == std::string::npos) {
        return kImgTypeBl;
    }

    if (name.find("tzk") != std::string::npos) {
        return kImgTypeOptee;
    }

    return kImgTypeGpt;
}

std::string VersionToString(uint32_t version)
{
    const uint32_t major = (version >> 16) & 0xFFFFU;
    const uint32_t minor = version & 0xFFFFU;

    std::ostringstream out;
    out << major << "." << minor;
    return out.str();
}

bool DecompressGzip(const std::filesystem::path &src, const std::filesystem::path &dst)
{
    gzFile gz = gzopen(src.string().c_str(), "rb");
    if (gz == nullptr) {
        return false;
    }

    std::ofstream out(dst, std::ios::binary);
    if (!out) {
        gzclose(gz);
        return false;
    }

    static constexpr size_t kDecompressBufSize = 65536;
    std::vector<uint8_t> buf(kDecompressBufSize);
    int bytesRead = 0;
    while ((bytesRead = gzread(gz, buf.data(), static_cast<unsigned>(kDecompressBufSize))) > 0) {
        out.write(reinterpret_cast<const char *>(buf.data()), bytesRead);
    }

    gzclose(gz);
    return bytesRead == 0 && !out.fail();
}

std::string DeviceModeToString(SL26XXDeviceMode mode)
{
    switch (mode) {
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_BOOTROM:
        return "bootrom";
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_M52BL:
        return "m52bl";
    case SL26XXDeviceMode::SL26XX_DEVICE_MODE_SYSMGR:
        return "sysmgr";
    default:
        return "unknown";
    }
}

} // namespace

class AstraDeviceSL26XXImpl final : public AstraDeviceImpl {
public:
    AstraDeviceSL26XXImpl(std::unique_ptr<USBDevice> device, const std::string &tempDir,
        bool bootOnly, const std::string &bootCommand)
        : AstraDeviceImpl(std::move(device), tempDir, bootOnly, bootCommand)
    {}

    ~AstraDeviceSL26XXImpl() override
    {
        Close();
    }

    int Boot(std::shared_ptr<AstraBootImage> bootImage, AstraDeviceBootStage bootStage) override
    {
        ASTRA_LOG;

        if (bootImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing boot image for SL26XX update boot flow" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Missing boot image");
            return -1;
        }

        if (m_usbDevice == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing USB device" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Missing USB device");
            return -1;
        }

        if (m_deviceOpened) {
            return 0;
        }

        int ret = m_usbDevice->Open(std::bind(&AstraDeviceSL26XXImpl::USBEventHandler, this,
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open SL26XX device" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to open device");
            return ret;
        }

        m_deviceOpened = true;
        m_deviceName = "device:" + m_usbDevice->GetUSBPath();
        m_status = ASTRA_DEVICE_STATUS_OPENED;

        m_status = ASTRA_DEVICE_STATUS_BOOT_START;
        ReportStatus(ASTRA_DEVICE_STATUS_BOOT_START, 0, "", "Starting SL26XX boot");

        {
            std::lock_guard<std::mutex> lock(m_rxMutex);
            m_deviceDisconnected = false;
            m_rxBuffer.clear();
        }
        m_expectResetDisconnect = false;

        if (!m_bootOnly) {
            // On SL26XX image updating is handled by the sysmgr. Once the sysmgr is running then booting is complete
            // and we move on to the update phase.
            bootStage = ASTRA_DEVICE_BOOT_STAGE_SYSMGR;
        }

        ret = m_usbDevice->EnableInterrupts();
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to enable interrupts for SL26XX" << endLog;
            m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to enable interrupts");
            return ret;
        }

        const uint16_t devVid = m_usbDevice->GetVendorId();
        const uint16_t devPid = m_usbDevice->GetProductId();
        const uint8_t numInterfaces = m_usbDevice->GetNumInterfaces();
        const uint16_t sysMgrVid = bootImage->GetSysMgrVendorId();
        const uint16_t sysMgrPid = bootImage->GetSysMgrProductId();

        SL26XXDeviceMode resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_UNKNOWN;
        if (sysMgrVid != 0 && devVid == sysMgrVid && devPid == sysMgrPid) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_SYSMGR;
        } else if (numInterfaces > 1) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_BOOTROM;
        } else if (numInterfaces == 1) {
            resolvedMode = SL26XXDeviceMode::SL26XX_DEVICE_MODE_M52BL;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX mode resolved: " << DeviceModeToString(resolvedMode)
                                  << " numInterfaces=" << static_cast<int>(numInterfaces)
                                  << " vid=0x" << std::hex << devVid << " pid=0x" << devPid << std::dec << endLog;

        if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_BOOTROM) {
            if (!RunSpkBootSequence(*bootImage)) {
                m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to run SL26XX key/spk/m52bl boot sequence");
                return -1;
            }

            // M52BL has been loaded. Device will disconnect and re-enumerate now.
            m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
            return 1;
        } else if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_M52BL) {
            uint32_t blVersion = 0;
            if (GetBootloaderVersion(blVersion)) {
                log(ASTRA_LOG_LEVEL_INFO) << "SL26XX M52BL version: " << VersionToString(blVersion)
                                          << " (0x" << std::hex << std::uppercase << blVersion
                                          << std::dec << ")" << endLog;
            } else {
                log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX M52BL version query failed" << endLog;
            }

            if (bootStage == ASTRA_DEVICE_BOOT_STAGE_M52BL) {
                // Target stage reached: device is already in M52BL.
                m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE, 100, "", "Device already in M52BL");
                return 0;
            }

            if (!RunSmBootSequence(*bootImage)) {
                m_expectResetDisconnect = false;
                m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to run SL26XX SysMgr boot sequence");
                return -1;
            }

            // SysMgr has been loaded. Device will disconnect and re-enumerate now.
            m_status = ASTRA_DEVICE_STATUS_BOOT_PROGRESS;
            return 1;
        } else if (resolvedMode == SL26XXDeviceMode::SL26XX_DEVICE_MODE_SYSMGR) {
            // Device is already in SysMgr — no boot sequence needed.
            uint32_t smVersion = 0;
            if (GetSysMgrVersion(smVersion)) {
                log(ASTRA_LOG_LEVEL_INFO) << "SL26XX SysMgr version: " << VersionToString(smVersion)
                                          << " (0x" << std::hex << std::uppercase << smVersion
                                          << std::dec << ")" << endLog;
            } else {
                log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX SysMgr version query failed" << endLog;
            }

            if (bootStage == ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER) {
                if (!RunAcoreSequence(*bootImage)) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                    ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Failed to run SL26XX A-Core boot sequence");
                    return -1;
                }
                m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
                ReportStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE, 100, "", "SL26XX A-Core bootloader sequence complete");
                return 0;
            }

            // AUTO, SYSMGR stage, and all others — SysMgr is the natural endpoint.
            m_status = ASTRA_DEVICE_STATUS_BOOT_COMPLETE;
            ReportStatus(ASTRA_DEVICE_STATUS_BOOT_COMPLETE, 100, "", "Device already in SysMgr");
            return 0;
        }

        m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
        ReportStatus(ASTRA_DEVICE_STATUS_BOOT_FAIL, 0, "", "Unable to resolve SL26XX device mode");
        return -1;
    }

    int Update(std::shared_ptr<FlashImage> flashImage) override
    {
        ASTRA_LOG;

        if (!m_deviceOpened) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX device is not booted/opened" << endLog;
            m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, "", "Device not open");
            return -1;
        }

        if (dynamic_cast<USBCDCDevice *>(m_usbDevice.get()) == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX update requires a USB CDC device" << endLog;
            m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, "", "SL26XX update requires USB CDC transport");
            return -1;
        }

        if (flashImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing flash image" << endLog;
            m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, "", "Missing flash image");
            return -1;
        }

        if (flashImage->GetFlashImageType() != FLASH_IMAGE_TYPE_EMMC) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX implementation currently supports eMMC images only" << endLog;
            m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, "", "SL26XX supports eMMC updates only");
            return -1;
        }

        {
            std::lock_guard<std::mutex> lock(m_rxMutex);
            m_deviceDisconnected = false;
            m_rxBuffer.clear();
        }
        m_expectResetDisconnect = false;

        m_status = ASTRA_DEVICE_STATUS_UPDATE_START;
        ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_START, 0, "", "Starting SL26XX eMMC update");

        if (!RunEmmcUpdateFlow(flashImage)) {
            m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
            ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_FAIL, 0, "", "SL26XX eMMC update failed");
            return -1;
        }

        m_status = ASTRA_DEVICE_STATUS_UPDATE_COMPLETE;
        ReportStatus(ASTRA_DEVICE_STATUS_UPDATE_COMPLETE, 100, "", "Success");
        return 0;
    }

    int WaitForCompletion() override
    {
        if (m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL || m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL) {
            return -1;
        }
        return 0;
    }

    int SendToConsole(const std::string &data) override
    {
        ASTRA_LOG;
        (void)data;

        log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX console transport is not implemented" << endLog;
        return -1;
    }

    int ReceiveFromConsole(std::string &data) override
    {
        ASTRA_LOG;
        data.clear();

        log(ASTRA_LOG_LEVEL_WARNING) << "SL26XX console transport is not implemented" << endLog;
        return -1;
    }

    void Close() override
    {
        ASTRA_LOG;

        std::lock_guard<std::mutex> lock(m_closeMutex);
        if (m_closed.exchange(true)) {
            return;
        }

        {
            std::lock_guard<std::mutex> rxLock(m_rxMutex);
            m_deviceDisconnected = true;
            m_rxBuffer.clear();
            m_rxCV.notify_all();
        }
        m_expectResetDisconnect = false;

        m_deviceOpened = false;
        AstraDeviceImpl::Close();
    }

private:
    using ImageActionMap = std::unordered_map<std::string, std::vector<std::string>>;

    std::mutex m_closeMutex;
    std::atomic<bool> m_closed{false};
    bool m_deviceOpened = false;

    std::mutex m_rxMutex;
    std::condition_variable m_rxCV;
    std::deque<uint8_t> m_rxBuffer;
    bool m_deviceDisconnected = false;
    std::atomic<bool> m_expectResetDisconnect{false};

    void USBEventHandler(USBDevice::USBEvent event, uint8_t *buf, size_t size)
    {
        if (event == USBDevice::USB_DEVICE_EVENT_INTERRUPT) {
            if (buf != nullptr && size > 0) {
                std::lock_guard<std::mutex> lock(m_rxMutex);
                for (size_t i = 0; i < size; ++i) {
                    m_rxBuffer.push_back(buf[i]);
                }
                m_rxCV.notify_all();
            }
            return;
        }

        if (event == USBDevice::USB_DEVICE_EVENT_NO_DEVICE ||
            event == USBDevice::USB_DEVICE_EVENT_TRANSFER_CANCELED ||
            event == USBDevice::USB_DEVICE_EVENT_TRANSFER_ERROR)
        {
            {
                std::lock_guard<std::mutex> lock(m_rxMutex);
                m_deviceDisconnected = true;
                m_rxCV.notify_all();
            }

            const bool suppressDisconnectFailure = m_expectResetDisconnect.load();

            if (m_status != ASTRA_DEVICE_STATUS_BOOT_COMPLETE &&
                m_status != ASTRA_DEVICE_STATUS_UPDATE_COMPLETE)
            {
                if (suppressDisconnectFailure &&
                    (m_status == ASTRA_DEVICE_STATUS_BOOT_START || m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS))
                {
                    return;
                }

                if (m_status == ASTRA_DEVICE_STATUS_BOOT_START || m_status == ASTRA_DEVICE_STATUS_BOOT_PROGRESS) {
                    m_status = ASTRA_DEVICE_STATUS_BOOT_FAIL;
                } else {
                    m_status = ASTRA_DEVICE_STATUS_UPDATE_FAIL;
                }

                ReportStatus(m_status, 0, "", "Device disconnected");
            }
        }
    }

    void ClearRxBuffer()
    {
        std::lock_guard<std::mutex> lock(m_rxMutex);
        m_rxBuffer.clear();
    }

    bool WaitForDeviceDisconnect(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_rxMutex);
        return m_rxCV.wait_for(lock, timeout, [this]() {
            return m_deviceDisconnected;
        });
    }

    bool ReadExactBytes(size_t bytesToRead, std::vector<uint8_t> &out,
        std::chrono::milliseconds timeout)
    {
        out.clear();

        std::unique_lock<std::mutex> lock(m_rxMutex);
        const bool ready = m_rxCV.wait_for(lock, timeout, [this, bytesToRead]() {
            return m_rxBuffer.size() >= bytesToRead || m_deviceDisconnected;
        });

        if (!ready || m_rxBuffer.size() < bytesToRead) {
            return false;
        }

        out.reserve(bytesToRead);
        for (size_t i = 0; i < bytesToRead; ++i) {
            out.push_back(m_rxBuffer.front());
            m_rxBuffer.pop_front();
        }

        return true;
    }

    bool WriteAll(const uint8_t *data, size_t size)
    {
        USBDevice *usbDevice = m_usbDevice.get();
        if (usbDevice == nullptr) {
            return false;
        }

        size_t offset = 0;
        while (offset < size) {
            int transferred = 0;
            const int ret = usbDevice->Write(const_cast<uint8_t *>(data + offset), size - offset, &transferred);
            if (ret < 0 || transferred <= 0) {
                return false;
            }

            offset += static_cast<size_t>(transferred);
        }

        return true;
    }

    int ReadResponseCode(bool rawMode, std::chrono::milliseconds timeout)
    {
        ASTRA_LOG;

        std::vector<uint8_t> responseHeader;
        if (!ReadExactBytes(kHostHeaderSize, responseHeader, timeout)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Timed out waiting for protocol response header" << endLog;
            return -1;
        }

        if (responseHeader[0] != kHostSync1 || responseHeader[1] != kHostSync2) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Invalid protocol sync bytes in response header" << endLog;
            return -1;
        }

        if (rawMode) {
            return static_cast<int>(ReadU32LE(&responseHeader[4]));
        }

        const uint32_t payloadSize = ReadU32LE(&responseHeader[4]);
        if (payloadSize == 0) {
            return 0;
        }

        if (payloadSize > (4U * 1024U * 1024U)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Protocol response payload too large: " << payloadSize << endLog;
            return -1;
        }

        std::vector<uint8_t> payload;
        if (!ReadExactBytes(payloadSize, payload, timeout)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Timed out waiting for protocol response payload" << endLog;
            return -1;
        }

        if (payload.size() < sizeof(uint32_t)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Protocol response payload too small" << endLog;
            return -1;
        }

        return static_cast<int>(ReadU32LE(payload.data()));
    }

    int SendPacket(uint8_t serviceId, uint8_t opcode, const std::vector<uint8_t> &payload,
        uint8_t hostOpcode, uint32_t addr, uint32_t imageType, bool isLast,
        std::chrono::milliseconds timeout, std::optional<uint32_t> numWordsOverride = std::nullopt,
        bool rawMode = false, bool waitForResponse = true)
    {
        ASTRA_LOG;

        std::vector<uint8_t> paddedPayload = payload;
        const size_t padding = (4 - (paddedPayload.size() % 4)) % 4;
        if (padding > 0) {
            paddedPayload.insert(paddedPayload.end(), padding, 0xFF);
        }

        uint32_t numWords = 0;
        if (numWordsOverride.has_value()) {
            numWords = *numWordsOverride;
        } else {
            numWords = static_cast<uint32_t>(paddedPayload.size() / 4);
        }

        std::vector<uint8_t> innerPacket;
        innerPacket.reserve(kOpHeaderSize + paddedPayload.size());
        innerPacket.push_back(kHostSync1);
        innerPacket.push_back(kHostSync2);
        innerPacket.push_back(serviceId);
        innerPacket.push_back(opcode);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, numWords);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, addr);
        AppendU32LE(innerPacket, imageType);
        AppendU32LE(innerPacket, isLast ? 1U : 0U);
        AppendU32LE(innerPacket, 0);
        innerPacket.insert(innerPacket.end(), paddedPayload.begin(), paddedPayload.end());

        std::vector<uint8_t> finalPacket;
        if (rawMode) {
            finalPacket = std::move(innerPacket);
        } else {
            finalPacket.reserve(kHostHeaderSize + innerPacket.size());
            finalPacket.push_back(kHostSync1);
            finalPacket.push_back(kHostSync2);
            finalPacket.push_back(kHostApiServiceId);
            finalPacket.push_back(hostOpcode);
            AppendU32LE(finalPacket, static_cast<uint32_t>(innerPacket.size()));
            finalPacket.insert(finalPacket.end(), innerPacket.begin(), innerPacket.end());
        }

        ClearRxBuffer();

        if (!WriteAll(finalPacket.data(), finalPacket.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write protocol packet" << endLog;
            return -1;
        }

        if (!waitForResponse) {
            return 0;
        }

        return ReadResponseCode(rawMode, timeout);
    }

    int SendEmmcCommand(uint32_t subCommand, uint32_t param1, uint32_t param2,
        std::chrono::milliseconds timeout, std::chrono::milliseconds delay)
    {
        ASTRA_LOG;

        std::vector<uint8_t> innerPacket;
        innerPacket.reserve(kOpHeaderSize);
        innerPacket.push_back(kHostSync1);
        innerPacket.push_back(kHostSync2);
        innerPacket.push_back(kServiceIdBoot);
        innerPacket.push_back(kOpcodeEmmcOp);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, subCommand);
        AppendU32LE(innerPacket, param1);
        AppendU32LE(innerPacket, param2);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, 0);
        AppendU32LE(innerPacket, 0);

        std::vector<uint8_t> packet;
        packet.reserve(kHostHeaderSize + innerPacket.size());
        packet.push_back(kHostSync1);
        packet.push_back(kHostSync2);
        packet.push_back(kHostApiServiceId);
        packet.push_back(kHostApiOpcodeEmmc);
        AppendU32LE(packet, static_cast<uint32_t>(innerPacket.size()));
        packet.insert(packet.end(), innerPacket.begin(), innerPacket.end());

        ClearRxBuffer();

        if (!WriteAll(packet.data(), packet.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write eMMC command packet" << endLog;
            return -1;
        }

        const int rc = ReadResponseCode(false, timeout);

        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }

        return rc;
    }

    int SendSpkCommand(uint8_t opcode, const uint8_t *payload, uint32_t payloadSize,
        std::chrono::milliseconds timeout)
    {
        ASTRA_LOG;

        std::vector<uint8_t> packet;
        packet.reserve(kOpHeaderSize + payloadSize);
        packet.push_back(kHostSync1);
        packet.push_back(kHostSync2);
        packet.push_back(kServiceIdBoot);
        packet.push_back(opcode);
        AppendU32LE(packet, payloadSize);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);

        if (payloadSize > 0 && payload != nullptr) {
            packet.insert(packet.end(), payload, payload + payloadSize);
        }

        ClearRxBuffer();

        if (!WriteAll(packet.data(), packet.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write SPK bootstrap packet, opcode=0x"
                << std::hex << std::uppercase << static_cast<uint32_t>(opcode) << std::dec << endLog;
            return -1;
        }

        return ReadResponseCode(true, timeout);
    }

    bool GetBootloaderVersion(uint32_t &version)
    {
        ASTRA_LOG;

        version = 0;

        std::vector<uint8_t> packet;
        packet.reserve(kOpHeaderSize);
        packet.push_back(kHostSync1);
        packet.push_back(kHostSync2);
        packet.push_back(kServiceIdBoot);
        packet.push_back(kOpcodeVersion);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);
        AppendU32LE(packet, 0);

        ClearRxBuffer();

        if (!WriteAll(packet.data(), packet.size())) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send SL26XX BL version request" << endLog;
            return false;
        }

        std::vector<uint8_t> responseHeader;
        if (!ReadExactBytes(kHostHeaderSize, responseHeader, std::chrono::seconds(2))) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Timed out waiting for SL26XX BL version response" << endLog;
            return false;
        }

        if (responseHeader[0] != kHostSync1 || responseHeader[1] != kHostSync2) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Invalid sync bytes in SL26XX BL version response" << endLog;
            return false;
        }

        // BL VERSION typically encodes version in response header bytes [4..7].
        version = ReadU32LE(&responseHeader[4]);

        // Some ROM variants append an extra 4-byte word after the response header.
        std::vector<uint8_t> trailing;
        (void)ReadExactBytes(sizeof(uint32_t), trailing, std::chrono::milliseconds(50));

        return true;
    }

    bool GetSysMgrVersion(uint32_t &version)
    {
        ASTRA_LOG;

        version = 0;

        const int rc = SendPacket(kServiceIdBoot, kOpcodeVersion, {}, kHostApiOpcodeVersion,
                                  0, 0, false, std::chrono::seconds(2));
        if (rc < 0) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Failed to query SL26XX SysMgr version" << endLog;
            return false;
        }

        version = static_cast<uint32_t>(rc);
        return true;
    }

    bool SendSpkFile(const std::filesystem::path &path, const std::string &imageName, uint8_t opcode)
    {
        ASTRA_LOG;

        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open SPK bootstrap file: " << path.string() << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Failed to open image");
            return false;
        }

        const std::streamoff endPos = input.tellg();
        if (endPos < 0 || static_cast<uint64_t>(endPos) > std::numeric_limits<uint32_t>::max()) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Invalid SPK bootstrap file size for " << imageName << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Invalid image size");
            return false;
        }

        const uint32_t size = static_cast<uint32_t>(endPos);
        input.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer;
        buffer.resize(size);
        if (size > 0) {
            input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
            if (input.gcount() != static_cast<std::streamsize>(size)) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to read SPK bootstrap image: " << path.string() << endLog;
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Failed to read image");
                return false;
            }
        }

        ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, imageName);
        const int rc = SendSpkCommand(opcode, buffer.data(), size, std::chrono::seconds(5));
        if (rc != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SPK bootstrap upload failed for " << imageName << ", rc=" << rc << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Bootstrap upload failed");
            return false;
        }

        ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, imageName);
        return true;
    }

    const Image *FindKeyBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "key.bin"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("key") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    const Image *FindSpkBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "spk.bin"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("spk") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    const Image *FindM52BlBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "m52bl.bin"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("m52") != std::string::npos && imageName.find("bl") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    bool RunSpkBootSequence(const AstraBootImage &bootImage)
    {
        ASTRA_LOG;

        const Image *keyImage = FindKeyBootImage(bootImage);
        const Image *spkImage = FindSpkBootImage(bootImage);
        const Image *m52BlImage = FindM52BlBootImage(bootImage);

        if (keyImage == nullptr || spkImage == nullptr || m52BlImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing key/spk/m52bl image in boot image set" << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX run-spk: uploading " << keyImage->GetName() << ", "
                                  << spkImage->GetName() << ", and " << m52BlImage->GetName() << endLog;

        if (!SendSpkFile(keyImage->GetPath(), keyImage->GetName(), kOpcodeUploadKey)) {
            return false;
        }

        if (!SendSpkFile(spkImage->GetPath(), spkImage->GetName(), kOpcodeUploadSpk)) {
            return false;
        }

        if (!SendSpkFile(m52BlImage->GetPath(), m52BlImage->GetName(), kOpcodeUploadM52Bl)) {
            return false;
        }

        return true;
    }

    const Image *FindSysMgrBootImage(const AstraBootImage &bootImage)
    {
        // Match the same primary filename used by usb_boot_tool.py --op run-sm --sm sysmgr.subimg.
        static const std::array<const char *, 1> kPreferredSysMgrNames = {
            "sysmgr.subimg"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredSysMgrNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("sysmgr") != std::string::npos)
            {
                return &image;
            }
        }

        return nullptr;
    }

    bool RunSmBootSequence(const AstraBootImage &bootImage)
    {
        ASTRA_LOG;

        const Image *sysMgrImage = FindSysMgrBootImage(bootImage);
        if (sysMgrImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Unable to find SysMgr image in boot image set" << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX run-sm: uploading " << sysMgrImage->GetName()
                                  << " to 0x" << std::hex << std::uppercase << kAddrSmLoad
                                  << " and sending Run Image command" << std::dec << endLog;

        if (!UploadFile(sysMgrImage->GetPath(), sysMgrImage->GetName(), kImgTypeSm, kAddrSmLoad, true)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to upload SysMgr image: " << sysMgrImage->GetName() << endLog;
            return false;
        }

        m_expectResetDisconnect = true;

        const int runRc = SendPacket(kServiceIdBoot, kOpcodeRunImage, {}, kHostApiOpcodeGeneric,
            kAddrSmLoad, 0, false, std::chrono::seconds(5), std::nullopt, true, false);
        if (runRc != 0) {
            m_expectResetDisconnect = false;
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to send Run Image command, rc=" << runRc << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "Run Image command sent, waiting for device to re-enumerate as SysMgr" << endLog;
        return true;
    }

    const Image *FindBlBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "bl.subimg"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("bl") != std::string::npos &&
                imageName.find("m52") == std::string::npos &&
                imageName.find("spk") == std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    const Image *FindTzkBootImage(const AstraBootImage &bootImage)
    {
        static const std::array<const char *, 1> kPreferredNames = {
            "tzk.subimg"
        };

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            for (const char *preferredName : kPreferredNames) {
                if (imageName == preferredName) {
                    return &image;
                }
            }
        }

        for (const Image &image : bootImage.GetImages()) {
            const std::string imageName = ToLower(image.GetName());
            if (imageName.find("tzk") != std::string::npos) {
                return &image;
            }
        }

        return nullptr;
    }

    bool RunAcoreSequence(const AstraBootImage &bootImage)
    {
        ASTRA_LOG;

        const Image *blImage = FindBlBootImage(bootImage);
        const Image *tzkImage = FindTzkBootImage(bootImage);

        if (blImage == nullptr || tzkImage == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing BL or TZK image for A-Core sequence" << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX run-acore: uploading " << blImage->GetName()
                                  << " and " << tzkImage->GetName() << endLog;

        if (!UploadFile(blImage->GetPath(), blImage->GetName(), kImgTypeBl)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to upload BL image: " << blImage->GetName() << endLog;
            return false;
        }

        const int execRc1 = SendPacket(kServiceIdBoot, kOpcodeExec0C, {}, kHostApiOpcodeExec,
            0, 0, false, std::chrono::seconds(5));
        if (execRc1 != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "BL exec command failed, rc=" << execRc1 << endLog;
            return false;
        }

        if (!UploadFile(tzkImage->GetPath(), tzkImage->GetName(), kImgTypeOptee)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to upload TZK image: " << tzkImage->GetName() << endLog;
            return false;
        }

        const int execRc2 = SendPacket(kServiceIdBoot, kOpcodeExec0C, {}, kHostApiOpcodeExec,
            0, 0, false, std::chrono::seconds(5));
        if (execRc2 != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "TZK exec command failed, rc=" << execRc2 << endLog;
            return false;
        }

        log(ASTRA_LOG_LEVEL_INFO) << "SL26XX A-Core sequence complete" << endLog;
        return true;
    }

    bool UploadData(const uint8_t *data, uint64_t size, const std::string &imageName,
        uint32_t imageType, uint32_t loadAddress = kAddrAcLoad, bool rawMode = false,
        bool reportStatus = true, uint64_t totalSize = 0, uint64_t byteOffset = 0)
    {
        ASTRA_LOG;

        if (size > std::numeric_limits<uint32_t>::max()) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Upload size too large for protocol: " << size << endLog;
            if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Image too large");
            return false;
        }

        if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, imageName);

        const int setupRc = SendPacket(kServiceIdBoot, kOpcodeUpload, {}, kHostApiOpcodeGeneric,
            loadAddress, imageType, false, std::chrono::seconds(5), static_cast<uint32_t>(size), rawMode);
        if (setupRc != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX upload setup failed for " << imageName << ", rc=" << setupRc << endLog;
            if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Upload setup failed");
            return false;
        }

        // When totalSize is provided we always emit progress scaled to the whole image,
        // regardless of reportStatus (which only gates the start/complete events).
        const uint64_t scaleSize = (totalSize > 0) ? totalSize : size;
        const bool emitProgress = reportStatus || (totalSize > 0);

        uint64_t sent = 0;
        while (sent < size) {
            const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(kStreamChunkSize, size - sent));
            if (!WriteAll(data + sent, chunkSize)) {
                log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX upload write failed for " << imageName << endLog;
                if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Upload write failed");
                return false;
            }

            sent += chunkSize;
            if (emitProgress) {
                const double progress = (scaleSize == 0) ? 100.0
                    : (static_cast<double>(byteOffset + sent) / static_cast<double>(scaleSize)) * 100.0;
                ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS, progress, imageName);
            }
        }

        const int verifyRc = ReadResponseCode(rawMode, std::chrono::seconds(20));
        if (verifyRc != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX upload verification failed for " << imageName << ", rc=" << verifyRc << endLog;
            if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Upload verify failed");
            return false;
        }

        if (reportStatus) ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, imageName);
        return true;
    }

    bool UploadFile(const std::filesystem::path &path, const std::string &imageName, uint32_t imageType,
        uint32_t loadAddress = kAddrAcLoad, bool rawMode = false)
    {
        ASTRA_LOG;

        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open upload file: " << path.string() << endLog;
            ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, 0, imageName, "Failed to open image");
            return false;
        }

        const std::streamoff endPos = input.tellg();
        if (endPos < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to query file size: " << path.string() << endLog;
            return false;
        }

        const uint64_t size = static_cast<uint64_t>(endPos);
        input.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer;
        buffer.resize(static_cast<size_t>(size));
        if (size > 0) {
            input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
            if (input.gcount() != static_cast<std::streamsize>(size)) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to read file data: " << path.string() << endLog;
                return false;
            }
        }

        return UploadData(buffer.data(), size, imageName, imageType, loadAddress, rawMode);
    }

    bool UploadBuffer(const std::vector<uint8_t> &buffer, const std::string &imageName, uint32_t imageType,
        uint32_t loadAddress = kAddrAcLoad, bool rawMode = false, bool reportStatus = true,
        uint64_t totalSize = 0, uint64_t byteOffset = 0)
    {
        return UploadData(buffer.data(), buffer.size(), imageName, imageType, loadAddress, rawMode, reportStatus, totalSize, byteOffset);
    }

    bool ParseImageList(const std::filesystem::path &imageListPath, ImageActionMap &actions)
    {
        ASTRA_LOG;

        std::ifstream file(imageListPath);
        if (!file) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open emmc_image_list: " << imageListPath.string() << endLog;
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            const std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            std::vector<std::string> parts = SplitByComma(trimmed);
            if (parts.size() < 2) {
                continue;
            }

            std::string fileName = parts[0];
            const std::string target = ToLower(parts[1]);

            if (fileName.find("rootfs_s.subimg") != std::string::npos) {
                fileName = "rootfs.subimg.gz";
            }

            std::vector<std::string> &files = actions[target];
            if (std::find(files.begin(), files.end(), fileName) == files.end()) {
                files.push_back(fileName);
            }
        }

        return true;
    }

    bool ParsePartitionList(const std::filesystem::path &partListPath, std::vector<EmmcPartitionDesc> &partitions)
    {
        ASTRA_LOG;

        std::ifstream file(partListPath);
        if (!file) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open emmc_part_list: " << partListPath.string() << endLog;
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            std::vector<std::string> fields = SplitByComma(trimmed);
            if (fields.size() < 3) {
                std::stringstream ss(trimmed);
                fields.clear();
                std::string token;
                while (ss >> token) {
                    fields.push_back(token);
                }
                if (fields.size() < 3) {
                    continue;
                }
            }

            EmmcPartitionDesc desc;
            desc.name = fields[0];
            try {
                desc.startMb = std::stoull(fields[1], nullptr, 0);
                desc.sizeMb = std::stoull(fields[2], nullptr, 0);
            } catch (const std::exception &) {
                continue;
            }

            if (desc.sizeMb == 0) {
                continue;
            }

            partitions.push_back(desc);
        }

        return !partitions.empty();
    }

    std::optional<std::filesystem::path> ResolveImagePath(const std::filesystem::path &imageDir,
        const std::string &fileName)
    {
        std::filesystem::path basePath = imageDir / fileName;
        if (std::filesystem::exists(basePath)) {
            return basePath;
        }

        if (basePath.extension() != ".gz") {
            std::filesystem::path gzPath = basePath;
            gzPath += ".gz";
            if (std::filesystem::exists(gzPath)) {
                return gzPath;
            }
        } else {
            std::filesystem::path noGzPath = basePath;
            noGzPath.replace_extension("");
            if (std::filesystem::exists(noGzPath)) {
                return noGzPath;
            }
        }

        return std::nullopt;
    }

    bool InitAndSwitchToUser()
    {
        ASTRA_LOG;

        if (SendEmmcCommand(0, 0, 0, std::chrono::seconds(2), std::chrono::milliseconds(100)) != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX eMMC init command failed" << endLog;
            return false;
        }

        if (SendEmmcCommand(2, 0, 0, std::chrono::seconds(2), std::chrono::milliseconds(100)) != 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "SL26XX eMMC user-area switch failed" << endLog;
            return false;
        }

        return true;
    }

    uint64_t UploadAndFlashChunked(const std::filesystem::path &imagePath, const std::string &imageName,
        uint64_t startLba, uint32_t imageType)
    {
        ASTRA_LOG;

        std::ifstream file(imagePath, std::ios::binary);
        if (!file) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open chunked image: " << imagePath.string() << endLog;
            return 0;
        }

        const uint64_t totalFileSize = std::filesystem::file_size(imagePath);

        ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_START, 0, imageName);

        const size_t chunkSizeBytes = static_cast<size_t>(kChunkSizeMb * kMbSize);
        std::vector<uint8_t> chunkBuffer(chunkSizeBytes);

        uint64_t currentLba = startLba;
        uint64_t totalBlocksWritten = 0;
        uint64_t bytesReadSoFar = 0;

        while (file.good()) {
            file.read(reinterpret_cast<char *>(chunkBuffer.data()), static_cast<std::streamsize>(chunkBuffer.size()));
            const std::streamsize bytesRead = file.gcount();
            if (bytesRead <= 0) {
                break;
            }

            std::vector<uint8_t> chunk(chunkBuffer.begin(), chunkBuffer.begin() + bytesRead);
            const size_t padLen = (kBlockSize - (chunk.size() % kBlockSize)) % kBlockSize;
            if (padLen > 0) {
                chunk.insert(chunk.end(), padLen, 0);
            }

            const uint32_t chunkBlocks = static_cast<uint32_t>(chunk.size() / kBlockSize);

            if (!UploadBuffer(chunk, imageName, imageType, kAddrAcLoad, false, false, totalFileSize, bytesReadSoFar)) {
                return 0;
            }

            if (SendEmmcCommand(5, static_cast<uint32_t>(currentLba), chunkBlocks,
                    std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
            {
                return 0;
            }

            if (SendEmmcCommand(4, static_cast<uint32_t>(currentLba), chunkBlocks,
                    std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
            {
                return 0;
            }

            if (SendEmmcCommand(3, static_cast<uint32_t>(currentLba), chunkBlocks,
                    std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
            {
                return 0;
            }

            currentLba += chunkBlocks;
            totalBlocksWritten += chunkBlocks;
            bytesReadSoFar += static_cast<uint64_t>(bytesRead);
        }

        ReportStatus(ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, 100, imageName);
        return totalBlocksWritten;
    }

    bool RunEmmcUpdateFlow(const std::shared_ptr<FlashImage> &flashImage)
    {
        ASTRA_LOG;

        std::filesystem::path partListPath;
        std::filesystem::path imageListPath;

        for (const Image &image : flashImage->GetImages()) {
            if (image.GetName() == "emmc_part_list") {
                partListPath = image.GetPath();
            } else if (image.GetName() == "emmc_image_list") {
                imageListPath = image.GetPath();
            }
        }

        if (partListPath.empty() || imageListPath.empty()) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Missing emmc_part_list or emmc_image_list in flash image" << endLog;
            return false;
        }

        const std::filesystem::path imageDir = partListPath.parent_path();

        std::vector<EmmcPartitionDesc> partitions;
        if (!ParsePartitionList(partListPath, partitions)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to parse emmc_part_list" << endLog;
            return false;
        }

        ImageActionMap operations;
        if (!ParseImageList(imageListPath, operations) || operations.empty()) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to parse emmc_image_list" << endLog;
            return false;
        }

        const std::vector<uint8_t> gpt = BuildPrimaryGpt(partitions);
        const uint32_t gptBlocks = static_cast<uint32_t>((gpt.size() + kBlockSize - 1) / kBlockSize);

        if (!UploadBuffer(gpt, "gpt.bin", kImgTypeGpt)) {
            return false;
        }

        if (!InitAndSwitchToUser()) {
            return false;
        }

        if (SendEmmcCommand(5, 0, gptBlocks, std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0) {
            return false;
        }

        if (SendEmmcCommand(4, 0, gptBlocks, std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0) {
            return false;
        }

        if (SendEmmcCommand(3, 0, gptBlocks, std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0) {
            return false;
        }

        for (uint32_t bootId : {1U, 2U}) {
            const std::string key = "b" + std::to_string(bootId);
            auto it = operations.find(key);
            if (it == operations.end()) {
                continue;
            }

            for (const std::string &fileName : it->second) {
                const std::optional<std::filesystem::path> path = ResolveImagePath(imageDir, fileName);
                if (!path.has_value()) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Missing boot image file: " << fileName << endLog;
                    return false;
                }

                std::filesystem::path effectivePath = path.value();
                if (effectivePath.extension() == ".gz") {
                    log(ASTRA_LOG_LEVEL_INFO) << "Decompressing " << effectivePath.filename().string() << endLog;
                    std::filesystem::path decompressedPath = std::filesystem::path(m_tempDir) / effectivePath.stem();
                    if (!DecompressGzip(effectivePath, decompressedPath)) {
                        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to decompress: " << effectivePath.string() << endLog;
                        return false;
                    }
                    effectivePath = decompressedPath;
                }

                const uint64_t fileSize = std::filesystem::file_size(effectivePath);
                const uint32_t fileBlocks = static_cast<uint32_t>((fileSize + kBlockSize - 1) / kBlockSize);

                if (!UploadFile(effectivePath, fileName, kImgTypeGpt)) {
                    return false;
                }

                if (SendEmmcCommand(0, 0, 0, std::chrono::seconds(2), std::chrono::milliseconds(200)) != 0) {
                    return false;
                }

                if (SendEmmcCommand(2, bootId, 0, std::chrono::seconds(2), std::chrono::seconds(12)) != 0) {
                    return false;
                }

                if (SendEmmcCommand(5, 0, fileBlocks, std::chrono::seconds(240), std::chrono::seconds(3)) != 0) {
                    return false;
                }

                if (SendEmmcCommand(4, 0, fileBlocks, std::chrono::seconds(240), std::chrono::seconds(7)) != 0) {
                    return false;
                }

                if (SendEmmcCommand(3, 0, fileBlocks, std::chrono::seconds(240), std::chrono::milliseconds(0)) != 0) {
                    return false;
                }
            }
        }

        const uint64_t lbasPerMb = kMbSize / kBlockSize;
        uint64_t previousEnd = 0;

        for (size_t index = 0; index < partitions.size(); ++index) {
            const EmmcPartitionDesc &part = partitions[index];

            const uint64_t startLba = part.startMb > 0 ? (part.startMb * lbasPerMb) : (previousEnd + 1);
            const uint64_t sizeLbas = part.sizeMb * lbasPerMb;
            const uint64_t endLba = startLba + sizeLbas - 1;
            previousEnd = endLba;

            const std::string targetId = "sd" + std::to_string(index + 1);
            auto opIt = operations.find(targetId);
            if (opIt == operations.end()) {
                continue;
            }

            uint64_t currentOffset = 0;
            for (const std::string &fileName : opIt->second) {
                const std::string lowerName = ToLower(fileName);
                if (lowerName == "format") {
                    continue;
                }

                if (lowerName == "erase") {
                    if (!InitAndSwitchToUser()) {
                        return false;
                    }

                    if (SendEmmcCommand(5, static_cast<uint32_t>(startLba), static_cast<uint32_t>(sizeLbas),
                            std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
                    {
                        return false;
                    }
                    continue;
                }

                const std::optional<std::filesystem::path> filePath = ResolveImagePath(imageDir, fileName);
                if (!filePath.has_value()) {
                    if (ToLower(part.name).find("home") != std::string::npos) {
                        continue;
                    }

                    log(ASTRA_LOG_LEVEL_ERROR) << "Missing image file for " << targetId << ": " << fileName << endLog;
                    return false;
                }

                std::filesystem::path effectivePath = filePath.value();
                if (effectivePath.extension() == ".gz") {
                    log(ASTRA_LOG_LEVEL_INFO) << "Decompressing " << effectivePath.filename().string() << endLog;
                    std::filesystem::path decompressedPath = std::filesystem::path(m_tempDir) / effectivePath.stem();
                    if (!DecompressGzip(effectivePath, decompressedPath)) {
                        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to decompress: " << effectivePath.string() << endLog;
                        return false;
                    }
                    effectivePath = decompressedPath;
                }

                const uint64_t fileSize = std::filesystem::file_size(effectivePath);
                const uint64_t fileBlocks = (fileSize + kBlockSize - 1) / kBlockSize;
                const uint64_t targetLba = startLba + currentOffset;

                if ((targetLba + fileBlocks - 1) > endLba) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Image overflows partition: " << fileName << " -> " << part.name << endLog;
                    return false;
                }

                const uint32_t imageType = GetImageTypeFromPartitionName(part.name);

                if (!InitAndSwitchToUser()) {
                    return false;
                }

                const double fileSizeMb = static_cast<double>(fileSize) / static_cast<double>(kMbSize);
                if (fileSizeMb > static_cast<double>(kLargeFileThresholdMb)) {
                    const uint64_t writtenBlocks = UploadAndFlashChunked(effectivePath, fileName, targetLba, imageType);
                    if (writtenBlocks == 0) {
                        return false;
                    }
                    currentOffset += writtenBlocks;
                    continue;
                }

                if (!UploadFile(effectivePath, fileName, imageType)) {
                    return false;
                }

                if (SendEmmcCommand(5, static_cast<uint32_t>(targetLba), static_cast<uint32_t>(fileBlocks),
                        std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
                {
                    return false;
                }

                if (SendEmmcCommand(4, static_cast<uint32_t>(targetLba), static_cast<uint32_t>(fileBlocks),
                        std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
                {
                    return false;
                }

                if (SendEmmcCommand(3, static_cast<uint32_t>(targetLba), static_cast<uint32_t>(fileBlocks),
                        std::chrono::seconds(240), std::chrono::milliseconds(100)) != 0)
                {
                    return false;
                }

                currentOffset += fileBlocks;
            }
        }

        return true;
    }
};

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL26XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand)
{
    return std::make_unique<AstraDeviceSL26XXImpl>(std::move(device), tempDir, bootOnly, bootCommand);
}

