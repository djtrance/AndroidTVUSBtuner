// pt230_host.cpp - habla el protocolo Siano del PT230 desde macOS/Linux con libusb.
//
// No necesita Android ni el driver propietario: reutiliza los constructores de
// mensajes de driver-skeleton (siano_proto.cpp) y hace el I/O con libusb.
//
// Compilar (macOS con Homebrew):
//   clang++ -std=c++17 -O2 -Wall -Wextra \
//       -I driver-skeleton/app/src/main/cpp \
//       tools/pt230_host.cpp driver-skeleton/app/src/main/cpp/siano_proto.cpp \
//       -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib \
//       -Wl,-rpath,/opt/homebrew/lib -lusb-1.0 -o /tmp/pt230_host
//
// Uso:
//   /tmp/pt230_host --probe
//   /tmp/pt230_host --fw fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp \
//                   --freq 473142857 --bw 8 --seconds 8 --out /tmp/pt230.ts
//
// El firmware vive en RAM del chip: si algo sale mal, desconecta y reconecta.
// La herramienta es de solo lectura sobre el dongle salvo la descarga de
// firmware, que es justamente la operacion normal de arranque.

#include <libusb.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "siano_proto.h"

namespace {

constexpr int EP_OUT = 0x02;
constexpr int EP_IN = 0x81;
constexpr int IN_BUF = 0x4000;      // mismo buffer que libpt27xDrv.so
constexpr int RESPONSE_ALIGN = 504; // 512 - 8, MSG_HDR_FLAG_SPLIT_MSG
constexpr uint16_t FLAG_SPLIT = 0x0004;

struct Id {
    uint16_t vid;
    uint16_t pid;
    const char* name;
};

const Id kIds[] = {
    {0x1f4d, 0xa230, "PT230 (Rio; chip_model=0x2270/SMS2270)"},
    {0x1f4d, 0xa270, "PT230 (variante a270)"},
    {0x3275, 0x0080, "PLEX PX-S1UD"},
    {0x187f, 0x0600, "Siano Rio"},
};

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

unsigned long long g_splitMessages = 0;  // diagnosticos de framing
volatile std::sig_atomic_t g_stopRequested = 0;
int g_tsOutputFd = STDOUT_FILENO;

void requestStop(int) {
    g_stopRequested = 1;
}

bool writeAll(int fd, const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

const char* msgName(uint16_t t) {
    switch (t) {
        case 579: return "INIT_DEVICE_RES";
        case 602: return "ADD_PID_FILTER_RES";
        case 604: return "REMOVE_PID_FILTER_RES";
        case 654: return "GET_STATISTICS_EX_RES";
        case 661: return "DATA_DOWNLOAD_RES";
        case 663: return "DATA_VALIDITY_RES";
        case 665: return "SWDOWNLOAD_TRIGGER_RES";
        case 669: return "GET_VERSION_EX_RES";
        case 693: return "DVBT_BDA_DATA (TS)";
        case 529: return "RECEIVE_1SEG_THROUGH_FULLSEG_RES";
        case 703: return "SW_RELOAD_START_RES";
        case 705: return "SW_RELOAD_EXEC_RES";
        case 777: return "ISDBT_TUNE_RES";
        case 805: return "INTERFACE_LOCK_IND";
        case 806: return "INTERFACE_UNLOCK_IND";
        case 827: return "SIGNAL_DETECTED_IND";
        case 828: return "NO_SIGNAL_IND";
        default: return "?";
    }
}

void hexdump(const std::vector<uint8_t>& v, size_t maxn = 48) {
    size_t n = v.size() < maxn ? v.size() : maxn;
    for (size_t i = 0; i < n; ++i) std::printf(" %02x", v[i]);
    if (v.size() > maxn) std::printf(" ...");
}

struct Dev {
    libusb_context* ctx = nullptr;
    libusb_device_handle* h = nullptr;
    uint16_t vid = 0, pid = 0;
    bool ackMode = true;
    std::vector<uint8_t> lastPayload;

    ~Dev() { close(); }

    bool findAndOpen(bool verbose) {
        if (!ctx && libusb_init(&ctx) < 0) {
            std::printf("[-] libusb_init fallo\n");
            return false;
        }
        libusb_device** list = nullptr;
        ssize_t n = libusb_get_device_list(ctx, &list);
        libusb_device* found = nullptr;
        for (ssize_t i = 0; i < n && !found; ++i) {
            struct libusb_device_descriptor dd;
            if (libusb_get_device_descriptor(list[i], &dd) < 0) continue;
            for (const auto& id : kIds) {
                if (dd.idVendor == id.vid && dd.idProduct == id.pid) {
                    found = list[i];
                    vid = id.vid;
                    pid = id.pid;
                    if (verbose) std::printf("[+] %04x:%04x  %s\n", vid, pid, id.name);
                    break;
                }
            }
        }
        if (!found) {
            std::printf("[-] no encontre el dongle (buscaba 1f4d:a230 / a270 / 3275:0080 / 187f:0600)\n");
            libusb_free_device_list(list, 1);
            return false;
        }
        libusb_ref_device(found);
        int r = libusb_open(found, &h);
        libusb_free_device_list(list, 1);
        if (r < 0) {
            std::printf("[-] libusb_open: %s\n", libusb_error_name(r));
            h = nullptr;
            return false;
        }
#ifdef __linux__
        libusb_set_auto_detach_kernel_driver(h, 1);
#endif
        r = libusb_claim_interface(h, 0);
        if (r < 0) {
            std::printf("[-] claim interface 0: %s\n", libusb_error_name(r));
            libusb_close(h);
            h = nullptr;
            return false;
        }
        if (verbose) std::printf("[+] interfaz 0 reclamada\n");
        return true;
    }

    void close() {
        if (h) {
            libusb_release_interface(h, 0);
            libusb_close(h);
            h = nullptr;
        }
        if (ctx) {
            libusb_exit(ctx);
            ctx = nullptr;
        }
    }

    bool send(const std::vector<uint8_t>& msg) {
        int tr = 0;
        int r = libusb_bulk_transfer(h, EP_OUT, const_cast<uint8_t*>(msg.data()),
                                     static_cast<int>(msg.size()), &tr, 1000);
        if (r < 0 || tr != static_cast<int>(msg.size())) {
            std::printf("[-] bulk OUT: %s (%d/%zu)\n", libusb_error_name(r), tr, msg.size());
            return false;
        }
        return true;
    }

    // Lee un mensaje. Devuelve false si no hubo datos en el timeout.
    bool recv(uint16_t& type, std::vector<uint8_t>& payload, int timeout_ms) {
        std::vector<uint8_t> raw(IN_BUF);
        int tr = 0;
        int r = libusb_bulk_transfer(h, EP_IN, raw.data(), IN_BUF, &tr, timeout_ms);
        if (r == LIBUSB_ERROR_TIMEOUT) return false;
        if (r < 0) {
            std::printf("[-] bulk IN: %s\n", libusb_error_name(r));
            return false;
        }
        if (tr < 8) return false;
        raw.resize(static_cast<size_t>(tr));

        uint16_t flags = static_cast<uint16_t>(raw[6] | (raw[7] << 8));
        size_t off = 0;
        if (flags & FLAG_SPLIT) {
            ++g_splitMessages;
            off = RESPONSE_ALIGN + ((flags >> 8) & 3);
            if (off + 8 > raw.size()) return false;
            std::memmove(&raw[off], raw.data(), 8);
        }
        type = static_cast<uint16_t>(raw[off] | (raw[off + 1] << 8));
        uint16_t mlen = static_cast<uint16_t>(raw[off + 4] | (raw[off + 5] << 8));
        if (mlen < 8 || off + mlen > raw.size()) {
            std::printf("[!] mensaje truncado type=%u len=%u bufsize=%zu off=%zu\n",
                        type, mlen, raw.size(), off);
            return false;
        }
        payload.assign(raw.begin() + static_cast<long>(off + 8),
                       raw.begin() + static_cast<long>(off + mlen));
        return true;
    }

    bool sendWait(const std::vector<uint8_t>& msg, uint16_t expect,
                  int timeout_ms = 400, bool print = true) {
        uint16_t type = siano::peekType(msg.data(), msg.size());
        if (!send(msg)) return false;
        if (!ackMode) return true;
        long long deadline = nowMs() + timeout_ms;
        while (nowMs() < deadline) {
            uint16_t rt = 0;
            std::vector<uint8_t> p;
            if (!recv(rt, p, static_cast<int>(deadline - nowMs()))) continue;
            if (print && rt != 693) {
                std::printf("  <- %u %s (%zu B)", rt, msgName(rt), p.size());
                hexdump(p, 24);
                std::printf("\n");
            }
            if (rt == expect) {
                lastPayload = p;
                return true;
            }
        }
        if (print)
            std::printf("  [!] sin respuesta %u para type=%u\n", expect, type);
        return false;
    }
};

bool looksLikeTs(const uint8_t* b, size_t n) {
    if (n < 188) return false;
    size_t total = 0, sync = 0;
    for (size_t off = 0; off + 188 <= n; off += 188) {
        ++total;
        if (b[off] == 0x47) ++sync;
    }
    return total && sync * 10 >= total * 9;
}

struct PsiSection {
    uint16_t pid;
    std::vector<uint8_t> bytes;
};

void extractCompleteSections(uint16_t pid, std::vector<uint8_t>& pending,
                             std::vector<PsiSection>& out) {
    while (pending.size() >= 3) {
        if (pending[0] == 0xff) {
            pending.clear();
            return;
        }
        const size_t total = 3 + static_cast<size_t>(((pending[1] & 0x0f) << 8) | pending[2]);
        if (total < 3 || total > 4096) {
            pending.clear();
            return;
        }
        if (pending.size() < total) return;
        out.push_back({pid, std::vector<uint8_t>(pending.begin(), pending.begin() + static_cast<long>(total))});
        pending.erase(pending.begin(), pending.begin() + static_cast<long>(total));
    }
}

std::vector<PsiSection> collectPsiSections(const std::vector<uint8_t>& ts,
                                           const std::set<uint16_t>& wanted) {
    std::vector<PsiSection> out;
    std::unordered_map<uint16_t, std::vector<uint8_t>> pending;
    for (size_t off = 0; off + 188 <= ts.size(); off += 188) {
        const uint8_t* pkt = ts.data() + off;
        if (pkt[0] != 0x47) continue;
        const uint16_t pid = static_cast<uint16_t>(((pkt[1] & 0x1f) << 8) | pkt[2]);
        if (!wanted.count(pid)) continue;
        const uint8_t afc = static_cast<uint8_t>((pkt[3] >> 4) & 3);
        if (!(afc & 1)) continue;
        size_t payload = 4;
        if (afc & 2) payload += 1 + pkt[4];
        if (payload >= 188) continue;
        const bool start = (pkt[1] & 0x40) != 0;
        auto& buffer = pending[pid];
        if (start) {
            const size_t pointer = pkt[payload];
            ++payload;
            if (payload + pointer > 188) {
                buffer.clear();
                continue;
            }
            if (!buffer.empty() && pointer > 0) {
                buffer.insert(buffer.end(), pkt + payload, pkt + payload + pointer);
                extractCompleteSections(pid, buffer, out);
            }
            buffer.clear();
            payload += pointer;
            while (payload + 3 <= 188 && pkt[payload] != 0xff) {
                const size_t sectionSize = 3 + static_cast<size_t>(((pkt[payload + 1] & 0x0f) << 8) |
                                                                   pkt[payload + 2]);
                if (sectionSize > 4096) break;
                if (payload + sectionSize <= 188) {
                    out.push_back({pid, std::vector<uint8_t>(pkt + payload, pkt + payload + sectionSize)});
                    payload += sectionSize;
                } else {
                    buffer.assign(pkt + payload, pkt + 188);
                    break;
                }
            }
        } else if (!buffer.empty()) {
            buffer.insert(buffer.end(), pkt + payload, pkt + 188);
            extractCompleteSections(pid, buffer, out);
        }
    }
    return out;
}

std::string decodeBroadcastText(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = data[i];
        if (c == 0 || c == 0xff) break;
        if (c < 0x20) {
            if (c == '\n' || c == '\r') out.push_back(' ');
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            // Los servicios ISDB-T de la muestra usan ASCII/Latin-1. Convierte
            // bytes Latin-1 a UTF-8 para pasarlos seguros por la pipe de Swift.
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return out;
}

std::string percentEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

struct VideoProgram {
    uint16_t program = 0;
    uint16_t pid = 0x1fff;
    uint8_t streamType = 0;
    uint8_t serviceType = 0;
    std::string provider;
    std::string name;
    std::string epgTitle;
    std::string epgText;
};

bool isVideoStreamType(uint8_t type) {
    return type == 0x01 || type == 0x02 || type == 0x10 ||
           type == 0x1b || type == 0x24 || type == 0x42;
}

std::vector<VideoProgram> findVideoPrograms(const std::vector<uint8_t>& ts) {
    std::map<uint16_t, uint16_t> programToPmt;
    const auto patSections = collectPsiSections(ts, {0});
    for (const auto& section : patSections) {
        const auto& s = section.bytes;
        if (s.size() < 12 || s[0] != 0x00) continue;
        const size_t end = s.size() - 4;
        for (size_t q = 8; q + 4 <= end; q += 4) {
            const uint16_t program = static_cast<uint16_t>((s[q] << 8) | s[q + 1]);
            const uint16_t pmtPid = static_cast<uint16_t>(((s[q + 2] & 0x1f) << 8) | s[q + 3]);
            if (program != 0) programToPmt[program] = pmtPid;
        }
    }
    if (programToPmt.empty()) return {};

    std::set<uint16_t> wanted{0x11, 0x12};  // SDT + EIT actual/present-following
    for (const auto& p : programToPmt) wanted.insert(p.second);
    const auto sections = collectPsiSections(ts, wanted);

    struct ServiceInfo {
        uint8_t type = 0;
        std::string provider;
        std::string name;
        std::string epgTitle;
        std::string epgText;
        bool epgIsRunning = false;
    };
    std::map<uint16_t, ServiceInfo> services;

    for (const auto& item : sections) {
        const auto& s = item.bytes;
        if (s.size() < 8) continue;
        if (item.pid == 0x11 && (s[0] == 0x42 || s[0] == 0x46)) {
            size_t q = 11;
            const size_t end = s.size() - 4;
            while (q + 5 <= end) {
                const uint16_t serviceId = static_cast<uint16_t>((s[q] << 8) | s[q + 1]);
                const size_t descriptorLength = static_cast<size_t>(((s[q + 3] & 0x0f) << 8) | s[q + 4]);
                size_t d = q + 5;
                const size_t descriptorEnd = std::min(end, d + descriptorLength);
                while (d + 2 <= descriptorEnd) {
                    const uint8_t tag = s[d];
                    const size_t len = s[d + 1];
                    const size_t start = d + 2;
                    if (start + len > descriptorEnd) break;
                    if (tag == 0x48 && len >= 3) {
                        ServiceInfo& info = services[serviceId];
                        info.type = s[start];
                        const size_t providerLen = s[start + 1];
                        if (providerLen + 2 <= len) {
                            info.provider = decodeBroadcastText(s.data() + start + 2, providerLen);
                            const size_t nameLenPos = start + 2 + providerLen;
                            const size_t nameLen = s[nameLenPos];
                            if (nameLenPos + 1 + nameLen <= start + len) {
                                info.name = decodeBroadcastText(s.data() + nameLenPos + 1, nameLen);
                            }
                        }
                    }
                    d = start + len;
                }
                q = descriptorEnd;
            }
        } else if (item.pid == 0x12 && (s[0] == 0x4e || s[0] == 0x4f) && s.size() >= 18) {
            const uint16_t serviceId = static_cast<uint16_t>((s[3] << 8) | s[4]);
            size_t q = 14;
            const size_t end = s.size() - 4;
            while (q + 12 <= end) {
                const uint8_t runningStatus = static_cast<uint8_t>((s[q + 10] >> 5) & 7);
                const size_t descriptorLength = static_cast<size_t>(((s[q + 10] & 0x0f) << 8) | s[q + 11]);
                size_t d = q + 12;
                const size_t descriptorEnd = std::min(end, d + descriptorLength);
                while (d + 2 <= descriptorEnd) {
                    const uint8_t tag = s[d];
                    const size_t len = s[d + 1];
                    const size_t start = d + 2;
                    if (start + len > descriptorEnd) break;
                    if (tag == 0x4d && len >= 5) {
                        const size_t eventNameLen = s[start + 3];
                        if (start + 4 + eventNameLen < start + len) {
                            const size_t textLenPos = start + 4 + eventNameLen;
                            const size_t textLen = s[textLenPos];
                            if (textLenPos + 1 + textLen <= start + len) {
                                ServiceInfo& info = services[serviceId];
                                if (info.epgTitle.empty() || (runningStatus == 4 && !info.epgIsRunning)) {
                                    info.epgTitle = decodeBroadcastText(s.data() + start + 4, eventNameLen);
                                    info.epgText = decodeBroadcastText(s.data() + textLenPos + 1, textLen);
                                    info.epgIsRunning = runningStatus == 4;
                                }
                            }
                        }
                    }
                    d = start + len;
                }
                q = descriptorEnd;
            }
        }
    }

    std::map<uint16_t, VideoProgram> results;
    for (const auto& item : sections) {
        const auto programIt = std::find_if(programToPmt.begin(), programToPmt.end(),
            [&item](const auto& p) { return p.second == item.pid; });
        if (programIt == programToPmt.end()) continue;
        const auto& s = item.bytes;
        if (s.size() < 16 || s[0] != 0x02) continue;
        const size_t programInfoLength = static_cast<size_t>(((s[10] & 0x0f) << 8) | s[11]);
        size_t q = 12 + programInfoLength;
        const size_t end = s.size() - 4;
        while (q + 5 <= end) {
            const uint8_t streamType = s[q];
            const uint16_t elementaryPid = static_cast<uint16_t>(((s[q + 1] & 0x1f) << 8) | s[q + 2]);
            const size_t infoLength = static_cast<size_t>(((s[q + 3] & 0x0f) << 8) | s[q + 4]);
            if (isVideoStreamType(streamType) && !results.count(programIt->first)) {
                VideoProgram video;
                video.program = programIt->first;
                video.pid = elementaryPid;
                video.streamType = streamType;
                if (const auto service = services.find(video.program); service != services.end()) {
                    video.serviceType = service->second.type;
                    video.provider = service->second.provider;
                    video.name = service->second.name;
                    video.epgTitle = service->second.epgTitle;
                    video.epgText = service->second.epgText;
                }
                if (video.name.empty()) video.name = "Servicio " + std::to_string(video.program);
                results.emplace(video.program, std::move(video));
            }
            q += 5 + infoLength;
        }
    }

    std::vector<VideoProgram> out;
    for (auto& item : results) out.push_back(std::move(item.second));
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // la GUI consume el log en vivo
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    const char* fwPath = "fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp";
    uint32_t freq = 473142857u;
    uint32_t bw = siano::BW_ISDBT_13SEG;
    int seconds = 8;
    const char* outPath = nullptr;
    bool probe = false;
    bool noFw = false;
    bool autoFw = false;
    bool runningFw = false;
    bool scan = false;
    bool statsMode = false;
    bool liveMode = false;
    bool tsStdout = false;
    uint32_t pid = 0x2000;  // 0x2000 = TS completo sin filtrar
    int chStart = 14, chEnd = 51, dwell = 2000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::printf("[-] falta valor para %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--fw") fwPath = next("--fw");
        else if (a == "--freq") freq = static_cast<uint32_t>(std::strtoul(next("--freq"), nullptr, 0));
        else if (a == "--bw") bw = static_cast<uint32_t>(std::strtoul(next("--bw"), nullptr, 0));
        else if (a == "--seconds") seconds = std::atoi(next("--seconds"));
        else if (a == "--out") outPath = next("--out");
        else if (a == "--probe") probe = true;
        else if (a == "--no-fw") noFw = true;
        else if (a == "--auto-fw") autoFw = true;
        else if (a == "--running") runningFw = true;
        else if (a == "--scan") scan = true;
        else if (a == "--stats") statsMode = true;
        else if (a == "--live") liveMode = true;
        else if (a == "--ts-stdout") tsStdout = true;
        else if (a == "--pid") pid = static_cast<uint32_t>(std::strtoul(next("--pid"), nullptr, 0));
        else if (a == "--ch-start") chStart = std::atoi(next("--ch-start"));
        else if (a == "--ch-end") chEnd = std::atoi(next("--ch-end"));
        else if (a == "--dwell") dwell = std::atoi(next("--dwell"));
        else {
            std::printf("uso: %s [--probe] [--no-fw] [--auto-fw] [--running] [--fw PATH] [--freq HZ] [--bw 4|8] "
                        "[--pid 0x2000] [--seconds N] [--live] [--ts-stdout] [--out FILE] [--stats] "
                        "[--scan] [--ch-start N] [--ch-end N] [--dwell MS]\n", argv[0]);
            return 2;
        }
    }

    // Para el reproductor GUI, stdout transporta solo MPEG-TS; todo el log
    // del host se redirige a stderr para que Swift lo pueda mostrar aparte.
    if (tsStdout) {
        std::signal(SIGPIPE, SIG_IGN);
        g_tsOutputFd = dup(STDOUT_FILENO);
        if (g_tsOutputFd < 0) {
            std::fprintf(stderr, "[-] no pude duplicar el canal de MPEG-TS\n");
            return 1;
        }
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            std::fprintf(stderr, "[-] no pude separar el canal de log del TS\n");
            return 1;
        }
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }

    Dev dev;
    if (!dev.findAndOpen(true)) return 1;

    auto word = [](const std::vector<uint8_t>& p, size_t o) -> uint32_t {
        if (o + 4 > p.size()) return 0;
        return static_cast<uint32_t>(p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24));
    };

