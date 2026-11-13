// siano_proto.h - protocolo Siano SMS1xxx (familia SMS1140 / board "Rio").
//
// Reconstruido del driver propietario libpt27xDrv.so y del stack mainline
// smsusb/smscoreapi. Solo cubre lo necesario para arrancar el PT230,
// sintonizar ISDB-T y filtrar PIDs.
//
// Cabecera en el cable (little-endian):
//   u16 msg_type | u8 src | u8 dst | u16 length | u16 flags
// `length` incluye la cabecera.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace siano {

constexpr uint8_t SRC_HOST = 0;    // SMS_INIT_MSG
constexpr uint8_t SRC_BDA = 201;   // DVBT_BDA_CONTROL_MSG_ID
constexpr uint8_t DST_HIF = 11;    // HIF_TASK
constexpr uint32_t FW_START_ADDRESS = 0x00040260;
constexpr size_t MAX_PAYLOAD = 240;  // SMS_MAX_PAYLOAD_SIZE

enum MsgType : uint16_t {
    MSG_SET_MAX_TX_MSG_LEN = 516,
    MSG_RECEIVE_1SEG_THROUGH_FULLSEG = 528,
    MSG_INIT_DEVICE = 578,
    MSG_ADD_PID_FILTER = 601,
    MSG_REMOVE_PID_FILTER = 603,
    MSG_GET_STATISTICS_EX = 653,
    MSG_DATA_DOWNLOAD = 660,
    MSG_DATA_VALIDITY = 662,
    MSG_SWDOWNLOAD_TRIGGER = 664,
    MSG_GET_VERSION_EX = 668,
    MSG_SW_RELOAD_START = 702,
    MSG_SW_RELOAD_EXEC = 704,
    MSG_ISDBT_TUNE = 776,
    MSG_NEW_CRYSTAL = 794,
};

// enum SMS_DEVICE_MODE de smscoreapi.h
enum DeviceMode : uint32_t {
    DEVICE_MODE_NONE = 0xFFFFFFFFu,  // -1
    DEVICE_MODE_DVBT = 0,
    DEVICE_MODE_DVBH = 1,
    DEVICE_MODE_DAB_TDMB = 2,
    DEVICE_MODE_DVBT_BDA = 4,
    DEVICE_MODE_ISDBT = 5,
    DEVICE_MODE_ISDBT_BDA = 6,
};

// enum sms_bandwidth_mode
enum Bandwidth : uint32_t {
    BW_6_MHZ = 2,
    BW_ISDBT_1SEG = 4,
    BW_ISDBT_3SEG = 5,
    BW_ISDBT_13SEG = 8,
};

#pragma pack(push, 1)
struct MsgHdr {
    uint16_t msg_type;
    uint8_t msg_src_id;
    uint8_t msg_dst_id;
    uint16_t msg_length;
    uint16_t msg_flags;
};

// struct sms_firmware: el .inp entero, tal cual, sin transformar.
struct FirmwareHdr {
    uint32_t check_sum;
    uint32_t length;
    uint32_t start_address;
};
#pragma pack(pop)

static_assert(sizeof(MsgHdr) == 8, "cabecera Siano debe medir 8 bytes");
static_assert(sizeof(FirmwareHdr) == 12, "cabecera de firmware debe medir 12 bytes");

// --- constructores -------------------------------------------------------

std::vector<uint8_t> message(MsgType type);
std::vector<uint8_t> message(MsgType type, uint32_t word, uint8_t src = SRC_HOST);
std::vector<uint8_t> message(MsgType type, const void* data, size_t len,
                             uint8_t src = SRC_HOST);

// 776 ISDBT_TUNE. El driver original manda {freq, 8, 12000000, 0} con src=201.
// `bandwidth` = 4 (1seg) u 8 (13seg); `if_freq` suele ser 12 MHz.
std::vector<uint8_t> buildTune(uint32_t freq_hz, uint32_t bandwidth = BW_ISDBT_13SEG,
                               uint32_t if_freq = 12000000);

// 601/603: payload de una palabra (el PID).
std::vector<uint8_t> buildPidFilter(MsgType type, uint32_t pid);

// 660: bloque de firmware en `address`.
std::vector<uint8_t> buildDataDownload(uint32_t address, const uint8_t* data, size_t len);

// Secuencia completa de descarga (702/660/662/664/704) a partir del .inp.
// `running` = true si el chip ya tiene firmware (usa SW_RELOAD_*).
std::vector<std::vector<uint8_t>> buildFirmwareSequence(const uint8_t* fw, size_t size,
                                                        bool running = false);

// --- parseo de respuestas ------------------------------------------------

uint16_t peekType(const uint8_t* buf, size_t len);
uint32_t peekWord(const uint8_t* buf, size_t len, size_t offset);

}  // namespace siano
