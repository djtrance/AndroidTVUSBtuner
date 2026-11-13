package com.padtv.siano

/**
 * Puente JNI. Todo el protocolo vive en C++ (app/src/main/cpp).
 * El transporte USB vive en [UsbTransport].
 */
object Native {
    init {
        System.loadLibrary("siano")
    }

    external fun protocolVersion(): String

    /** Mensaje sin payload (p.ej. GET_VERSION_EX = 668). */
    external fun buildSimple(type: Int, src: Int): ByteArray

    /** Mensaje con una palabra de payload (p.ej. RECEIVE_1SEG_THROUGH_FULLSEG = 1). */
    external fun buildWithWord(type: Int, word: Int, src: Int): ByteArray

    /** INIT_DEVICE (578) con el modo de dispositivo. */
    external fun buildInit(mode: Int): ByteArray

    /** ISDBT_TUNE (776): frecuencia en Hz + ancho (4 = 1seg, 8 = 13seg). */
    external fun buildTune(freqHz: Int, bandwidth: Int): ByteArray

    /** ADD_PID_FILTER (601) / REMOVE_PID_FILTER (603). */
    external fun buildPidFilter(add: Boolean, pid: Int): ByteArray

    /** Secuencia 702/660/662/664/704 a partir del .inp completo. */
    external fun firmwareMessages(firmware: ByteArray, running: Boolean): Array<ByteArray>

    external fun peekType(msg: ByteArray): Int
    external fun peekWord(msg: ByteArray, offset: Int): Int

    /** Builders del puente USB/I2C DVBsky para T230/C688. */
    external fun buildT230I2cWrite(address: Int, data: ByteArray): ByteArray
    external fun buildT230I2cRead(address: Int, length: Int): ByteArray
    external fun extractT230I2cData(reply: ByteArray, payloadLength: Int): ByteArray?
}