    if (probe) {
        std::printf("[+] probe: leyendo bulk IN 500 ms (no deberia haber datos en frio)...\n");
        uint16_t t = 0;
        std::vector<uint8_t> p;
        if (dev.recv(t, p, 500))
            std::printf("[+] ya habia un mensaje: type=%u %s\n", t, msgName(t));
        else
            std::printf("[+] sin datos (esperado si el firmware no esta cargado)\n");
        return 0;
    }

    if (autoFw && !noFw) {
        // Detecta firmware Rio activo para que la app no fuerce una descarga
        // "cold" sobre un chip que ya está inicializado. En ROM el GET_VERSION
        // suele responder como 0x2230/0xff, por lo que se continuará abajo con
        // la descarga normal del .inp.
        dev.sendWait(siano::message(siano::MSG_INIT_DEVICE, 6u, siano::SRC_HOST),
                     579, 1000, false);
        if (dev.sendWait(siano::message(siano::MSG_GET_VERSION_EX, nullptr, 0, siano::SRC_HOST),
                         669, 1200, false) && dev.lastPayload.size() >= 16) {
            const auto& p = dev.lastPayload;
            const uint32_t chip = word(p, 0);
            const uint32_t fwVersion = word(p, 4);
            std::string name(reinterpret_cast<const char*>(p.data() + 14), p.size() - 14);
            const auto zero = name.find('\0');
            if (zero != std::string::npos) name.resize(zero);
            if (chip == 0x2270 && fwVersion >= 0x4000 && name.rfind("ISDBT ", 0) == 0) {
                noFw = true;
                std::printf("[+] firmware Rio activo: %s (chip=0x%x, fw=0x%x); omito descarga\n",
                            name.c_str(), chip, fwVersion);
            } else {
                std::printf("[+] firmware no inicializado para ISDB-T (chip=0x%x fw=0x%x '%s'); cargaré el .inp\n",
                            chip, fwVersion, name.c_str());
            }
        } else {
            std::printf("[+] no pude confirmar firmware activo; intentaré arranque frío\n");
        }
    }

