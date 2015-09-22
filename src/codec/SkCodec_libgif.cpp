/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCodec_libgif.h"
#include "SkCodecPriv.h"
#include "SkColorPriv.h"
#include "SkColorTable.h"
#include "SkScaledCodec.h"
#include "SkStream.h"
#include "SkSwizzler.h"
#include "SkUtils.h"

/*
 * Checks the start of the stream to see if the image is a gif
 */
bool SkGifCodec::IsGif(SkStream* stream) {
    char buf[GIF_STAMP_LEN];
    if (stream->read(buf, GIF_STAMP_LEN) == GIF_STAMP_LEN) {
        if (memcmp(GIF_STAMP,   buf, GIF_STAMP_LEN) == 0 ||
            memcmp(GIF87_STAMP, buf, GIF_STAMP_LEN) == 0 ||
            memcmp(GIF89_STAMP, buf, GIF_STAMP_LEN) == 0)
        {
            return true;
        }
    }
    return false;
}

/*
 * Warning reporting function
 */
static void gif_warning(const char* msg) {
    SkCodecPrintf("Gif Warning: %s\n", msg);
}

/*
 * Error function
 */
static SkCodec::Result gif_error(const char* msg, SkCodec::Result result = SkCodec::kInvalidInput) {
    SkCodecPrintf("Gif Error: %s\n", msg);
    return result;
}


/*
 * Read function that will be passed to gif_lib
 */
static int32_t read_bytes_callback(GifFileType* fileType, GifByteType* out, int32_t size) {
    SkStream* stream = (SkStream*) fileType->UserData;
    return (int32_t) stream->read(out, size);
}

/*
 * Open the gif file
 */
static GifFileType* open_gif(SkStream* stream) {
    return DGifOpen(stream, read_bytes_callback, nullptr);
}

/*
 * Check if a there is an index of the color table for a transparent pixel
 */
static uint32_t find_trans_index(const SavedImage& image) {
    // If there is a transparent index specified, it will be contained in an
    // extension block.  We will loop through extension blocks in reverse order
    // to check the most recent extension blocks first.
    for (int32_t i = image.ExtensionBlockCount - 1; i >= 0; i--) {
        // Get an extension block
        const ExtensionBlock& extBlock = image.ExtensionBlocks[i];

        // Specifically, we need to check for a graphics control extension,
        // which may contain transparency information.  Also, note that a valid
        // graphics control extension is always four bytes.  The fourth byte
        // is the transparent index (if it exists), so we need at least four
        // bytes.
        if (GRAPHICS_EXT_FUNC_CODE == extBlock.Function && extBlock.ByteCount >= 4) {
            // Check the transparent color flag which indicates whether a
            // transparent index exists.  It is the least significant bit of
            // the first byte of the extension block.
            if (1 == (extBlock.Bytes[0] & 1)) {
                // Use uint32_t to prevent sign extending
                return extBlock.Bytes[3];
            }

            // There should only be one graphics control extension for the image frame
            break;
        }
    }

    // Use maximum unsigned int (surely an invalid index) to indicate that a valid
    // index was not found.
    return SK_MaxU32;
}

