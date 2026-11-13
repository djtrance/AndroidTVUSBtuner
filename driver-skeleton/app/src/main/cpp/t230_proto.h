// Protocolo USB/I2C y comandos de identificación para Geniatech/MyGica T230.
// Builders puros; no realizan I/O y son utilizables desde Android/NDK o escritorio.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace t230 {

constexpr uint16_t kUsbVid = 0x0572;
constexpr uint16_t kUsbPid = 0xc688;
constexpr uint16_t kBcdDevice = 0x0800;
constexpr uint8_t kInterface = 0;
constexpr uint8_t kInterfaceClassVendorSpecific = 0xff;
constexpr uint8_t kInterfaceSubclass = 0x01;
constexpr uint8_t kInterfaceProtocol = 0x01;
constexpr uint8_t kControlAlternateSetting = 0;
constexpr uint8_t kBulkOutEndpoint = 0x01;
constexpr uint8_t kBulkInEndpoint = 0x81;
constexpr uint8_t kTransportStreamEndpoint = 0x82;
constexpr uint8_t kDemodAddress = 0x64;
constexpr uint8_t kTunerAddress = 0x60;
constexpr size_t kMaxI2cMessage = 60;

// Una transacción de control USB genérica del driver dvbsky. `out` va a EP 0x01;
// al completar la escritura se leen exactamente `inLength` bytes de EP 0x81.
struct BridgeTransfer {
    std::vector<uint8_t> out;
    size_t inLength = 0;
};

// I2C bridge protocol from reference/dvbsky.c:
//   write:       08 addr write_len data...
//   read:        09 00 read_len addr             (reply read_len + 1)
//   write/read:  09 write_len read_len addr data... (reply read_len + 1)
BridgeTransfer buildI2cWrite(uint8_t address, const uint8_t* data, size_t length);
BridgeTransfer buildI2cRead(uint8_t address, size_t length);
BridgeTransfer buildI2cWriteRead(uint8_t address, const uint8_t* writeData,
                                 size_t writeLength, size_t readLength);

// Quita el primer byte de la respuesta del puente (ACK/estado no interpretado
// por dvbsky.c). Requiere una respuesta completa de payloadLength + 1 bytes.
bool extractI2cReadData(const uint8_t* bridgeReply, size_t replyLength,
                        size_t payloadLength, std::vector<uint8_t>& payload);

struct SiCommandPlan {
    BridgeTransfer write;
    // Si hay respuesta, esta lectura se repite hasta que el bit CTS esté activo.
    BridgeTransfer poll;
    size_t responseLength = 0;
    bool valid = false;
};

// El firmware SiLabs acepta el comando por I2C y se consulta por separado.
// responseLength=0 no crea una lectura de polling.
SiCommandPlan buildSi2168Command(const uint8_t* command, size_t commandLength,
                                size_t responseLength,
                                uint8_t address = kDemodAddress);
SiCommandPlan buildSi2168Init();
SiCommandPlan buildSi2168PowerUp();
SiCommandPlan buildSi2168GetRev();

enum class SiCommandState {
    Invalid,
    Busy,
    Ready,
    FirmwareError,
};

SiCommandState parseSiCommandState(const uint8_t* response, size_t responseLength);

struct Si2168Revision {
    uint32_t chipId = 0;  // bytes 1..4 del GET_REV, en orden big-endian como Linux
    uint8_t partNumber = 0;
    char versionChar = '\0';    // byte 1; p. ej. 'B'
    char versionDigit1 = '\0';  // byte 3; p. ej. '4'
    char versionDigit2 = '\0';  // byte 4; p. ej. '0'
    uint8_t patch = 0;
};

bool parseSi2168Revision(const uint8_t* response, size_t responseLength,
                         Si2168Revision& revision);
bool isSi2168B40(const Si2168Revision& revision);

// GET_REV (0x02, 13 bytes) del tuner: la identidad se expone en crudo para no
// confundir los part IDs Si2148 compatibles con firmware Si2158.
struct SiTunerRevision {
    uint8_t partId = 0;  // byte 2
    char versionChar = '\0';    // byte 1
    char versionDigit1 = '\0';  // byte 3
    char versionDigit2 = '\0';  // byte 4
    uint8_t romId = 0;   // byte 12
};

SiCommandPlan buildSiTunerGetRev(uint8_t address = kTunerAddress);
bool parseSiTunerRevision(const uint8_t* response, size_t responseLength,
                          SiTunerRevision& revision);

// Cada paso es un control USB genérico; delayAfterMs reproduce el msleep
// posterior indicado por dvbsky.c. No ejecuta ni temporiza la secuencia.
struct ControlStep {
    BridgeTransfer transfer;
    uint32_t delayAfterMs = 0;
};

// identify_state del T230 (rama no-T230A de dvbsky.c); termina en WARM.
std::vector<ControlStep> buildT230StartResetSequence();

// streaming_ctrl. Al deshabilitar solo se manda la escritura previa; al
// habilitar se añade la escritura final tras 20 ms.
std::vector<ControlStep> buildT230StreamControl(bool enabled);

}  // namespace t230
