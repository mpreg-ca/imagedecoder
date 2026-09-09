package ca.mpreg.imagedecoder

import java.io.InputStream
import java.nio.ByteBuffer
import kotlin.math.log2
import kotlin.math.pow

class ImageDecoder private constructor(
    private val ptr: Long,
    val pages: Int,
    var page: Int,
    val isHdr: Boolean,
    private val hdrKindRaw: Int,
    val hdrHeadroom: Float,
    private val loader: String,
) {
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
        val headroomStops: Float
            get() = maxContentBoost.maxOrNull()?.takeIf { it > 1f }?.let { kotlin.math.log2(it) }
                ?: 0f

        /**
         * Reference arithmetic, matching libultrahdr's `applyGain` - for a caller applying the
         * map on the CPU or in a shader to check its own version against.
         */
        fun gainFor(value: Int, channel: Int): Float {
            val i = channel.coerceIn(0, 2)
            var g = (value and 0xFF) / 255f
            if (gamma[i] != 1f) g = g.pow(1f / gamma[i])
            val logBoost = log2(minContentBoost[i]) * (1f - g) + log2(maxContentBoost[i]) * g
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

    @Synchronized
    @Throws(DecodeException::class)
    external fun decode(
        page: Int = 0, crop: Boolean = false, getTrim: Boolean = false
    ): DecodeResult

    class EncodeResult private constructor(
        private val ptr: Long,
        val bytes: ByteBuffer,
    ) {
        protected fun finalize() {
            free()
        }

        private external fun free()
    }

    @Synchronized
    @Throws(DecodeException::class)
    external fun encode(suffix: String, page: Int = -1): EncodeResult

    protected fun finalize() {
        synchronized(this) {
            free()
        }
    }

    private external fun free()

    companion object {
        init {
            System.loadLibrary("imagedecoder2")
        }

        @JvmStatic
        @Throws(DecodeException::class)
        external fun new(inputStream: InputStream): ImageDecoder
    }
}
