#include "siano_proto.h"

#include <cstring>

namespace siano {
namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::vector<uint8_t> message(MsgType type, const void* data, size_t len, uint8_t src) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(MsgHdr) + len);
    put16(out, static_cast<uint16_t>(type));
    out.push_back(src);
    out.push_back(DST_HIF);
    put16(out, static_cast<uint16_t>(sizeof(MsgHdr) + len));
    put16(out, 0);  // flags
    if (data && len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + len);
    }
    return out;
}

std::vector<uint8_t> message(MsgType type) { return message(type, nullptr, 0); }

std::vector<uint8_t> message(MsgType type, uint32_t word, uint8_t src) {
    return message(type, &word, sizeof(word), src);
}

std::vector<uint8_t> buildTune(uint32_t freq_hz, uint32_t bandwidth, uint32_t if_freq) {
    uint8_t payload[16];
    // Data[0]=frecuencia, Data[1]=ancho/modo, Data[2]=IF, Data[3]=0
    for (int i = 0; i < 4; ++i) {
        uint32_t w = 0;
        switch (i) {
            case 0: w = freq_hz; break;
            case 1: w = bandwidth; break;
            case 2: w = if_freq; break;
            default: w = 0; break;
        }
        payload[i * 4 + 0] = static_cast<uint8_t>(w & 0xff);
        payload[i * 4 + 1] = static_cast<uint8_t>((w >> 8) & 0xff);
        payload[i * 4 + 2] = static_cast<uint8_t>((w >> 16) & 0xff);
        payload[i * 4 + 3] = static_cast<uint8_t>((w >> 24) & 0xff);
    }
    return message(MSG_ISDBT_TUNE, payload, sizeof(payload), SRC_BDA);
}

std::vector<uint8_t> buildPidFilter(MsgType type, uint32_t pid) {
    return message(type, pid, SRC_BDA);
}

std::vector<uint8_t> buildDataDownload(uint32_t address, const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(MsgHdr) + 4 + len);
    put16(out, MSG_DATA_DOWNLOAD);
    out.push_back(SRC_HOST);
    out.push_back(DST_HIF);
    put16(out, static_cast<uint16_t>(sizeof(MsgHdr) + 4 + len));
    put16(out, 0);
    put32(out, address);
    out.insert(out.end(), data, data + len);
    return out;
}

std::vector<std::vector<uint8_t>> buildFirmwareSequence(const uint8_t* fw, size_t size,
                                                        bool running) {
    std::vector<std::vector<uint8_t>> out;
    if (!fw || size < sizeof(FirmwareHdr)) return out;

    FirmwareHdr hdr{};
    std::memcpy(&hdr, fw, sizeof(hdr));
    if (hdr.length > size - sizeof(FirmwareHdr) || hdr.length == 0) return out;

    const uint8_t* payload = fw + sizeof(FirmwareHdr);
    uint32_t address = hdr.start_address;

    if (running) {
        // El chip ya corre firmware: hay que pedir el reload y usar la
        // direccion de entrada que viene en payload[20] (offset 32 del .inp).
        out.push_back(message(MSG_SW_RELOAD_START));
        if (hdr.length >= 24) {
            address = get32(payload + 20);
        }
    }

    size_t remaining = hdr.length;
    const uint8_t* p = payload;
    while (remaining > 0) {
        size_t chunk = remaining > MAX_PAYLOAD ? MAX_PAYLOAD : remaining;
        out.push_back(buildDataDownload(address, p, chunk));
        address += static_cast<uint32_t>(chunk);
        p += chunk;
        remaining -= chunk;
    }

    uint8_t validity[12];
    const uint32_t words[3] = {hdr.start_address, hdr.length, 0};
    for (int i = 0; i < 3; ++i) {
        validity[i * 4 + 0] = static_cast<uint8_t>(words[i] & 0xff);
        validity[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 8) & 0xff);
        validity[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 16) & 0xff);
        validity[i * 4 + 3] = static_cast<uint8_t>((words[i] >> 24) & 0xff);
    }
    out.push_back(message(MSG_DATA_VALIDITY, validity, sizeof(validity)));

    if (running) {
        out.push_back(message(MSG_SW_RELOAD_EXEC));
    } else {
        // {entry point, prioridad, tamano de pila, parametro, task id}
        const uint32_t trigger[5] = {hdr.start_address, 6, 0x200, 0, 4};
        uint8_t buf[20];
        for (int i = 0; i < 5; ++i) {
            buf[i * 4 + 0] = static_cast<uint8_t>(trigger[i] & 0xff);
            buf[i * 4 + 1] = static_cast<uint8_t>((trigger[i] >> 8) & 0xff);
            buf[i * 4 + 2] = static_cast<uint8_t>((trigger[i] >> 16) & 0xff);
            buf[i * 4 + 3] = static_cast<uint8_t>((trigger[i] >> 24) & 0xff);
        }
        out.push_back(message(MSG_SWDOWNLOAD_TRIGGER, buf, sizeof(buf)));
    }
    return out;
}

uint16_t peekType(const uint8_t* buf, size_t len) {
    if (!buf || len < 2) return 0;
    return static_cast<uint16_t>(buf[0] | (buf[1] << 8));
}

uint32_t peekWord(const uint8_t* buf, size_t len, size_t offset) {
    if (!buf || offset + 4 > len) return 0;
    return get32(buf + offset);
}

}  // namespace siano
