// jni_bridge.cpp - puente entre Kotlin y el protocolo Siano.
//
// Todo el I/O USB vive en Kotlin (UsbDeviceConnection); aqui solo se
// construyen y parsean mensajes. Asi el transporte es portable y el codigo
// nativo se puede probar en el Mac sin dongle.

#include <jni.h>

#include <cstdint>
#include <vector>

#include "siano_proto.h"
#include "t230_proto.h"

namespace {

jbyteArray toByteArray(JNIEnv* env, const std::vector<uint8_t>& v) {
    jbyteArray out = env->NewByteArray(static_cast<jsize>(v.size()));
    if (out && !v.empty()) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(v.size()),
                                reinterpret_cast<const jbyte*>(v.data()));
    }
    return out;
}

std::vector<uint8_t> fromByteArray(JNIEnv* env, jbyteArray arr) {
    std::vector<uint8_t> out;
    if (!arr) return out;
    const jsize n = env->GetArrayLength(arr);
    out.resize(static_cast<size_t>(n));
    if (n > 0) {
        env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte*>(out.data()));
    }
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_com_padtv_siano_Native_protocolVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF("siano-pt230/0.1");
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildSimple(JNIEnv* env, jclass,
                                                                    jint type, jint src) {
    return toByteArray(env, siano::message(static_cast<siano::MsgType>(type), nullptr, 0,
                                           static_cast<uint8_t>(src)));
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildWithWord(JNIEnv* env, jclass,
                                                                      jint type, jint word,
                                                                      jint src) {
    return toByteArray(env, siano::message(static_cast<siano::MsgType>(type),
                                           static_cast<uint32_t>(word),
                                           static_cast<uint8_t>(src)));
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildInit(JNIEnv* env, jclass,
                                                                  jint mode) {
    return toByteArray(env, siano::message(siano::MSG_INIT_DEVICE,
                                           static_cast<uint32_t>(mode), siano::SRC_HOST));
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildTune(JNIEnv* env, jclass,
                                                                  jint freq_hz, jint bandwidth) {
    return toByteArray(env, siano::buildTune(static_cast<uint32_t>(freq_hz),
                                             static_cast<uint32_t>(bandwidth)));
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildPidFilter(JNIEnv* env, jclass,
                                                                       jboolean add, jint pid) {
    return toByteArray(env, siano::buildPidFilter(
                               add ? siano::MSG_ADD_PID_FILTER : siano::MSG_REMOVE_PID_FILTER,
                               static_cast<uint32_t>(pid)));
}

JNIEXPORT jobjectArray JNICALL Java_com_padtv_siano_Native_firmwareMessages(
    JNIEnv* env, jclass, jbyteArray firmware, jboolean running) {
    const std::vector<uint8_t> fw = fromByteArray(env, firmware);
    const auto msgs = siano::buildFirmwareSequence(fw.data(), fw.size(), running == JNI_TRUE);

    jclass byteArrayClass = env->FindClass("[B");
    if (!byteArrayClass) return nullptr;
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(msgs.size()), byteArrayClass, nullptr);
    if (!out) return nullptr;
    for (jsize i = 0; i < static_cast<jsize>(msgs.size()); ++i) {
        jbyteArray item = toByteArray(env, msgs[i]);
        env->SetObjectArrayElement(out, i, item);
        env->DeleteLocalRef(item);
    }
    return out;
}

JNIEXPORT jint JNICALL Java_com_padtv_siano_Native_peekType(JNIEnv* env, jclass, jbyteArray msg) {
    const std::vector<uint8_t> buf = fromByteArray(env, msg);
    return static_cast<jint>(siano::peekType(buf.data(), buf.size()));
}

JNIEXPORT jint JNICALL Java_com_padtv_siano_Native_peekWord(JNIEnv* env, jclass, jbyteArray msg,
                                                           jint offset) {
    const std::vector<uint8_t> buf = fromByteArray(env, msg);
    return static_cast<jint>(siano::peekWord(buf.data(), buf.size(),
                                             static_cast<size_t>(offset)));
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildT230I2cWrite(
    JNIEnv* env, jclass, jint address, jbyteArray data) {
    const std::vector<uint8_t> payload = fromByteArray(env, data);
    const auto transfer = t230::buildI2cWrite(static_cast<uint8_t>(address),
                                               payload.data(), payload.size());
    return toByteArray(env, transfer.out);
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_buildT230I2cRead(
    JNIEnv* env, jclass, jint address, jint length) {
    if (length < 0) return env->NewByteArray(0);
    const auto transfer = t230::buildI2cRead(static_cast<uint8_t>(address),
                                              static_cast<size_t>(length));
    return toByteArray(env, transfer.out);
}

JNIEXPORT jbyteArray JNICALL Java_com_padtv_siano_Native_extractT230I2cData(
    JNIEnv* env, jclass, jbyteArray reply, jint payloadLength) {
    if (payloadLength < 0) return nullptr;
    const std::vector<uint8_t> raw = fromByteArray(env, reply);
    std::vector<uint8_t> payload;
    if (!t230::extractI2cReadData(raw.data(), raw.size(),
                                  static_cast<size_t>(payloadLength), payload))
        return nullptr;
    return toByteArray(env, payload);
}

}  // extern "C"
