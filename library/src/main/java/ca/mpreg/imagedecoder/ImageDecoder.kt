package ca.mpreg.imagedecoder

import java.io.InputStream
import java.nio.ByteBuffer

class ImageDecoder private constructor(
    private val nativePtr: Long,
    val count: Int,
    val isHdr: Boolean,
) {
    class DecodeException private constructor(message: String) : Exception(message)

    class DecodeResult private constructor(
        private val ptr: Long,
        val image: ByteBuffer,
        val width: Int,
        val height: Int,
        val duration: Int,
        val page: Int,
    ) {
        protected fun finalize() {
            free(ptr)
        }

        private external fun free(ptr: Long)
    }

    @Synchronized
    @Throws(DecodeException::class)
    fun decodeNext(crop: Boolean = false): DecodeResult? {
        return decodeNext(nativePtr, crop)
    }

    protected fun finalize() {
        free(nativePtr)
    }

    private external fun decodeNext(nativePtr: Long, crop: Boolean): DecodeResult?

    private external fun free(nativePtr: Long)

    companion object {
        init {
            System.loadLibrary("imagedecoder")
        }

        @JvmStatic
        @Throws(DecodeException::class)
        external fun newInstance(inputStream: InputStream): ImageDecoder?
    }
}