    if (!noFw) {
        auto fw = slurp(fwPath);
        if (fw.size() < 12) {
            std::printf("[-] no pude leer firmware %s\n", fwPath);
            return 1;
        }
        auto seq = siano::buildFirmwareSequence(fw.data(), fw.size(), runningFw);
        std::printf("[+] firmware %s: %zu B, %zu mensajes%s\n", fwPath, fw.size(),
                    seq.size(), runningFw ? " (modo running 702/704)" : " (arranque frio)");
        long long t0 = nowMs();
        int sent = 0, failed = 0;
        for (const auto& m : seq) {
            uint16_t type = siano::peekType(m.data(), m.size());
            uint16_t expect = type == 660 ? 661 : type == 662 ? 663 : type == 664 ? 665
                            : type == 702 ? 703 : type == 704 ? 705 : 0;
            bool ok = expect ? dev.sendWait(m, expect, 400, false) : dev.send(m);
            ++sent;
            if (!ok) {
                ++failed;
                if (failed == 1) {
                    std::printf("  [!] primer bloque sin ACK; paso a envio sin espera\n");
                    dev.ackMode = false;
                }
            }
            if (sent % 50 == 0)
                std::printf("  ... %d/%zu (%lld ms)\n", sent, seq.size(), nowMs() - t0);
        }
        std::printf("[+] firmware enviado: %d mensajes, %d fallos, %lld ms\n",
                    sent, failed, nowMs() - t0);
        usleep(400000);  // espera de 400 ms del driver original
    }

