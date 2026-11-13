// Pruebas offline de los builders/parsers del puente DVBsky/T230.
// Compilar desde driver-skeleton:
//   c++ -std=c++17 -Wall -Wextra -pedantic -I app/src/main/cpp \
//       app/src/main/cpp/t230_proto.cpp tests/test_t230_proto.cpp -o /tmp/test_t230_proto
//   /tmp/test_t230_proto

#include <cstdio>
#include <initializer_list>
#include <vector>

#include "t230_proto.h"

namespace {

int failures = 0;

void expect(bool condition, const char* description) {
    std::printf("  [%s] %s\n", condition ? " ok " : "FAIL", description);
    if (!condition) ++failures;
}

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> input) {
    return std::vector<uint8_t>(input);
}

}  // namespace

int main() {
    using namespace t230;

    std::printf("== puente DVBsky / I2C ==\n");
    const uint8_t reg[] = {0x12, 0x34};
    const auto write = buildI2cWrite(kDemodAddress, reg, sizeof(reg));
    expect(write.out == bytes({0x08, 0x64, 0x02, 0x12, 0x34}) && write.inLength == 1,
           "I2C write usa 08 addr len data y espera ACK de un byte");

    const auto read = buildI2cRead(kDemodAddress, 13);
    expect(read.out == bytes({0x09, 0x00, 0x0d, 0x64}) && read.inLength == 14,
           "I2C read usa 09 00 len addr y reserva byte inicial");

    const auto combined = buildI2cWriteRead(0x51, reg, sizeof(reg), 6);
    expect(combined.out == bytes({0x09, 0x02, 0x06, 0x51, 0x12, 0x34}) &&
               combined.inLength == 7,
           "I2C write/read usa 09 wlen rlen addr data");
    const uint8_t rawReply[] = {0xa5, 1, 2, 3};
    std::vector<uint8_t> payload;
    expect(extractI2cReadData(rawReply, sizeof(rawReply), 3, payload) &&
               payload == bytes({1, 2, 3}),
           "parseo de lectura descarta byte inicial del puente");
    expect(!extractI2cReadData(rawReply, sizeof(rawReply) - 1, 3, payload),
           "rechaza respuestas I2C truncadas");
    expect(buildI2cWrite(kDemodAddress, reg, 61).out.empty(),
           "rechaza mensajes I2C mayores de 60 bytes");

    std::printf("\n== Si2168 GET_REV ==\n");
    const auto init = buildSi2168Init();
    expect(init.valid && init.write.out == bytes({
               0x08, 0x64, 0x0d, 0xc0, 0x12, 0x00, 0x0c, 0x00, 0x0d, 0x16,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) && init.responseLength == 0,
           "INIT coincide con la secuencia del driver Linux");
    const auto power = buildSi2168PowerUp();
    expect(power.valid && power.write.out == bytes({
               0x08, 0x64, 0x08, 0xc0, 0x06, 0x01, 0x0f, 0x00, 0x20, 0x20, 0x01}) &&
               power.poll.out == bytes({0x09, 0x00, 0x01, 0x64}) &&
               power.poll.inLength == 2,
           "POWER_UP es el comando documentado y su poll devuelve un byte SiLabs");
    const auto getRev = buildSi2168GetRev();
    expect(getRev.valid && getRev.write.out == bytes({0x08, 0x64, 0x01, 0x02}) &&
               getRev.poll.out == bytes({0x09, 0x00, 0x0d, 0x64}),
           "GET_REV = 02 con respuesta de 13 bytes");

    const uint8_t busy[] = {0x00};
    const uint8_t ready[] = {0x80};
    const uint8_t error[] = {0xc0};
    expect(parseSiCommandState(busy, sizeof(busy)) == SiCommandState::Busy &&
               parseSiCommandState(ready, sizeof(ready)) == SiCommandState::Ready &&
               parseSiCommandState(error, sizeof(error)) == SiCommandState::FirmwareError,
           "decodifica CTS/error en byte de estado SiLabs");

    uint8_t demodReply[13] = {0x80, 'B', 0x68, '4', '0', 0x01};
    Si2168Revision demod{};
    expect(parseSi2168Revision(demodReply, sizeof(demodReply), demod) &&
               demod.chipId == 0x42683430 && isSi2168B40(demod),
           "identifica Si2168-B40 desde los bytes GET_REV");
    demodReply[0] = 0x00;
    expect(!parseSi2168Revision(demodReply, sizeof(demodReply), demod),
           "no acepta GET_REV mientras CTS siga bajo");

    const auto tunerRevPlan = buildSiTunerGetRev();
    expect(tunerRevPlan.valid && tunerRevPlan.write.out == bytes({0x08, 0x60, 0x01, 0x02}) &&
               tunerRevPlan.poll.out == bytes({0x09, 0x00, 0x0d, 0x60}),
           "GET_REV tuner usa la dirección confirmada 0x60");
    uint8_t tunerReply[13] = {0x80, 'A', 0x48, '2', '0', 0x00};
    tunerReply[12] = 0x33;
    SiTunerRevision tuner{};
    expect(parseSiTunerRevision(tunerReply, sizeof(tunerReply), tuner) &&
               tuner.partId == 0x48 && tuner.versionChar == 'A' &&
               tuner.versionDigit1 == '2' && tuner.versionDigit2 == '0' &&
               tuner.romId == 0x33,
           "parsea part ID y ROM ID del tuner sin asumir alias de firmware");

    std::printf("\n== inicio/reset y flujo TS T230 ==\n");
    const auto reset = buildT230StartResetSequence();
    expect(reset.size() == 5 && reset[0].transfer.out == bytes({0x0e, 0x04, 0x01}) &&
               reset[0].transfer.inLength == 1 && reset[0].delayAfterMs == 20 &&
               reset[1].transfer.out == bytes({0x0e, 0x83, 0x00}) &&
               reset[2].transfer.out == bytes({0x0e, 0xc0, 0x01}) &&
               reset[2].delayAfterMs == 100 &&
               reset[3].transfer.out == bytes({0x0e, 0x83, 0x01}) &&
               reset[4].transfer.out == bytes({0x0e, 0xc0, 0x00}) &&
               reset[4].delayAfterMs == 50,
           "secuencia GPIO reproduce identify_state para T230 no-T230A");

    const auto streamOn = buildT230StreamControl(true);
    expect(streamOn.size() == 2 &&
               streamOn[0].transfer.out == bytes({0x37, 0x00, 0x00}) &&
               streamOn[0].transfer.inLength == 0 && streamOn[0].delayAfterMs == 20 &&
               streamOn[1].transfer.out == bytes({0x36, 0x03, 0x00}),
           "habilitar TS hace pre-write, espera 20 ms y post-write");
    const auto streamOff = buildT230StreamControl(false);
    expect(streamOff.size() == 1 &&
               streamOff[0].transfer.out == bytes({0x37, 0x00, 0x00}) &&
               streamOff[0].delayAfterMs == 0,
           "deshabilitar TS solo hace el pre-write" );

    std::printf("\n%s (%d fallos)\n", failures ? "FALLO" : "TODO OK", failures);
    return failures ? 1 : 0;
}
