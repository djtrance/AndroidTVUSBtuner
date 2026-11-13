#include "t230_proto.h"

namespace t230 {
namespace {

bool validI2cArgs(uint8_t address, const uint8_t* data, size_t length) {
    return address <= 0x7f && length <= kMaxI2cMessage && (!length || data);
}

BridgeTransfer genericWrite(const uint8_t* data, size_t length) {
    BridgeTransfer transfer;
    if (length && !data) return transfer;
    if (length) transfer.out.assign(data, data + length);
    transfer.inLength = 0;
    return transfer;
}

ControlStep gpio(uint8_t port, uint8_t value, uint32_t delayAfterMs = 0) {
    ControlStep step;
    step.transfer.out = {0x0e, port, value};
    step.transfer.inLength = 1;  // dvbsky_gpio_ctrl usa generic_rw(..., rlen=1)
    step.delayAfterMs = delayAfterMs;
    return step;
}

ControlStep streamWrite(uint8_t a, uint8_t b, uint8_t c, uint32_t delayAfterMs = 0) {
    ControlStep step;
    const uint8_t bytes[] = {a, b, c};
    step.transfer = genericWrite(bytes, sizeof(bytes));
    step.delayAfterMs = delayAfterMs;
    return step;
}

}  // namespace

BridgeTransfer buildI2cWrite(uint8_t address, const uint8_t* data, size_t length) {
    BridgeTransfer transfer;
    if (!validI2cArgs(address, data, length)) return transfer;
    transfer.out.reserve(length + 3);
    transfer.out = {0x08, address, static_cast<uint8_t>(length)};
    if (length) transfer.out.insert(transfer.out.end(), data, data + length);
    transfer.inLength = 1;
    return transfer;
}

BridgeTransfer buildI2cRead(uint8_t address, size_t length) {
    BridgeTransfer transfer;
    if (address > 0x7f || length > kMaxI2cMessage) return transfer;
    transfer.out = {0x09, 0, static_cast<uint8_t>(length), address};
    transfer.inLength = length + 1;
    return transfer;
}

BridgeTransfer buildI2cWriteRead(uint8_t address, const uint8_t* writeData,
                                 size_t writeLength, size_t readLength) {
    BridgeTransfer transfer;
    if (!validI2cArgs(address, writeData, writeLength) || readLength > kMaxI2cMessage)
        return transfer;
    transfer.out.reserve(writeLength + 4);
    transfer.out = {0x09, static_cast<uint8_t>(writeLength),
                    static_cast<uint8_t>(readLength), address};
    if (writeLength)
        transfer.out.insert(transfer.out.end(), writeData, writeData + writeLength);
    transfer.inLength = readLength + 1;
    return transfer;
}

bool extractI2cReadData(const uint8_t* bridgeReply, size_t replyLength,
                        size_t payloadLength, std::vector<uint8_t>& payload) {
    payload.clear();
    if (!bridgeReply || payloadLength > kMaxI2cMessage ||
        replyLength != payloadLength + 1)
        return false;
    payload.assign(bridgeReply + 1, bridgeReply + replyLength);
    return true;
}

SiCommandPlan buildSi2168Command(const uint8_t* command, size_t commandLength,
                                size_t responseLength, uint8_t address) {
    SiCommandPlan plan;
    if (!command || commandLength == 0 || commandLength > kMaxI2cMessage ||
        responseLength > kMaxI2cMessage || address > 0x7f)
        return plan;
    plan.write = buildI2cWrite(address, command, commandLength);
    if (responseLength) plan.poll = buildI2cRead(address, responseLength);
    plan.responseLength = responseLength;
    plan.valid = !plan.write.out.empty() &&
                 (!responseLength || !plan.poll.out.empty());
    return plan;
}

SiCommandPlan buildSi2168Init() {
    static const uint8_t command[] = {
        0xc0, 0x12, 0x00, 0x0c, 0x00, 0x0d, 0x16,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    return buildSi2168Command(command, sizeof(command), 0);
}

SiCommandPlan buildSi2168PowerUp() {
    static const uint8_t command[] = {0xc0, 0x06, 0x01, 0x0f, 0x00, 0x20, 0x20, 0x01};
    return buildSi2168Command(command, sizeof(command), 1);
}

SiCommandPlan buildSi2168GetRev() {
    static const uint8_t command[] = {0x02};
    return buildSi2168Command(command, sizeof(command), 13);
}

SiCommandState parseSiCommandState(const uint8_t* response, size_t responseLength) {
    if (!response || !responseLength) return SiCommandState::Invalid;
    if (!(response[0] & 0x80)) return SiCommandState::Busy;
    if (response[0] & 0x40) return SiCommandState::FirmwareError;
    return SiCommandState::Ready;
}

bool parseSi2168Revision(const uint8_t* response, size_t responseLength,
                         Si2168Revision& revision) {
    if (!response || responseLength < 13 ||
        parseSiCommandState(response, responseLength) != SiCommandState::Ready)
        return false;
    revision.chipId = (static_cast<uint32_t>(response[1]) << 24) |
                      (static_cast<uint32_t>(response[2]) << 16) |
                      (static_cast<uint32_t>(response[3]) << 8) | response[4];
    revision.versionChar = static_cast<char>(response[1]);
    revision.partNumber = response[2];
    revision.versionDigit1 = static_cast<char>(response[3]);
    revision.versionDigit2 = static_cast<char>(response[4]);
    revision.patch = response[5];
    return true;
}

bool isSi2168B40(const Si2168Revision& revision) {
    return revision.versionChar == 'B' && revision.partNumber == 0x68 &&
           revision.versionDigit1 == '4' && revision.versionDigit2 == '0';
}

SiCommandPlan buildSiTunerGetRev(uint8_t address) {
    static const uint8_t command[] = {0x02};
    return buildSi2168Command(command, sizeof(command), 13, address);
}

bool parseSiTunerRevision(const uint8_t* response, size_t responseLength,
                          SiTunerRevision& revision) {
    if (!response || responseLength < 13 ||
        parseSiCommandState(response, responseLength) != SiCommandState::Ready)
        return false;
    revision.versionChar = static_cast<char>(response[1]);
    revision.partId = response[2];
    revision.versionDigit1 = static_cast<char>(response[3]);
    revision.versionDigit2 = static_cast<char>(response[4]);
    revision.romId = response[12];
    return true;
}

std::vector<ControlStep> buildT230StartResetSequence() {
    // dvbsky_identify_state() para todos los IDs excepto MYGICA_T230A.
    return {
        gpio(0x04, 1, 20),
        gpio(0x83, 0),
        gpio(0xc0, 1, 100),
        gpio(0x83, 1),
        gpio(0xc0, 0, 50),
    };
}

std::vector<ControlStep> buildT230StreamControl(bool enabled) {
    std::vector<ControlStep> steps;
    steps.push_back(streamWrite(0x37, 0x00, 0x00, enabled ? 20 : 0));
    if (enabled) steps.push_back(streamWrite(0x36, 0x03, 0x00));
    return steps;
}

}  // namespace t230