    std::printf("[+] INIT_DEVICE(6 = ISDBT_BDA)\n");
    dev.sendWait(siano::message(siano::MSG_INIT_DEVICE, 6u, siano::SRC_HOST), 579, 1500);

    std::printf("[+] GET_VERSION_EX\n");
    if (dev.sendWait(siano::message(siano::MSG_GET_VERSION_EX, nullptr, 0, siano::SRC_HOST), 669, 1500)) {
        const auto& p = dev.lastPayload;
        if (p.size() >= 8) {
            uint32_t chip = static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
            uint32_t ver = static_cast<uint32_t>(p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24));
            std::printf("    chip_model=0x%x fw_version=0x%x (raw:", chip, ver);
            hexdump(p, p.size());
            std::printf(" )\n");
        }
    }

    std::printf("[+] RECEIVE_1SEG_THROUGH_FULLSEG = 1\n");
    dev.sendWait(siano::message(siano::MSG_RECEIVE_1SEG_THROUGH_FULLSEG, 1u, siano::SRC_HOST), 529, 800);

    uint64_t tsBytes = 0, ctrl = 0, other = 0;

    if (scan) {
        // En modo BDA el chip solo entrega TS si el demodulador engancha.
        // 805/806 son eventos de la interfaz USB, no de RF: el indicador fiable
        // es is_demod_locked (y is_rf_locked) dentro de GET_STATISTICS_EX_RES.
        std::printf("[+] escaneo canales %d..%d, dwell=%d ms, bw=%u\n", chStart, chEnd, dwell, bw);
        std::printf("[+] analizaré PAT/PMT y marcaré canales con pista de video\n");
        int hits = 0;
        for (int ch = chStart; ch <= chEnd && !g_stopRequested; ++ch) {
            uint32_t f = 473142857u + static_cast<uint32_t>(ch - 14) * 6000000u;
            dev.sendWait(siano::buildTune(f, bw), 777, 800, false);
            dev.send(siano::buildPidFilter(siano::MSG_ADD_PID_FILTER, pid));

            // Dejar sintonizar, drenar el endpoint y guardar una muestra para
            // comprobar PAT/PMT y el stream_type de video.
            uint64_t tsCh = 0;
            std::vector<uint8_t> sample;
            constexpr size_t MAX_SCAN_SAMPLE = 8 * 1024 * 1024;
            long long end = nowMs() + dwell;
            while (!g_stopRequested && nowMs() < end) {
                uint16_t t = 0;
                std::vector<uint8_t> p;
                if (!dev.recv(t, p, static_cast<int>(end - nowMs()))) continue;
                if (t == 693) {
                    tsCh += p.size();
                    const size_t take = std::min(p.size(), MAX_SCAN_SAMPLE - sample.size());
                    sample.insert(sample.end(), p.begin(), p.begin() + static_cast<long>(take));
                }
            }
            if (g_stopRequested) break;

            // Estadisticas reales del canal.
            bool got = false;
            uint32_t rf = 0, demod = 0, mode = 0, state = 0;
            int32_t snr = 0, rssi = 0, inband = 0;
            if (dev.sendWait(siano::message(siano::MSG_GET_STATISTICS_EX, nullptr, 0, siano::SRC_BDA),
                             654, 1200, false)) {
                const auto& p = dev.lastPayload;
                if (p.size() >= 0x38) {
                    got = true;
                    rf    = word(p, 0x0c);
                    demod = word(p, 0x10);
                    snr   = static_cast<int32_t>(word(p, 0x18));
                    rssi  = static_cast<int32_t>(word(p, 0x1c));
                    inband = static_cast<int32_t>(word(p, 0x20));
                    mode  = word(p, 0x30);
                    state = word(p, 0x34);
                }
            }
            tsBytes += tsCh;
            auto videos = findVideoPrograms(sample);
            const bool missingMetadata = std::any_of(videos.begin(), videos.end(), [](const VideoProgram& v) {
                return v.serviceType == 0 || v.name.rfind("Servicio ", 0) == 0 ||
                       (v.epgTitle.empty() && v.serviceType != 0xc0);
            });
            if (missingMetadata && !g_stopRequested) {
                // SDT no siempre aparece en la primera ventana de 2 s. Extiende
                // solo los canales con video pero sin nombre/tipo de servicio.
                const long long metadataEnd = nowMs() + 1500;
                while (!g_stopRequested && nowMs() < metadataEnd) {
                    uint16_t t = 0;
                    std::vector<uint8_t> p;
                    if (!dev.recv(t, p, static_cast<int>(metadataEnd - nowMs()))) continue;
                    if (t == 693) {
                        tsCh += p.size();
                        const size_t take = std::min(p.size(), MAX_SCAN_SAMPLE - sample.size());
                        sample.insert(sample.end(), p.begin(), p.begin() + static_cast<long>(take));
                    }
                }
                videos = findVideoPrograms(sample);
            }
            std::printf("[SCAN] ch=%d freq=%u rf=%u demod=%u snr=%d rssi=%d inband=%d "
                        "mode=%u state=%u ts_bytes=%llu video_count=%zu\n",
                        ch, f, got ? rf : 0, got ? demod : 0, snr, rssi, inband,
                        mode, state, static_cast<unsigned long long>(tsCh), videos.size());
            if (!videos.empty()) ++hits;
            for (const auto& video : videos) {
                const char* segmentType = video.serviceType == 0xc0 ? "1seg" : "13seg";
                std::printf("[SERVICE] ch=%d freq=%u program=%u service_type=0x%02x segment=%s "
                            "name=%s provider=%s video_pid=0x%04x epg_title=%s epg_text=%s\n",
                            ch, f, video.program, video.serviceType, segmentType,
                            percentEncode(video.name).c_str(), percentEncode(video.provider).c_str(),
                            video.pid, percentEncode(video.epgTitle).c_str(),
                            percentEncode(video.epgText).c_str());
            }
        }
        std::printf("[=] canales con video: %d%s\n", hits,
                    g_stopRequested ? " (escaneo cancelado)" : "");
        return hits || g_stopRequested ? 0 : 3;
    }

    std::printf("[+] ISDBT_TUNE freq=%u bw=%u\n", freq, bw);
    dev.sendWait(siano::buildTune(freq, bw), 777, 1500);

    // En modo BDA el chip no emite TS hasta recibir ADD_PID_FILTER_REQ.
    // 0x2000 pide el TS completo (todos los PIDs). El firmware probado no
    // devuelve 602 de forma fiable, asi que se envia sin esperar ACK.
    std::printf("[+] ADD_PID 0x%04x (0x2000 = TS completo)\n", pid);
    dev.send(siano::buildPidFilter(siano::MSG_ADD_PID_FILTER, pid));

    if (liveMode) {
        if (dev.sendWait(siano::message(siano::MSG_GET_STATISTICS_EX, nullptr, 0, siano::SRC_BDA),
                         654, 1500, false) && dev.lastPayload.size() >= 0x38) {
            const auto& p = dev.lastPayload;
            std::printf("[RF] rf_lock=%u demod_lock=%u snr=%d rssi=%d inband=%d\n",
                        word(p, 0x0c), word(p, 0x10),
                        static_cast<int32_t>(word(p, 0x18)),
                        static_cast<int32_t>(word(p, 0x1c)),
                        static_cast<int32_t>(word(p, 0x20)));
        } else {
            std::printf("[RF] estadisticas no disponibles\n");
        }
    }

    if (statsMode) {
        for (int k = 0; k < (seconds < 1 ? 1 : seconds); ++k) {
            dev.sendWait(siano::message(siano::MSG_GET_STATISTICS_EX, nullptr, 0, siano::SRC_BDA),
                         654, 1500, false);
            const auto& p = dev.lastPayload;
            std::printf("\n[stats %d] %zu bytes\n", k, p.size());
            for (size_t o = 0; o + 4 <= p.size(); o += 4)
                std::printf("   +0x%02zx = 0x%08x (%d)\n", o, word(p, o), static_cast<int32_t>(word(p, o)));
            if (p.size() >= 0x2c) {
                std::printf("   isdbt (+4): rf_lock=%u demod_lock=%u lna=%u SNR=%d dB RSSI=%d dBm "
                            "in_band=%d carrier_off=%d freq=%u bw=%u mode=%u state=%u\n",
                            word(p, 0x0c), word(p, 0x10), word(p, 0x14),
                            static_cast<int32_t>(word(p, 0x18)),
                            static_cast<int32_t>(word(p, 0x1c)),
                            static_cast<int32_t>(word(p, 0x20)),
                            static_cast<int32_t>(word(p, 0x24)),
                            word(p, 0x28), word(p, 0x2c), word(p, 0x30), word(p, 0x34));
            }
            usleep(1000000);
        }
        return 0;
    }

    std::printf("[+] RX %d s (ctrl + TS)...\n", seconds);
    std::ofstream out;
    if (outPath) {
        out.open(outPath, std::ios::binary);
        if (!out) {
            std::printf("[-] no pude abrir el archivo de salida: %s\n", outPath);
            return 1;
        }
    }

    uint16_t lastCtrl = 0;
    long long end = nowMs() + seconds * 1000LL;
    long long captureStart = nowMs();
    long long nextReport = nowMs() + 1000;
    long long nextStats = nowMs() + 2500;
    long long reportStart = nowMs();
    uint64_t reportBytes = 0;
    while (liveMode ? !g_stopRequested : nowMs() < end) {
        if (liveMode && nowMs() >= nextStats) {
            // Pide estadísticas sin bloquear ni consumir TS; la respuesta 654
            // se procesa dentro del mismo RX loop.
            dev.send(siano::message(siano::MSG_GET_STATISTICS_EX, nullptr, 0, siano::SRC_BDA));
            nextStats = nowMs() + 2500;
        }
        uint16_t type = 0;
        std::vector<uint8_t> p;
        if (dev.recv(type, p, 300)) {
            if (type == 693) {
                tsBytes += p.size();
                if (out) out.write(reinterpret_cast<const char*>(p.data()),
                                   static_cast<std::streamsize>(p.size()));
                if (tsStdout && !writeAll(g_tsOutputFd, p.data(), p.size())) {
                    std::fprintf(stderr, "[-] se cerró el consumidor de MPEG-TS\n");
                    tsStdout = false;
                }
            } else {
                ++ctrl;
                if (type == 654 && p.size() >= 0x38) {
                    std::printf("[RF] rf_lock=%u demod_lock=%u snr=%d rssi=%d inband=%d\n",
                                word(p, 0x0c), word(p, 0x10),
                                static_cast<int32_t>(word(p, 0x18)),
                                static_cast<int32_t>(word(p, 0x1c)),
                                static_cast<int32_t>(word(p, 0x20)));
                } else if (type != lastCtrl) {
                    std::printf("  <- %u %s (%zu B)\n", type, msgName(type), p.size());
                    lastCtrl = type;
                }
                if (type == 654 && p.size() >= 12) {
                    // struct sms_stats: in_band_power, snr, ber, ... (ver smscoreapi.h)
                    std::printf("     stats raw:");
                    hexdump(p, 24);
                    std::printf("\n");
                }
            }
            if (tsBytes && !looksLikeTs(p.data(), p.size())) ++other;
        }

        if (liveMode && nowMs() >= nextReport) {
            long long now = nowMs();
            double kbps = (tsBytes - reportBytes) * 8.0 / (now - reportStart);
            std::printf("[LIVE] bytes=%llu kbps=%.0f\n",
                        static_cast<unsigned long long>(tsBytes), kbps);
            reportBytes = tsBytes;
            reportStart = now;
            nextReport = now + 1000;
            if (out) out.flush();
        }
        if (g_stopRequested) {
            break;
        }
        if (!liveMode && nowMs() >= end) {
            break;
        }
    }
    if (out) {
        out.flush();
        out.close();
    }

    std::printf("\n[=] TS: %llu bytes (%.1f KB), ctrl=%llu, otros=%llu, split=%llu\n",
                static_cast<unsigned long long>(tsBytes), tsBytes / 1024.0,
                static_cast<unsigned long long>(ctrl),
                static_cast<unsigned long long>(other),
                g_splitMessages);
    const long long elapsed = nowMs() - captureStart;
    if (elapsed > 0)
        std::printf("[=] bitrate: %.1f kbps\n", tsBytes * 8.0 / elapsed);
    if (outPath && tsBytes) std::printf("[=] guardado en %s\n", outPath);
    return tsBytes ? 0 : 3;  // 3 = protocolo ok pero sin TS (antena?)
}
