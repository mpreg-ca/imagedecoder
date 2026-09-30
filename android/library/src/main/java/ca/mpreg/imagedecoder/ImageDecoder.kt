package ca.mpreg.imagedecoder

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.log2
import kotlin.math.pow

/**
 * Decodes one image; construction parses the header only.
 *
 * Frame pixels and decoder memory are native, invisible to the collector: [close] both.
 * [close] is idempotent and thread-safe.
 */
class ImageDecoder private constructor(
    // Cleared by native code; see nativeFree.
    private var ptr: Long,
    /** HDR pixels (PQ, HLG or LINEAR), decoded to half-float; false for [HdrKind.GAINMAP]. */
    val isHdr: Boolean,
    private val hdrKindRaw: Int,
    /** Stops above SDR white when [isHdr]; for [HdrKind.GAINMAP], see [Gainmap.headroomStops]. */
    val hdrHeadroom: Float,
    private val formatRaw: String,
    val width: Int,
    val height: Int,
) : Closeable {
    val format: Format = Format.fromNative(formatRaw)

    val pages: Int
        @Throws(DecodeException::class)
        @Synchronized get() {
            checkOpen()
            return nativeFrameCount().coerceAtLeast(1)
        }

    private external fun nativeFrameCount(): Int

    private val closed = AtomicBoolean(false)

    enum class Format {
        PNG,
        JPEG,
        TIFF,
        WEBP,
        GIF,
        AVIF,
        HEIF,
        JXL,
        JP2,
        UNKNOWN,
        ;

        companion object {
            internal fun fromNative(name: String): Format =
                entries.firstOrNull { it.name.equals(name, ignoreCase = true) } ?: UNKNOWN
        }
    }

    enum class HdrKind {
        NONE,

        /** SDR base in [Frame.image]; the map from [getGainmap]. */
        GAINMAP,
        PQ,
        HLG,
        LINEAR,
    }

    val hdrKind: HdrKind
        get() = HdrKind.entries.getOrElse(hdrKindRaw) { HdrKind.NONE }

    /** Per-pixel multiplier from the SDR base to HDR. */
    class Gainmap private constructor(
        private val pixelBuffer: ByteBuffer,
        val width: Int,
        val height: Int,
        val channels: Int,
        val gamma: FloatArray,
        val minContentBoost: FloatArray,
        val maxContentBoost: FloatArray,
        val offsetSdr: FloatArray,
        val offsetHdr: FloatArray,
        /** Stops of display headroom at or below which none of the map applies. */
        val baseHdrHeadroom: Float,
        /** Stops of display headroom at or above which all of it applies. */
        val alternateHdrHeadroom: Float,
    ) {
        init {
            require(
                gamma.size >= 3 && minContentBoost.size >= 3 && maxContentBoost.size >= 3 &&
                        offsetSdr.size >= 3 && offsetHdr.size >= 3
            ) { "gainmap metadata arrays must hold three entries" }
            require(width > 0 && height > 0 && channels > 0) { "empty gainmap" }
        }

        /** Gain inputs (divide by 255), oriented like [Frame.image]. */
        val pixels: ByteBuffer
            get() = pixelBuffer.duplicate().order(ByteOrder.nativeOrder())

        fun toByteArray(): ByteArray {
            val src = pixels
            return ByteArray(src.remaining()).also { src.get(it) }
        }

        /** Peak gain in stops: how far above SDR white the image can reach. */
        val headroomStops: Float
            get() = maxContentBoost.maxOrNull()?.takeIf { it.isFinite() && it > 1f }
                ?.let { log2(it) } ?: 0f

        /** Matches libultrahdr's `applyGain`. */
        fun gainFor(value: Int, channel: Int): Float {
            val i = channel.coerceIn(0, 2)
            var g = (value and 0xFF) / 255f

            val gammaI = gamma[i]
            if (gammaI != 1f && gammaI > 0f && gammaI.isFinite()) g = g.pow(1f / gammaI)

            val minBoost = minContentBoost[i].takeIf { it.isFinite() && it > 0f } ?: 1f
            val maxBoost = maxContentBoost[i].takeIf { it.isFinite() && it > 0f } ?: 1f

            val logBoost = log2(minBoost) * (1f - g) + log2(maxBoost) * g
            return 2f.pow(logBoost)
        }

        fun applyTo(linear: Float, value: Int, channel: Int): Float {
            val i = channel.coerceIn(0, 2)
            return (linear + offsetSdr[i]) * gainFor(value, i) - offsetHdr[i]
        }
    }

    open class DecodeException internal constructor(message: String) : Exception(message)

    class UnknownFormatException internal constructor(message: String) : DecodeException(message)

    /** Thrown instead of [OutOfMemoryError], so bad input can simply be skipped. */
    class OutOfMemoryException internal constructor(message: String) : DecodeException(message)

    /**
     * More bytes are needed before anything can be shown: [pushData] them and call
     * [decodeNext] again. Only a streaming decoder throws it, and the decoder stays usable.
     */
    class NeedMoreDataException internal constructor(message: String) : DecodeException(message)

    /**
     * One decoded frame. A whole frame owns native pixels, outliving the decoder until [close].
     * A [partial] one views the decoder's, valid until the next [decodeNext] or [rewind], which
     * close it.
     */
    class Frame private constructor(
        buffer: ByteBuffer,
        // 0 for a partial view.
        private val nativePixels: Long,
        val width: Int,
        val height: Int,
        val changed: Boolean,
        val partial: Boolean,
        internal val last: Boolean,
        /** Zero when [changed] is false. */
        val dirtyX: Int,
        val dirtyY: Int,
        val dirtyWidth: Int,
        val dirtyHeight: Int,
        /** Animation frame index. */
        val index: Int,
        /** Display time in ms; 0 for a still. */
        val duration: Int,
    ) : Closeable {
        private val closed = AtomicBoolean(false)

        internal val isOpen: Boolean get() = !closed.get()

        @Volatile
        private var pixels: ByteBuffer? = buffer

        /**
         * Packed RGBA in native byte order, 8-bit, or half-float when the decoder's
         * [ImageDecoder.isHdr]. Invalid after [close], including buffers taken before.
         */
        val image: ByteBuffer
            get() {
                val p = pixels
                check(isOpen && p != null) { "Frame has been closed" }
                return p.duplicate().order(ByteOrder.nativeOrder())
            }

        /** Idempotent and thread-safe. */
        override fun close() {
            if (closed.compareAndSet(false, true)) {
                pixels = null
                if (nativePixels != 0L) nativeFreePixels(nativePixels)
            }
        }

        @Deprecated("Call close(); the finalizer only catches a caller who forgot.")
        protected fun finalize() {
            close()
        }

        fun toByteArray(): ByteArray {
            val src = image
            return ByteArray(src.remaining()).also { src.get(it) }
        }

        /** Copies into [dst] at its position; throws IllegalArgumentException if it won't fit. */
        fun copyTo(dst: ByteBuffer) {
            val src = image
            require(dst.remaining() >= src.remaining()) {
                "destination holds ${dst.remaining()} bytes, need ${src.remaining()}"
            }
            dst.put(src)
        }
    }

    @Volatile
    private var more = true

    @Volatile
    var page: Int = 0
        private set

    /** False once a still's frame is out, or on an error; an animation always has another. */
    val hasNext: Boolean get() = more && !closed.get()

    /**
     * Decodes forward to the next whole frame, which after an animation's last is its first
     * again, forever (the file's loop count aside). If the pushed bytes run out first, returns
     * the latest progressive step instead ([Frame.partial]), or throws [NeedMoreDataException]
     * when there is nothing to show yet; call again after [pushData].
     *
     * Exif orientation is applied (a quarter turn swaps width and height). Output is sRGB
     * unless [isHdr] (a [HdrKind.GAINMAP] image's SDR base is sRGB too).
     *
     * @throws NeedMoreDataException if a streaming decode has nothing to show yet.
     * @throws OutOfMemoryException if the image does not fit in memory.
     * @throws DecodeException if the file is malformed or truncated, a still has no more
     *     frames, or this decoder is closed.
     */
    @Synchronized
    @Throws(DecodeException::class)
    fun decodeNext(): Frame {
        checkOpen()
        if (!more) throw DecodeException("No more frames")
        expirePartial()
        val f = try {
            nativeDecode()
        } catch (e: NeedMoreDataException) {
            throw e
        } catch (e: DecodeException) {
            // Some formats find their end on a call with no frame left: that pass is over.
            // A real error can't loop - the rewind repeats it.
            if (page == 0 || !isAnimated) {
                more = false // Decoder errors are final.
                throw e
            }
            rewind()
            try {
                nativeDecode()
            } catch (e2: DecodeException) {
                more = false
                throw e2
            }
        }
        if (f.last) {
            if (!isAnimated) {
                more = false
            } else {
                // Now: a whole frame owns its pixels, so the rewind can't touch this one.
                try {
                    rewind()
                } catch (e: DecodeException) {
                    // Ends here instead; the frame is still good.
                }
            }
        }
        page = f.index
        if (f.partial) lastPartial = f
        return f
    }

    // The partial frame out, a view of what the next decode or rewind rewrites or frees.
    private var lastPartial: Frame? = null

    val isAnimated: Boolean
        @Throws(DecodeException::class)
        get() = pages > 1


    /** Closed, so its [Frame.image] throws rather than reading memory the decoder reused. */
    private fun expirePartial() {
        lastPartial?.close()
        lastPartial = null
    }

    private external fun nativeDecode(): Frame

    /**
     * Back to frame 0 over the bytes already read, to replay an animation without reopening it.
     *
     * @throws DecodeException if the file isn't all in, an earlier decode failed, or this decoder
     *     is closed. Rewinding failed leaves no frames.
     */
    @Synchronized
    @Throws(DecodeException::class)
    fun rewind() {
        checkOpen()
        expirePartial()
        try {
            nativeRewind()
        } catch (e: DecodeException) {
            more = false
            throw e
        }
        more = true
        page = 0
    }

    private external fun nativeRewind()

    /**
     * The gain map, oriented like the frames; null unless [HdrKind.GAINMAP], or if it is
     * unreadable. Decoded on first call (a second image); each call returns a new copy.
     * Needs the whole file.
     *
     * @throws OutOfMemoryException if the map does not fit in memory.
     * @throws DecodeException if the image is incomplete, or this decoder is closed.
     */
    @Synchronized
    @Throws(DecodeException::class)
    fun getGainmap(): Gainmap? {
        checkOpen()
        return nativeGainmap()
    }

    private external fun nativeGainmap(): Gainmap?

    /** @throws DecodeException if [markComplete] has already been called. */
    @JvmOverloads
    @Synchronized
    @Throws(DecodeException::class)
    fun pushData(data: ByteArray, offset: Int = 0, length: Int = data.size) {
        checkOpen()
        // Subtracted: offset + length can overflow.
        require(
            offset >= 0 && length >= 0 && offset <= data.size &&
                    length <= data.size - offset
        ) {
            "offset $offset length $length out of bounds for ${data.size}"
        }
        nativePushData(data, offset, length)
    }

    private external fun nativePushData(data: ByteArray, offset: Int, length: Int)

    /** No more bytes are coming; running short is now truncation. */
    @Synchronized
    @Throws(DecodeException::class)
    fun markComplete() {
        checkOpen()
        nativeMarkComplete()
    }

    private external fun nativeMarkComplete()

    /** IFD0 and the Exif and GPS sub-IFDs. */
    @Synchronized
    @Throws(DecodeException::class)
    fun listTags(): List<String> {
        checkOpen()
        return nativeListTags().asList()
    }

    /** Multiple components are comma-separated, e.g. "8, 8, 8". */
    @Synchronized
    @Throws(DecodeException::class)
    fun getTag(name: String): String? {
        checkOpen()
        return nativeGetTag(name)
    }

    private external fun nativeListTags(): Array<String>

    private external fun nativeGetTag(name: String): String?

    /** Later calls throw [DecodeException]. */
    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        // Under the monitor, so an in-flight decode finishes first.
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
            System.loadLibrary("imagedecoder")
        }

        /**
         * Reads [inputStream] to the end, or with [complete] false only what it holds now
         * (at least the header); feed the rest with [pushData], then [markComplete]. The
         * caller closes the stream.
         *
         * @throws OutOfMemoryException if the image is larger than the decoder will buffer.
         * @throws UnknownFormatException if the bytes are not a format this build reads.
         * @throws DecodeException if the header cannot be read from what arrived.
         * @throws IOException if reading [inputStream] fails.
         */
        @JvmStatic
        @JvmOverloads
        @Throws(DecodeException::class, IOException::class)
        fun open(inputStream: InputStream, complete: Boolean = true): ImageDecoder =
            nativeNew(inputStream, complete)

        @JvmStatic
        private external fun nativeFreePixels(pixels: Long)

        @JvmStatic
        private external fun nativeNew(
            inputStream: InputStream,
            complete: Boolean,
        ): ImageDecoder
    }
}
