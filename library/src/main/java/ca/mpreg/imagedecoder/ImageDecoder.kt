package ca.mpreg.imagedecoder

import java.io.Closeable
import java.io.InputStream
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.log2
import kotlin.math.pow

/**
 * Decodes one image, held in native memory for the life of the object.
 *
 * The collector has no idea how large that buffer is, so leaving it to the finalizer is what
 * turns a decode loop into an OOM. [close] is idempotent and safe to call from any thread.
 */
class ImageDecoder private constructor(
    // Read and cleared by native code; see nativeFree.
    private var ptr: Long,
    pages: Int,
    page: Int,
    val isHdr: Boolean,
    private val hdrKindRaw: Int,
    val hdrHeadroom: Float,
    private val loader: String,
) : Closeable {
    /** At least 1: zero would make [decodeNext] divide by zero. */
    val pages: Int = if (pages > 0) pages else 1

    /** Next page [decodeNext] will hand back, always in `0 until pages`. */
    var page: Int = page.coerceIn(0, this.pages - 1)
        set(value) {
            require(value in 0 until pages) { "page $value out of range 0 until $pages" }
            field = value
        }

    private val closed = AtomicBoolean(false)

    /**
     * Normalized image format derived from the libvips loader name.
     *
     * Enabled formats: jpeg, png, webp, gif, tiff, heif (includes avif/heic), jxl, jp2k.
     */
    val format: String
        get() = when {
            loader.startsWith("jpeg") -> "jpeg"
            loader.startsWith("uhdr") -> "jpeg"
            loader.startsWith("png") -> "png"
            loader.startsWith("webp") -> "webp"
            loader.startsWith("gif") -> "gif"
            loader.startsWith("tiff") -> "tiff"
            loader.startsWith("heif") -> "heif"
            loader.startsWith("jxl") -> "jxl"
            loader.startsWith("jp2k") -> "jp2"
            else -> loader.removeSuffix("load_buffer").removeSuffix("load")
        }

    enum class HdrKind {
        NONE,

        /** [DecodeResult.image] is the SDR base and [DecodeResult.gainmap] the unapplied map. */
        GAINMAP,
        PQ,
        HLG,

        /** Already linear light - libvips hands a few float formats over that way. */
        LINEAR,
    }

    val hdrKind: HdrKind
        get() = HdrKind.entries.getOrElse(hdrKindRaw) { HdrKind.NONE }

    enum class PixelFormat(val bytesPerPixel: Int) {
        RGBA8(4),

        /**
         * Extended sRGB: sRGB primaries and transfer, 1.0 at diffuse white, highlights above,
         * negatives out of gamut, alpha linear. An SDR pixel holds the same value it would in
         * [RGBA8].
         */
        RGBA16F(8),
    }

    /**
     * The multiplier that turns an SDR base image into an HDR one, left for the caller to apply
     * ([applyTo]) because how much of it to use depends on the display.
     *
     * Every metadata array holds three entries even when [channels] is 1.
     */
    class Gainmap private constructor(
        /**
         * Tightly packed 8-bit at [width] x [height], usually smaller than the base image.
         *
         * Raw values: divide by 255 for the gain input, with no transfer function to undo. The
         * map looks like an image and invites being treated as one; that is the classic bug.
         */
        val pixels: ByteBuffer,
        val width: Int,
        val height: Int,
        val channels: Int,
        val gamma: FloatArray,
        val minContentBoost: FloatArray,
        val maxContentBoost: FloatArray,
        val offsetSdr: FloatArray,
        val offsetHdr: FloatArray,
    ) {
        init {
            // A short array would index out of bounds in gainFor, far from whatever produced it.
            require(
                gamma.size >= 3 && minContentBoost.size >= 3 && maxContentBoost.size >= 3 &&
                        offsetSdr.size >= 3 && offsetHdr.size >= 3
            ) { "gainmap metadata arrays must hold three entries" }
            require(width > 0 && height > 0 && channels > 0) { "empty gainmap" }
        }

        val headroomStops: Float
            get() = maxContentBoost.maxOrNull()?.takeIf { it.isFinite() && it > 1f }
                ?.let { log2(it) } ?: 0f

        /**
         * Reference arithmetic, matching libultrahdr's `applyGain` - for a caller applying the
         * map on the CPU or in a shader to check its own version against.
         */
        fun gainFor(value: Int, channel: Int): Float {
            val i = channel.coerceIn(0, 2)
            var g = (value and 0xFF) / 255f

            // Zero divides by zero, and a negative lands on NaN.
            val gammaI = gamma[i]
            if (gammaI != 1f && gammaI > 0f && gammaI.isFinite()) g = g.pow(1f / gammaI)

            // log2 of a non-positive boost is -Inf or NaN, and this multiplies every highlight.
            val minBoost = minContentBoost[i].takeIf { it.isFinite() && it > 0f } ?: 1f
            val maxBoost = maxContentBoost[i].takeIf { it.isFinite() && it > 0f } ?: 1f

            val logBoost = log2(minBoost) * (1f - g) + log2(maxBoost) * g
            return 2f.pow(logBoost)
        }

        /** [linear] is linear light, not an sRGB-encoded sample. */
        fun applyTo(linear: Float, value: Int, channel: Int): Float {
            val i = channel.coerceIn(0, 2)
            return (linear + offsetSdr[i]) * gainFor(value, i) - offsetHdr[i]
        }
    }

    open class DecodeException internal constructor(message: String) : Exception(message)

    class UnknownFormatException internal constructor(message: String) : DecodeException(message)

    /**
     * The image did not fit in memory, or is larger than this decoder will allocate for one
     * picture. Thrown instead of [OutOfMemoryError] so a caller decoding untrusted input can skip
     * the file rather than unwind its whole thread.
     */
    class OutOfMemoryException internal constructor(message: String) : DecodeException(message)

    class DecodeResult private constructor(
        val image: ByteBuffer,
        val width: Int,
        val height: Int,
        val duration: Int,
        val trim_left: Int,
        val trim_top: Int,
        val trim_width: Int,
        val trim_height: Int,
        private val pixelFormatRaw: Int,
        /** Stops of headroom above SDR white the source can reach; 0 for SDR. */
        val hdrHeadroom: Float,
        /**
         * Non-null only for [HdrKind.GAINMAP], and null even then if the metadata says the map
         * cannot brighten anything.
         */
        val gainmap: Gainmap?,
    ) {
        val pixelFormat: PixelFormat
            get() = PixelFormat.entries.getOrElse(pixelFormatRaw) { PixelFormat.RGBA8 }

        /**
         * False for a gainmap image even though it is an HDR one: there, [image] is the SDR base
         * and [gainmap] is what makes it HDR.
         */
        val isHdr: Boolean get() = pixelFormat == PixelFormat.RGBA16F

        val bytesPerRow: Int get() = width * pixelFormat.bytesPerPixel
    }

    @Synchronized
    @Throws(DecodeException::class)
    fun decodeNext(crop: Boolean = false, getTrim: Boolean = false): DecodeResult {
        val res = decode(page, crop, getTrim)
        page = (page + 1) % pages
        return res
    }

    /**
     * @throws OutOfMemoryException if the decoded image does not fit in memory.
     * @throws DecodeException if the file is malformed, or this decoder is closed.
     */
    @Synchronized
    @Throws(DecodeException::class)
    fun decode(page: Int = 0, crop: Boolean = false, getTrim: Boolean = false): DecodeResult {
        checkOpen()
        if (page < 0 || page >= pages) {
            throw DecodeException("page $page out of range 0 until $pages")
        }
        return nativeDecode(page, crop, getTrim)
    }

    private external fun nativeDecode(page: Int, crop: Boolean, getTrim: Boolean): DecodeResult

    /** Exif and tiff tag names of the main image, in file order; no value is parsed. */
    @Synchronized
    @Throws(DecodeException::class)
    fun listTags(): List<String> {
        checkOpen()
        return nativeListTags().asList()
    }

    /** Null if the image carries no [name]; several components read as "8, 8, 8". */
    @Synchronized
    @Throws(DecodeException::class)
    fun getTag(name: String): String? {
        checkOpen()
        return nativeGetTag(name)
    }

    private external fun nativeListTags(): Array<String>

    private external fun nativeGetTag(name: String): String?

    /** Bytes of a re-encoded image, in native memory. [close] frees them. */
    class EncodeResult private constructor(
        private var ptr: Long,
        private val buffer: ByteBuffer,
    ) : Closeable {
        private val closed = AtomicBoolean(false)

        /** Valid until [close]; copy anything that has to outlive this object. */
        val bytes: ByteBuffer
            get() {
                check(!closed.get()) { "EncodeResult has been closed" }
                return buffer
            }

        override fun close() {
            if (closed.compareAndSet(false, true)) nativeFree()
        }

        @Deprecated("Call close(); the finalizer only catches a caller who forgot.")
        protected fun finalize() {
            close()
        }

        private external fun nativeFree()
    }

    /**
     * @throws OutOfMemoryException if the encoded image does not fit in memory.
     * @throws DecodeException if encoding fails, or this decoder is closed.
     */
    @Synchronized
    @Throws(DecodeException::class)
    fun encode(suffix: String, page: Int = -1): EncodeResult {
        checkOpen()
        if (page < -1 || page >= pages) {
            throw DecodeException("page $page out of range -1 until $pages")
        }
        return nativeEncode(suffix, page)
    }

    private external fun nativeEncode(suffix: String, page: Int): EncodeResult

    /**
     * Releases the native image buffer. Idempotent; a later [decode] or [encode] throws
     * [DecodeException] rather than reading freed memory.
     */
    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        // The monitor, not just the flag: a decode in flight is still reading the buffer.
        synchronized(this) { nativeFree() }
    }

    private fun checkOpen() {
        if (closed.get() || ptr == 0L) throw DecodeException("ImageDecoder has been closed")
    }

    @Deprecated("Call close(); the finalizer only catches a caller who forgot.")
    protected fun finalize() {
        close()
    }

    private external fun nativeFree()

    companion object {
        init {
            System.loadLibrary("imagedecoder2")
        }

        /**
         * Reads [inputStream] to the end and parses its header, leaving it open for the caller
         * to close.
         *
         * @throws OutOfMemoryException if the image is larger than the decoder will buffer.
         * @throws UnknownFormatException if the bytes are not a supported image.
         */
        @JvmStatic
        @Throws(DecodeException::class)
        fun new(inputStream: InputStream): ImageDecoder = nativeNew(inputStream)

        @JvmStatic
        private external fun nativeNew(inputStream: InputStream): ImageDecoder
    }
}
