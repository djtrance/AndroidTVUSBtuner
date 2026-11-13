// test_proto.cpp - prueba de escritorio del protocolo Siano.
//
// Compila senza NDK ni JNI: solo siano_proto.cpp. Verifica la secuencia de
// firmware contra el .inp real y los bytes exactos de los mensajes.
//
//   clang++ -std=c++17 -Wall -Wextra -I app/src/main/cpp \
//       app/src/main/cpp/siano_proto.cpp tests/test_proto.cpp \
//       -o /tmp/test_siano && /tmp/test_siano ../../fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "siano_proto.h"

namespace {

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

int failures = 0;

void expect(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) ++failures;
}

void dump(const char* tag, const std::vector<uint8_t>& v) {
    std::printf("%s (%zu bytes):", tag, v.size());
    for (size_t i = 0; i < v.size() && i < 28; ++i) std::printf(" %02x", v[i]);
    if (v.size() > 28) std::printf(" ...");
    std::printf("\n");
}

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> l) {
    return std::vector<uint8_t>(l);
}

}  // namespace

int main(int argc, char** argv) {
    const char* fwPath = argc > 1 ? argv[1]
                                  : "../../fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp";

    std::printf("== mensajes sueltos ==\n");

    // INIT_DEVICE con modo ISDBT_BDA (6): 578, src 0, dst 11, len 12
    auto init = siano::message(siano::MSG_INIT_DEVICE, 6u, siano::SRC_HOST);
    dump("init(6)", init);
    expect(init == bytes({0x42, 0x02, 0x00, 0x0b, 0x0c, 0x00, 0x00, 0x00,
                          0x06, 0x00, 0x00, 0x00}),
           "INIT_DEVICE = 578 / src 0 / dst 11 / len 12 / modo 6");

    // RECEIVE_1SEG_THROUGH_FULLSEG = 1 -> 528
    auto one = siano::message(siano::MSG_RECEIVE_1SEG_THROUGH_FULLSEG, 1u, siano::SRC_HOST);
    dump("1seg_through_fullseg", one);
    expect(one == bytes({0x10, 0x02, 0x00, 0x0b, 0x0c, 0x00, 0x00, 0x00,
                         0x01, 0x00, 0x00, 0x00}),
           "RECEIVE_1SEG_THROUGH_FULLSEG = 528 con valor 1");

    // ISDBT_TUNE 776: {freq, bw, 12 MHz, 0}, src 201, len 24
    auto tune = siano::buildTune(473142857u, siano::BW_ISDBT_13SEG);
    dump("tune 13seg", tune);
    expect(tune.size() == 24, "ISDBT_TUNE mide 24 bytes");
    expect(siano::peekType(tune.data(), tune.size()) == siano::MSG_ISDBT_TUNE,
           "ISDBT_TUNE = 776");
    expect(tune[2] == 201 && tune[3] == 11, "src = 201 (BDA) / dst = 11 (HIF)");
    expect(tune[4] == 24 && tune[5] == 0, "length = 24 incluye cabecera");
    // freq 473142857 = 0x1C339649
    expect(tune[8] == 0x49 && tune[9] == 0x96 && tune[10] == 0x33 && tune[11] == 0x1c,
           "Data[0] = frecuencia en Hz, little-endian");
    expect(tune[12] == 8, "Data[1] = 8 (BW_ISDBT_13SEG)");
    expect(tune[16] == 0x00 && tune[17] == 0x1b && tune[18] == 0xb7 && tune[19] == 0x00,
           "Data[2] = 12 000 000 (0x00B71B00)");
    expect(tune[20] == 0 && tune[21] == 0 && tune[22] == 0 && tune[23] == 0,
           "Data[3] = 0");

    auto tune1 = siano::buildTune(473142857u, siano::BW_ISDBT_1SEG);
    expect(tune1[12] == 4, "Data[1] = 4 (BW_ISDBT_1SEG)");

    // filtros y estadisticas
    auto add = siano::buildPidFilter(siano::MSG_ADD_PID_FILTER, 0x1fff);
    dump("add_pid(0x1fff)", add);
    expect(siano::peekType(add.data(), add.size()) == 601 && add[2] == 201,
           "ADD_PID_FILTER = 601 con src = 201");
    auto stats = siano::message(siano::MSG_GET_STATISTICS_EX, nullptr, 0, siano::SRC_BDA);
    dump("stats", stats);
    expect(stats.size() == 8 && stats[2] == 201, "GET_STATISTICS_EX = 653, sin payload");

    std::printf("\n== secuencia de firmware ==\n");
    auto fw = readFile(fwPath);
    if (fw.size() < 12) {
        std::printf("[-] no pude leer %s (pasa la ruta como argumento)\n", fwPath);
        return 1;
    }
    siano::FirmwareHdr hdr{};
    std::memcpy(&hdr, fw.data(), sizeof(hdr));
    std::printf("[+] %s\n    %zu bytes check=0x%x length=0x%x start=0x%x\n",
                fwPath, fw.size(), hdr.check_sum, hdr.length, hdr.start_address);
    expect(hdr.start_address == siano::FW_START_ADDRESS, "start_address = 0x00040260");
    expect(hdr.length + 12 == fw.size(), "size == length + 12");

    const uint32_t expectedChunks = (hdr.length + siano::MAX_PAYLOAD - 1) / siano::MAX_PAYLOAD;
    auto seq = siano::buildFirmwareSequence(fw.data(), fw.size(), false);
    std::printf("[+] arranque frio: %zu mensajes\n", seq.size());
    expect(seq.size() == expectedChunks + 2, "chunks + SWDOWNLOAD_TRIGGER = total");

    dump("  [0]", seq.front());
    dump("  [last-1]", seq[seq.size() - 2]);
    dump("  [last]", seq.back());

    uint32_t addr = hdr.start_address;
    uint32_t sent = 0;
    bool contiguous = true;
    for (const auto& m : seq) {
        if (siano::peekType(m.data(), m.size()) != siano::MSG_DATA_DOWNLOAD) continue;
        if (siano::peekWord(m.data(), m.size(), 8) != addr) contiguous = false;
        addr += static_cast<uint32_t>(m.size() - 12);
        sent += static_cast<uint32_t>(m.size() - 12);
    }
    expect(contiguous, "DATA_DOWNLOAD con direcciones contiguas");
    expect(sent == hdr.length, "se envian exactamente length bytes");

    // DATA_VALIDITY {start, length, 0}
    const auto& validity = seq[seq.size() - 2];
    dump("  validity", validity);
    expect(siano::peekType(validity.data(), validity.size()) == 662,
           "DATA_VALIDITY = 662");
    expect(siano::peekWord(validity.data(), validity.size(), 8) == hdr.start_address &&
               siano::peekWord(validity.data(), validity.size(), 12) == hdr.length,
           "DATA_VALIDITY lleva {start, length}");

    // SWDOWNLOAD_TRIGGER {entry, 6, 0x200, 0, 4}
    const auto& trigger = seq.back();
    dump("  trigger", trigger);
    expect(siano::peekType(trigger.data(), trigger.size()) == 664,
           "SWDOWNLOAD_TRIGGER = 664");
    expect(siano::peekWord(trigger.data(), trigger.size(), 8) == hdr.start_address,
           "trigger.entry = start_address en arranque frio");
    expect(siano::peekWord(trigger.data(), trigger.size(), 12) == 6 &&
               siano::peekWord(trigger.data(), trigger.size(), 16) == 0x200,
           "trigger = {entry, 6, 0x200, ...}");

    // modo "running": RELOAD_START + entry desde payload[20] + RELOAD_EXEC
    auto run = siano::buildFirmwareSequence(fw.data(), fw.size(), true);
    std::printf("[+] modo running: %zu mensajes\n", run.size());
    expect(siano::peekType(run.front().data(), run.front().size()) == 702,
           "empieza con SW_RELOAD_START = 702");
    expect(siano::peekType(run.back().data(), run.back().size()) == 704,
           "termina con SW_RELOAD_EXEC = 704");
    uint32_t entry = 0;
    std::memcpy(&entry, fw.data() + 12 + 20, 4);
    bool entryOk = false;
    for (const auto& m : run) {
        if (siano::peekType(m.data(), m.size()) == siano::MSG_DATA_DOWNLOAD) {
            entryOk = siano::peekWord(m.data(), m.size(), 8) == entry;
            break;
        }
    }
    std::printf("    payload[20] = 0x%x\n", entry);
    expect(entryOk, "running usa la direccion de entrada de payload[20]");

    std::printf("\n%s (%d fallos)\n", failures ? "FALLO" : "TODO OK", failures);
    return failures ? 1 : 0;
}