static inline uint32_t ceil_div(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

/*
 * Gets the output row corresponding to the encoded row for interlaced gifs
 */
static uint32_t get_output_row_interlaced(uint32_t encodedRow, uint32_t height) {
    SkASSERT(encodedRow < height);
    // First pass
    if (encodedRow * 8 < height) {
        return encodedRow * 8;
    }
    // Second pass
    if (encodedRow * 4 < height) {
        return 4 + 8 * (encodedRow - ceil_div(height, 8));
    }
    // Third pass
    if (encodedRow * 2 < height) {
        return 2 + 4 * (encodedRow - ceil_div(height, 4));
    }
    // Fourth pass
    return 1 + 2 * (encodedRow - ceil_div(height, 2));
}

/*
 * This function cleans up the gif object after the decode completes
 * It is used in a SkAutoTCallIProc template
 */
void SkGifCodec::CloseGif(GifFileType* gif) {
    DGifCloseFile(gif, NULL);
}

/*
 * This function free extension data that has been saved to assist the image
 * decoder
 */
void SkGifCodec::FreeExtension(SavedImage* image) {
    if (NULL != image->ExtensionBlocks) {
        GifFreeExtensions(&image->ExtensionBlockCount, &image->ExtensionBlocks);
    }
}

/*
 * Read enough of the stream to initialize the SkGifCodec.
 * Returns a bool representing success or failure.
 *
 * @param codecOut
 * If it returned true, and codecOut was not nullptr,
 * codecOut will be set to a new SkGifCodec.
 *
 * @param gifOut
 * If it returned true, and codecOut was nullptr,
 * gifOut must be non-nullptr and gifOut will be set to a new
 * GifFileType pointer.
 *
 * @param stream
 * Deleted on failure.
 * codecOut will take ownership of it in the case where we created a codec.
 * Ownership is unchanged when we returned a gifOut.
 *
 */
bool SkGifCodec::ReadHeader(SkStream* stream, SkCodec** codecOut, GifFileType** gifOut) {
    SkAutoTDelete<SkStream> streamDeleter(stream);

    // Read gif header, logical screen descriptor, and global color table
    SkAutoTCallVProc<GifFileType, CloseGif> gif(open_gif(stream));

    if (nullptr == gif) {
        gif_error("DGifOpen failed.\n");
        return false;
    }

    // Read through gif extensions to get to the image data.  Set the
    // transparent index based on the extension data.
    uint32_t transIndex;
    SkCodec::Result result = ReadUpToFirstImage(gif, &transIndex);
    if (kSuccess != result){
        return false;
    }

    // Read the image descriptor
    if (GIF_ERROR == DGifGetImageDesc(gif)) {
        return false;
    }
    // If reading the image descriptor is successful, the image count will be
    // incremented.
    SkASSERT(gif->ImageCount >= 1);

    if (nullptr != codecOut) {
        // Get fields from header
        const int32_t width = gif->SWidth;
        const int32_t height = gif->SHeight;
        if (width <= 0 || height <= 0) {
            gif_error("Invalid dimensions.\n");
            return false;
        }

        // Determine the recommended alpha type.  The transIndex might be valid if it less
        // than 256.  We are not certain that the index is valid until we process the color
        // table, since some gifs have color tables with less than 256 colors.  If
        // there might be a valid transparent index, we must indicate that the image has
        // alpha.
        // In the case where we must support alpha, we have the option to set the
        // suggested alpha type to kPremul or kUnpremul.  Both are valid since the alpha
        // component will always be 0xFF or the entire 32-bit pixel will be set to zero.
        // We prefer kPremul because we support kPremul, and it is more efficient to use
        // kPremul directly even when kUnpremul is supported.
        SkAlphaType alphaType = (transIndex < 256) ? kPremul_SkAlphaType : kOpaque_SkAlphaType;

        // Return the codec
        // kIndex is the most natural color type for gifs, so we set this as
        // the default.
        const SkImageInfo& imageInfo = SkImageInfo::Make(width, height,
                kIndex_8_SkColorType, alphaType);
        *codecOut = new SkGifCodec(imageInfo, streamDeleter.detach(), gif.detach(), transIndex);
    } else {
        SkASSERT(nullptr != gifOut);
        streamDeleter.detach();
        *gifOut = gif.detach();
    }
    return true;
}

/*
 * Assumes IsGif was called and returned true
 * Creates a gif decoder
 * Reads enough of the stream to determine the image format
 */
SkCodec* SkGifCodec::NewFromStream(SkStream* stream) {
    SkCodec* codec = nullptr;
    if (ReadHeader(stream, &codec, nullptr)) {
        return codec;
    }
    return nullptr;
}

SkGifCodec::SkGifCodec(const SkImageInfo& srcInfo, SkStream* stream, GifFileType* gif,
        uint32_t transIndex)
    : INHERITED(srcInfo, stream)
    , fGif(gif)
    , fSrcBuffer(new uint8_t[this->getInfo().width()])
    // If it is valid, fTransIndex will be used to set fFillIndex.  We don't know if
    // fTransIndex is valid until we process the color table, since fTransIndex may
    // be greater than the size of the color table.
    , fTransIndex(transIndex)
    // Default fFillIndex is 0.  We will overwrite this if fTransIndex is valid, or if
    // there is a valid background color.
    , fFillIndex(0)
    , fFrameDims(SkIRect::MakeEmpty())
    , fFrameIsSubset(false)
    , fColorTable(NULL)
    , fSwizzler(NULL)
{}

bool SkGifCodec::onRewind() {
    GifFileType* gifOut = nullptr;
    if (!ReadHeader(this->stream(), nullptr, &gifOut)) {
        return false;
    }

    SkASSERT(nullptr != gifOut);
    fGif.reset(gifOut);
    return true;
}

SkCodec::Result SkGifCodec::ReadUpToFirstImage(GifFileType* gif, uint32_t* transIndex) {
    // Use this as a container to hold information about any gif extension
    // blocks.  This generally stores transparency and animation instructions.
    SavedImage saveExt;
    SkAutoTCallVProc<SavedImage, FreeExtension> autoFreeExt(&saveExt);
    saveExt.ExtensionBlocks = nullptr;
    saveExt.ExtensionBlockCount = 0;
    GifByteType* extData;
    int32_t extFunction;

    // We will loop over components of gif images until we find an image.  Once
    // we find an image, we will decode and return it.  While many gif files
    // contain more than one image, we will simply decode the first image.
    GifRecordType recordType;
    do {
        // Get the current record type
        if (GIF_ERROR == DGifGetRecordType(gif, &recordType)) {
            return gif_error("DGifGetRecordType failed.\n", kInvalidInput);
        }
        switch (recordType) {
            case IMAGE_DESC_RECORD_TYPE: {
                *transIndex = find_trans_index(saveExt);
                // FIXME: Gif files may have multiple images stored in a single
                //        file.  This is most commonly used to enable
                //        animations.  Since we are leaving animated gifs as a
                //        TODO, we will return kSuccess after decoding the
                //        first image in the file.  This is the same behavior
                //        as SkImageDecoder_libgif.
                //
                //        Most times this works pretty well, but sometimes it
                //        doesn't.  For example, I have an animated test image
                //        where the first image in the file is 1x1, but the
                //        subsequent images are meaningful.  This currently
                //        displays the 1x1 image, which is not ideal.  Right
                //        now I am leaving this as an issue that will be
                //        addressed when we implement animated gifs.
                //
                //        It is also possible (not explicitly disallowed in the
                //        specification) that gif files provide multiple
                //        images in a single file that are all meant to be
                //        displayed in the same frame together.  I will
                //        currently leave this unimplemented until I find a
                //        test case that expects this behavior.
                return kSuccess;
            }
            // Extensions are used to specify special properties of the image
            // such as transparency or animation.
            case EXTENSION_RECORD_TYPE:
                // Read extension data
                if (GIF_ERROR == DGifGetExtension(gif, &extFunction, &extData)) {
                    return gif_error("Could not get extension.\n", kIncompleteInput);
                }

                // Create an extension block with our data
                while (nullptr != extData) {
                    // Add a single block
                    if (GIF_ERROR == GifAddExtensionBlock(&saveExt.ExtensionBlockCount,
                                                          &saveExt.ExtensionBlocks,
                                                          extFunction, extData[0], &extData[1]))
                    {
                        return gif_error("Could not add extension block.\n", kIncompleteInput);
                    }
                    // Move to the next block
                    if (GIF_ERROR == DGifGetExtensionNext(gif, &extData)) {
                        return gif_error("Could not get next extension.\n", kIncompleteInput);
                    }
                }
                break;

            // Signals the end of the gif file
            case TERMINATE_RECORD_TYPE:
                break;

            default:
                // DGifGetRecordType returns an error if the record type does
                // not match one of the above cases.  This should not be
                // reached.
                SkASSERT(false);
                break;
        }
    } while (TERMINATE_RECORD_TYPE != recordType);

    return gif_error("Could not find any images to decode in gif file.\n", kInvalidInput);
}

/*
 * A gif may contain many image frames, all of different sizes.
 * This function checks if the frame dimensions are valid and corrects them if
 * necessary.
 */
bool SkGifCodec::setFrameDimensions(const GifImageDesc& desc) {
    // Fail on non-positive dimensions
    int32_t frameLeft = desc.Left;
    int32_t frameTop = desc.Top;
    int32_t frameWidth = desc.Width;
    int32_t frameHeight = desc.Height;
    int32_t height = this->getInfo().height();
    int32_t width = this->getInfo().width();
    if (frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }

    // Treat the following cases as warnings and try to fix
    if (frameWidth > width) {
        gif_warning("Image frame too wide, shrinking.\n");
        frameWidth = width;
        frameLeft = 0;
    } else if (frameLeft + frameWidth > width) {
        gif_warning("Shifting image frame to left to fit.\n");
        frameLeft = width - frameWidth;
    } else if (frameLeft < 0) {
        gif_warning("Shifting image frame to right to fit\n");
        frameLeft = 0;
    }
    if (frameHeight > height) {
        gif_warning("Image frame too tall, shrinking.\n");
        frameHeight = height;
        frameTop = 0;
    } else if (frameTop + frameHeight > height) {
        gif_warning("Shifting image frame up to fit.\n");
        frameTop = height - frameHeight;
    } else if (frameTop < 0) {
        gif_warning("Shifting image frame down to fit\n");
        frameTop = 0;
    }
    fFrameDims.setXYWH(frameLeft, frameTop, frameWidth, frameHeight);

    // Indicate if the frame dimensions do not match the header dimensions
    if (this->getInfo().dimensions() != fFrameDims.size()) {
        fFrameIsSubset = true;
    }

    return true;
}

void SkGifCodec::initializeColorTable(const SkImageInfo& dstInfo, SkPMColor* inputColorPtr,
        int* inputColorCount) {
    // Set up our own color table
    const uint32_t maxColors = 256;
    SkPMColor colorPtr[256];
    if (NULL != inputColorCount) {
        // We set the number of colors to maxColors in order to ensure
        // safe memory accesses.  Otherwise, an invalid pixel could
        // access memory outside of our color table array.
        *inputColorCount = maxColors;
    }

    // Get local color table
    ColorMapObject* colorMap = fGif->Image.ColorMap;
    // If there is no local color table, use the global color table
    if (NULL == colorMap) {
        colorMap = fGif->SColorMap;
    }

    uint32_t colorCount = 0;
    if (NULL != colorMap) {
        colorCount = colorMap->ColorCount;
        // giflib guarantees these properties
        SkASSERT(colorCount == (unsigned) (1 << (colorMap->BitsPerPixel)));
        SkASSERT(colorCount <= 256);
        for (uint32_t i = 0; i < colorCount; i++) {
            colorPtr[i] = SkPackARGB32(0xFF, colorMap->Colors[i].Red,
                    colorMap->Colors[i].Green, colorMap->Colors[i].Blue);
        }
    }

    // Gifs have the option to specify the color at a single index of the color
    // table as transparent.  If the transparent index is greater than the
    // colorCount, we know that there is no valid transparent color in the color
    // table.  If there is not valid transparent index, we will try to use the
    // backgroundIndex as the fill index.  If the backgroundIndex is also not
    // valid, we will let fFillIndex default to 0 (it is set to zero in the
    // constructor).  This behavior is not specified but matches
    // SkImageDecoder_libgif.
    uint32_t backgroundIndex = fGif->SBackGroundColor;
    if (fTransIndex < colorCount) {
        colorPtr[fTransIndex] = SK_ColorTRANSPARENT;
        fFillIndex = fTransIndex;
    } else if (backgroundIndex < colorCount) {
        fFillIndex = backgroundIndex;
    }

    // Fill in the color table for indices greater than color count.
    // This allows for predictable, safe behavior.
    for (uint32_t i = colorCount; i < maxColors; i++) {
        colorPtr[i] = colorPtr[fFillIndex];
    }

    fColorTable.reset(new SkColorTable(colorPtr, maxColors));
    copy_color_table(dstInfo, this->fColorTable, inputColorPtr, inputColorCount);
}

SkCodec::Result SkGifCodec::prepareToDecode(const SkImageInfo& dstInfo, SkPMColor* inputColorPtr,
        int* inputColorCount, const Options& opts) {
    // Rewind if necessary
    if (!this->rewindIfNeeded()) {
        return kCouldNotRewind;
    }

    // Check for valid input parameters
    if (opts.fSubset) {
        // Subsets are not supported.
        return kUnimplemented;
    }
    if (!conversion_possible(dstInfo, this->getInfo())) {
        return gif_error("Cannot convert input type to output type.\n",
                kInvalidConversion);
    }


    // We have asserted that the image count is at least one in ReadHeader().
    SavedImage* image = &fGif->SavedImages[fGif->ImageCount - 1];
    const GifImageDesc& desc = image->ImageDesc;

    // Check that the frame dimensions are valid and set them
    if(!this->setFrameDimensions(desc)) {
        return gif_error("Invalid dimensions for image frame.\n", kInvalidInput);
    }

    // Initialize color table and copy to the client if necessary
    this->initializeColorTable(dstInfo, inputColorPtr, inputColorCount);
    return kSuccess;
}

SkCodec::Result SkGifCodec::initializeSwizzler(const SkImageInfo& dstInfo,
        ZeroInitialized zeroInit) {
    const SkPMColor* colorPtr = get_color_ptr(fColorTable.get());
    fSwizzler.reset(SkSwizzler::CreateSwizzler(SkSwizzler::kIndex,
            colorPtr, dstInfo, zeroInit, this->getInfo()));
    if (nullptr != fSwizzler.get()) {
        return kSuccess;
    }
    return kUnimplemented;
}

SkCodec::Result SkGifCodec::readRow() {
    if (GIF_ERROR == DGifGetLine(fGif, fSrcBuffer.get(), fFrameDims.width())) {
        return kIncompleteInput;
    }
    return kSuccess;
}

/*
 * Initiates the gif decode
 */
SkCodec::Result SkGifCodec::onGetPixels(const SkImageInfo& dstInfo,
                                        void* dst, size_t dstRowBytes,
                                        const Options& opts,
                                        SkPMColor* inputColorPtr,
                                        int* inputColorCount) {
    Result result = this->prepareToDecode(dstInfo, inputColorPtr, inputColorCount, opts);
    if (kSuccess != result) {
        return result;
    }

    if (dstInfo.dimensions() != this->getInfo().dimensions()) {
        return gif_error("Scaling not supported.\n", kInvalidScale);
    }

    // Initialize the swizzler
    if (fFrameIsSubset) {
        const SkImageInfo subsetDstInfo = dstInfo.makeWH(fFrameDims.width(), fFrameDims.height());
        if (kSuccess != this->initializeSwizzler(subsetDstInfo, opts.fZeroInitialized)) {
            return gif_error("Could not initialize swizzler.\n", kUnimplemented);
        }

        // Fill the background
        const SkPMColor* colorPtr = get_color_ptr(fColorTable.get());
        SkSwizzler::Fill(dst, dstInfo, dstRowBytes, this->getInfo().height(),
                fFillIndex, colorPtr, opts.fZeroInitialized);

        // Modify the dst pointer
        const int32_t dstBytesPerPixel = SkColorTypeBytesPerPixel(dstInfo.colorType());
        dst = SkTAddOffset<void*>(dst, dstRowBytes * fFrameDims.top() +
                dstBytesPerPixel * fFrameDims.left());
    } else {
        if (kSuccess != this->initializeSwizzler(dstInfo, opts.fZeroInitialized)) {
            return gif_error("Could not initialize swizzler.\n", kUnimplemented);
        }
    }

    // Check the interlace flag and iterate over rows of the input
    uint32_t width = fFrameDims.width();
    uint32_t height = fFrameDims.height();
    if (fGif->Image.Interlace) {
        // In interlace mode, the rows of input are rearranged in
        // the output image.  We a helper function to help us
        // rearrange the decoded rows.
        for (uint32_t y = 0; y < height; y++) {
            if (kSuccess != this->readRow()) {
                // Recover from error by filling remainder of image
                memset(fSrcBuffer.get(), fFillIndex, width);
                for (; y < height; y++) {
                    void* dstRow = SkTAddOffset<void>(dst,
                            dstRowBytes * get_output_row_interlaced(y, height));
                    fSwizzler->swizzle(dstRow, fSrcBuffer.get());
                }
                return gif_error("Could not decode line.\n", kIncompleteInput);
            }
            void* dstRow = SkTAddOffset<void>(dst,
                    dstRowBytes * get_output_row_interlaced(y, height));
            fSwizzler->swizzle(dstRow, fSrcBuffer.get());
        }
    } else {
        // Standard mode
        void* dstRow = dst;
        for (uint32_t y = 0; y < height; y++) {
            if (kSuccess != this->readRow()) {
                const SkPMColor* colorPtr = get_color_ptr(fColorTable.get());
                SkSwizzler::Fill(dstRow, dstInfo, dstRowBytes,
                        height - y, fFillIndex, colorPtr, opts.fZeroInitialized);
                return gif_error("Could not decode line\n", kIncompleteInput);
            }
            fSwizzler->swizzle(dstRow, fSrcBuffer.get());
            dstRow = SkTAddOffset<void>(dstRow, dstRowBytes);
        }
    }
    return kSuccess;
}

// TODO (msarett): skbug.com/3582
//                 Should we implement reallyHasAlpha?  Or should we read extension blocks in the
//                 header?  Or should we do both?

class SkGifScanlineDecoder : public SkScanlineDecoder {
public:
    SkGifScanlineDecoder(const SkImageInfo& srcInfo, SkGifCodec* codec)
        : INHERITED(srcInfo)
        , fCodec(codec)
    {}

    SkEncodedFormat onGetEncodedFormat() const override {
        return kGIF_SkEncodedFormat;
    }

    SkCodec::Result onStart(const SkImageInfo& dstInfo, const SkCodec::Options& opts,
                            SkPMColor inputColorPtr[], int* inputColorCount) override {
        SkCodec::Result result = fCodec->prepareToDecode(dstInfo, inputColorPtr, inputColorCount,
                this->options());
        if (SkCodec::kSuccess != result) {
            return result;
        }

        // Check to see if scaling was requested.
        if (dstInfo.dimensions() != this->getInfo().dimensions()) {
            if (!SkScaledCodec::DimensionsSupportedForSampling(this->getInfo(), dstInfo)) {
                return gif_error("Scaling not supported.\n", SkCodec::kInvalidScale);
            }
        }

        // Initialize the swizzler
        if (fCodec->fFrameIsSubset) {
            int sampleX;
            SkScaledCodec::ComputeSampleSize(dstInfo, fCodec->getInfo(), &sampleX, NULL);
            const SkImageInfo subsetDstInfo = dstInfo.makeWH(
                    get_scaled_dimension(fCodec->fFrameDims.width(), sampleX),
                    fCodec->fFrameDims.height());
            if (SkCodec::kSuccess != fCodec->initializeSwizzler(subsetDstInfo,
                    opts.fZeroInitialized)) {
                return gif_error("Could not initialize swizzler.\n", SkCodec::kUnimplemented);
            }
        } else {
            if (SkCodec::kSuccess != fCodec->initializeSwizzler(dstInfo, opts.fZeroInitialized)) {
                return gif_error("Could not initialize swizzler.\n", SkCodec::kUnimplemented);
            }
        }

        return SkCodec::kSuccess;
    }

    SkCodec::Result onGetScanlines(void* dst, int count, size_t rowBytes) override {
        if (fCodec->fFrameIsSubset) {
            // Fill the requested rows
            const SkPMColor* colorPtr = get_color_ptr(fCodec->fColorTable.get());
            SkSwizzler::Fill(dst, this->dstInfo(), rowBytes, count, fCodec->fFillIndex,
                    colorPtr, this->options().fZeroInitialized);

            // Do nothing for rows before the image frame
            int rowsBeforeFrame = fCodec->fFrameDims.top() - INHERITED::getY();
            if (rowsBeforeFrame > 0) {
                count = SkTMin(0, count - rowsBeforeFrame);
                dst = SkTAddOffset<void>(dst, rowBytes * rowsBeforeFrame);
            }

            // Do nothing for rows after the image frame
            int rowsAfterFrame = INHERITED::getY() + count - fCodec->fFrameDims.bottom();
            if (rowsAfterFrame > 0) {
                count = SkTMin(0, count - rowsAfterFrame);
            }

            // Adjust dst pointer for left offset
            dst = SkTAddOffset<void>(dst, SkColorTypeBytesPerPixel(
                    this->dstInfo().colorType()) * fCodec->fFrameDims.left());
        }

        for (int i = 0; i < count; i++) {
            if (SkCodec::kSuccess != fCodec->readRow()) {
                const SkPMColor* colorPtr = get_color_ptr(fCodec->fColorTable.get());
                SkSwizzler::Fill(dst, this->dstInfo(), rowBytes,
                        count - i, fCodec->fFillIndex, colorPtr,
                        this->options().fZeroInitialized);
                return gif_error("Could not decode line\n", SkCodec::kIncompleteInput);
            }
            fCodec->fSwizzler->swizzle(dst, fCodec->fSrcBuffer.get());
            dst = SkTAddOffset<void>(dst, rowBytes);
        }
        return SkCodec::kSuccess;
    }

    SkScanlineOrder onGetScanlineOrder() const override {
        if (fCodec->fGif->Image.Interlace) {
            return kOutOfOrder_SkScanlineOrder;
        } else {
            return kTopDown_SkScanlineOrder;
        }
    }

    int onGetY() const override {
        if (fCodec->fGif->Image.Interlace) {
            return get_output_row_interlaced(INHERITED::onGetY(), this->dstInfo().height());
        } else {
            return INHERITED::onGetY();
        }
    }

private:
    SkAutoTDelete<SkGifCodec>   fCodec;

    typedef SkScanlineDecoder INHERITED;
};

SkScanlineDecoder* SkGifCodec::NewSDFromStream(SkStream* stream) {
    SkAutoTDelete<SkGifCodec> codec (static_cast<SkGifCodec*>(SkGifCodec::NewFromStream(stream)));
    if (!codec) {
        return NULL;
    }

    const SkImageInfo& srcInfo = codec->getInfo();

    return new SkGifScanlineDecoder(srcInfo, codec.detach());
}
