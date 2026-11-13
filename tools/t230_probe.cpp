// Non-streaming identity probe for MyGica/T230 (0572:c688) via the DVBsky USB/I2C bridge.
// It sends only the normal Si2168 init/power-up/GET_REV commands; it does not
// load firmware, tune a channel, or start TS streaming.

#include <libusb.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kVid = 0x0572;
constexpr uint16_t kPid = 0xc688;
constexpr uint8_t kEpOut = 0x01;
constexpr uint8_t kEpIn = 0x81;
constexpr uint8_t kDemodAddr = 0x64;
constexpr uint8_t kTunerAddr = 0x60;

struct Device {
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    ~Device() {
        if (handle) {
            libusb_release_interface(handle, 0);
            libusb_close(handle);
        }
        if (ctx) libusb_exit(ctx);
    }
};

} // namespace

int main() {
    Device dev;
    int rc = libusb_init(&dev.ctx);
    if (rc < 0) {
        std::fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        return 1;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(dev.ctx, &list);
    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
            desc.idVendor == kVid && desc.idProduct == kPid) {
            found = list[i];
            break;
        }
    }
    if (!found) {
        std::fprintf(stderr, "No encontré %04x:%04x (visibles: %zd)\n", kVid, kPid, count);
        libusb_free_device_list(list, 1);
        return 2;
    }

    libusb_device_descriptor desc{};
    libusb_get_device_descriptor(found, &desc);
    std::printf("USB %04x:%04x bcdDevice=%04x bcdUSB=%04x\n",
                desc.idVendor, desc.idProduct, desc.bcdDevice, desc.bcdUSB);
    for (uint8_t ci = 0; ci < desc.bNumConfigurations; ++ci) {
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_config_descriptor(found, ci, &config) < 0) continue;
        for (uint8_t ii = 0; ii < config->bNumInterfaces; ++ii) {
            const libusb_interface& intf = config->interface[ii];
            for (int ai = 0; ai < intf.num_altsetting; ++ai) {
                const libusb_interface_descriptor& alt = intf.altsetting[ai];
                std::printf("iface=%u alt=%u class=%02x sub=%02x proto=%02x endpoints=",
                            alt.bInterfaceNumber, alt.bAlternateSetting,
                            alt.bInterfaceClass, alt.bInterfaceSubClass, alt.bInterfaceProtocol);
                for (uint8_t ei = 0; ei < alt.bNumEndpoints; ++ei) {
                    const libusb_endpoint_descriptor& ep = alt.endpoint[ei];
                    std::printf(" 0x%02x(type=%u,maxpkt=%u)", ep.bEndpointAddress,
                                ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK, ep.wMaxPacketSize);
                }
                std::printf("\n");
            }
        }
        libusb_free_config_descriptor(config);
    }
    libusb_ref_device(found);
    rc = libusb_open(found, &dev.handle);
    libusb_free_device_list(list, 1);
    if (rc < 0) {
        std::fprintf(stderr, "libusb_open: %s\n", libusb_error_name(rc));
        return 3;
    }
    rc = libusb_claim_interface(dev.handle, 0);
    if (rc < 0) {
        std::fprintf(stderr, "claim interfaz 0: %s\n", libusb_error_name(rc));
        return 4;
    }
    std::printf("interface 0 claimed; querying Si2168 I2C via bulk 0x01/0x81\n");

    auto bulk = [&](uint8_t ep, uint8_t* bytes, int length) -> int {
        int actual = 0;
        const int result = libusb_bulk_transfer(dev.handle, ep, bytes, length, &actual, 1500);
        if (result < 0) {
            std::fprintf(stderr, "bulk 0x%02x: %s (%d/%d)\n", ep,
                         libusb_error_name(result), actual, length);
            return result;
        }
        if (actual != length) {
            std::fprintf(stderr, "bulk 0x%02x longitud inesperada %d/%d\n", ep, actual, length);
            return LIBUSB_ERROR_IO;
        }
        return 0;
    };

    auto genericRw = [&](const std::vector<uint8_t>& request, size_t replySize,
                         std::vector<uint8_t>& reply) -> int {
        std::vector<uint8_t> out = request;
        int result = bulk(kEpOut, out.data(), static_cast<int>(out.size()));
        if (result < 0) return result;
        reply.assign(replySize, 0);
        if (replySize == 0) return 0;
        return bulk(kEpIn, reply.data(), static_cast<int>(reply.size()));
    };

    auto i2cWrite = [&](uint8_t address, const uint8_t* bytes, size_t length) -> int {
        if (length > 60) return LIBUSB_ERROR_INVALID_PARAM;
        std::vector<uint8_t> req{0x08, address, static_cast<uint8_t>(length)};
        req.insert(req.end(), bytes, bytes + length);
        std::vector<uint8_t> reply;
        return genericRw(req, 1, reply);
    };

    auto i2cRead = [&](uint8_t address, size_t length, std::vector<uint8_t>& bytes) -> int {
        if (length > 60) return LIBUSB_ERROR_INVALID_PARAM;
        std::vector<uint8_t> req{0x09, 0, static_cast<uint8_t>(length), address};
        std::vector<uint8_t> reply;
        const int result = genericRw(req, length + 1, reply);
        if (result < 0) return result;
        bytes.assign(reply.begin() + 1, reply.end());
        return 0;
    };

    auto siCommand = [&](uint8_t address, const uint8_t* command, size_t writeLength,
                         size_t readLength, std::vector<uint8_t>& reply) -> int {
        int result = i2cWrite(address, command, writeLength);
        if (result < 0 || readLength == 0) return result;
        for (int attempt = 0; attempt < 50; ++attempt) {
            result = i2cRead(address, readLength, reply);
            if (result < 0) return result;
            if (reply.empty()) return LIBUSB_ERROR_IO;
            if (reply[0] & 0x80) {
                if (reply[0] & 0x40) return LIBUSB_ERROR_OTHER;
                return 0;
            }
            usleep(3000);
        }
        return LIBUSB_ERROR_TIMEOUT;
    };

    // Linux si2168_probe() sequence: initialize, power up, then GET_REV.
    const std::array<uint8_t, 13> init = {
        0xc0, 0x12, 0x00, 0x0c, 0x00, 0x0d, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    const std::array<uint8_t, 8> power = {0xc0, 0x06, 0x01, 0x0f, 0x00, 0x20, 0x20, 0x01};
    const uint8_t getRev = 0x02;
    std::vector<uint8_t> reply;

    rc = siCommand(kDemodAddr, init.data(), init.size(), 0, reply);
    if (rc < 0) {
        std::fprintf(stderr, "Si2168 INIT no respondió: %s\n", libusb_error_name(rc));
        return 5;
    }
    rc = siCommand(kDemodAddr, power.data(), power.size(), 1, reply);
    if (rc < 0) {
        std::fprintf(stderr, "Si2168 POWER_UP no respondió: %s\n", libusb_error_name(rc));
        return 6;
    }
    rc = siCommand(kDemodAddr, &getRev, 1, 13, reply);
    if (rc < 0) {
        std::fprintf(stderr, "Si2168 GET_REV no respondió: %s\n", libusb_error_name(rc));
        return 7;
    }

    if (reply.size() < 13) {
        std::fprintf(stderr, "GET_REV devolvió solo %zu bytes\n", reply.size());
        return 8;
    }
    std::printf("Si2168 GET_REV raw:");
    for (uint8_t b : reply) std::printf(" %02x", b);
    std::printf("\n");
    std::printf("demod silicon: Si2168-%c%c%c (raw chip id %02x%02x%02x%02x)\n",
                reply[1], reply[3], reply[4],
                reply[1], reply[2], reply[3], reply[4]);

    // Try the board's tuner behind the Si2168 I2C gate. This is only a
    // GET_REV request; skip it cleanly if the gate/tuner is not awake.
    const std::array<uint8_t, 3> openGate = {0xc0, 0x0d, 0x01};
    rc = siCommand(kDemodAddr, openGate.data(), openGate.size(), 0, reply);
    if (rc == 0) {
        // Common Si2148/Si2158 crystal power-up from the Linux si2157 driver.
        const std::array<uint8_t, 15> tunerPower = {
            0xc0, 0x00, 0x0c, 0x00, 0x00, 0x01, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x02, 0x00, 0x00, 0x01
        };
        const int tunerPowerResult = siCommand(kTunerAddr, tunerPower.data(), tunerPower.size(), 1, reply);
        const uint8_t tunerGetRev = 0x02;
        rc = tunerPowerResult < 0 ? tunerPowerResult : siCommand(kTunerAddr, &tunerGetRev, 1, 13, reply);
        if (rc == 0 && reply.size() >= 13 && (reply[0] & 0x80)) {
            std::printf("tuner Si21%u-%c%c%c ROM=0x%02x raw:",
                        reply[2], reply[1], reply[3], reply[4], reply[12]);
            for (uint8_t b : reply) std::printf(" %02x", b);
            std::printf("\n");
        } else {
            std::printf("tuner GET_REV no respondió (%s); demod validado\n",
                        libusb_error_name(rc));
        }
        const std::array<uint8_t, 3> closeGate = {0xc0, 0x0d, 0x00};
        (void)siCommand(kDemodAddr, closeGate.data(), closeGate.size(), 0, reply);
    }

    std::printf("Nota: solo se consultaron las IDs I2C; no se descargó firmware ni se sintonizó.\n");
    return 0;
}
