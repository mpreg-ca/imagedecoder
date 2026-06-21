package ca.mpreg.imagedecoder

import java.io.InputStream
import java.nio.ByteBuffer

class ImageDecoder private constructor(
    private val ptr: Long,
    val count: Int,
    var page: Int,
    val isHdr: Boolean,
) {
    class DecodeException private constructor(message: String) : Exception(message)

    class DecodeResult private constructor(
        private val ptr: Long,
        val image: ByteBuffer,
        val width: Int,
        val height: Int,
        val duration: Int,
        val trim_left: Int,
        val trim_top: Int,
        val trim_width: Int,
        val trim_height: Int,
    ) {
        protected fun finalize() {
            free()
        }

        private external fun free()
    }

    protected fun finalize() {
        free()
    }

    @Synchronized
    @Throws(DecodeException::class)
    fun decodeNext(crop: Boolean = false, getTrim: Boolean = false): DecodeResult {
        val res = decode(page, crop, getTrim)
        page = (page + 1) % count
        return res
    }

    @Synchronized
    @Throws(DecodeException::class)
    external fun decode(
        page: Int = 0,
        crop: Boolean = false,
        getTrim: Boolean = false
    ): DecodeResult

    private external fun free()

    companion object {
        init {
            System.loadLibrary("imagedecoder")
        }

        @JvmStatic
        @Throws(DecodeException::class)
        external fun new(inputStream: InputStream): ImageDecoder
    }
}
