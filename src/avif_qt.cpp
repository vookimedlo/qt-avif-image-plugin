/*
 * QT plug-in to allow import/export in AVIF image format.
 * Author: Daniel Novomesky
 */

/*
This software uses libavif
URL: https://github.com/AOMediaCodec/libavif/

Copyright 2019 Joe Drago. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <QtGlobal>
#include <QThread>

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
#include <QColorSpace>
#endif

#include "avif_qt_p.h"

QImageIOPlugin::Capabilities AVIFPlugin::capabilities(QIODevice *device, const QByteArray &format) const
{
    if (format == "avif") {
        return Capabilities(CanRead | CanWrite);
    }

    if (format == "avifs") {
        return Capabilities(CanRead);
    }

    if (!format.isEmpty()) {
        return {};
    }
    if (!device->isOpen()) {
        return {};
    }

    Capabilities cap;
    if (device->isReadable() && AVIFHandler::canRead(device)) {
        cap |= CanRead;
    }
    if (device->isWritable()) {
        cap |= CanWrite;
    }
    return cap;
}

AVIFHandler::AVIFHandler() :
    m_parseState(ParseAvifNotParsed),
    m_quality(52),
    m_container_width(0),
    m_container_height(0),
    m_rawAvifData(AVIF_DATA_EMPTY),
    m_decoder(NULL),
    m_must_jump_to_next_image(false)
{
}

AVIFHandler::~AVIFHandler()
{
    if (m_decoder) {
        avifDecoderDestroy(m_decoder);
    }
}

QImageIOHandler *AVIFPlugin::create(QIODevice *device, const QByteArray &format) const
{
    QImageIOHandler *handler = new AVIFHandler;
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}

bool AVIFHandler::canRead() const
{
    if (m_parseState == ParseAvifNotParsed && !canRead(device())) {
        return false;
    }

    if (m_parseState != ParseAvifError) {
        setFormat("avif");
        return true;
    }
    return false;
}

bool AVIFHandler::canRead(QIODevice *device)
{
    if (!device) {
        return false;
    }
    QByteArray header = device->peek(144);
    if (header.size() < 12) {
        return false;
    }

    avifROData input;
    input.data = (const uint8_t *) header.constData();
    input.size = header.size();

    if (avifPeekCompatibleFileType(&input)) {
        return true;
    }
    return false;
}

bool AVIFHandler::ensureParsed() const
{
    if (m_parseState == ParseAvifSuccess) {
        return true;
    }
    if (m_parseState == ParseAvifError) {
        return false;
    }

    AVIFHandler *that = const_cast<AVIFHandler *>(this);

    return that->ensureDecoder();
}

bool AVIFHandler::ensureDecoder()
{
    if (m_decoder) {
        return true;
    }

    m_rawData = device()->readAll();

    m_rawAvifData.data = (const uint8_t *) m_rawData.constData();
    m_rawAvifData.size = m_rawData.size();

    if (avifPeekCompatibleFileType(&m_rawAvifData) == AVIF_FALSE) {
        m_parseState = ParseAvifError;
        return false;
    }


    m_decoder = avifDecoderCreate();

    avifResult decodeResult;

    decodeResult = avifDecoderParse(m_decoder, &m_rawAvifData);
    if (decodeResult != AVIF_RESULT_OK) {
        qWarning("ERROR: Failed to parse input: %s\n", avifResultToString(decodeResult));

        avifDecoderDestroy(m_decoder);
        m_decoder = NULL;
        m_parseState = ParseAvifError;
        return false;
    }

    m_container_width = m_decoder->containerWidth;
    m_container_height = m_decoder->containerHeight;

    decodeResult = avifDecoderNextImage(m_decoder);

    if (decodeResult == AVIF_RESULT_OK) {

        if ((m_container_width != m_decoder->image->width) ||
                (m_container_height != m_decoder->image->height)) {
            qWarning("Decoded image size (%dx%d) do not match declared container size (%dx%d)!\n",
                     m_decoder->image->width, m_decoder->image->height,
                     m_container_width, m_container_height);

            avifDecoderDestroy(m_decoder);
            m_decoder = NULL;
            m_parseState = ParseAvifError;
            return false;
        }

        m_parseState = ParseAvifSuccess;
        if (decode_one_frame()) {
            return true;
        } else {
            m_parseState = ParseAvifError;
            return false;
        }
    } else {
        qWarning("ERROR: Failed to decode image: %s\n", avifResultToString(decodeResult));
    }

    avifDecoderDestroy(m_decoder);
    m_decoder = NULL;
    m_parseState = ParseAvifError;
    return false;
}

bool AVIFHandler::decode_one_frame()
{
    if (!ensureParsed()) {
        return false;
    }

    bool loadalpha;

    if (m_decoder->image->alphaPlane) {
        loadalpha = true;
    } else {
        loadalpha = false;
    }

    QImage result(m_decoder->image->width, m_decoder->image->height, loadalpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    if (m_decoder->image->icc.data && (m_decoder->image->icc.size > 0)) {
        result.setColorSpace(QColorSpace::fromIccProfile(QByteArray::fromRawData((const char *) m_decoder->image->icc.data, (int) m_decoder->image->icc.size)));
        if (! result.colorSpace().isValid()) {
            qWarning("Invalid QColorSpace created from ICC!\n");
        }
    } else {

        avifColorPrimaries primaries_to_load;
        avifTransferCharacteristics trc_to_load;

        if ((m_decoder->image->colorPrimaries == 2 /* AVIF_COLOR_PRIMARIES_UNSPECIFIED */) ||
                (m_decoder->image->colorPrimaries == 0 /* AVIF_COLOR_PRIMARIES_UNKNOWN */)) {
            primaries_to_load = (avifColorPrimaries) 1; // AVIF_COLOR_PRIMARIES_BT709
        } else {
            primaries_to_load = m_decoder->image->colorPrimaries;
        }
        if ((m_decoder->image->transferCharacteristics == 2 /* AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED */) ||
                (m_decoder->image->transferCharacteristics == 0 /* AVIF_TRANSFER_CHARACTERISTICS_UNKNOWN */)) {
            trc_to_load = (avifTransferCharacteristics) 13; // AVIF_TRANSFER_CHARACTERISTICS_SRGB
        } else {
            trc_to_load = m_decoder->image->transferCharacteristics;
        }

        float prim[8]; // outPrimaries: rX, rY, gX, gY, bX, bY, wX, wY
        avifColorPrimariesGetValues(primaries_to_load, prim);

        QPointF redPoint(prim[0], prim[1]);
        QPointF greenPoint(prim[2], prim[3]);
        QPointF bluePoint(prim[4], prim[5]);
        QPointF whitePoint(prim[6], prim[7]);


        QColorSpace::TransferFunction q_trc = QColorSpace::TransferFunction::Custom;
        float q_trc_gamma = 0.0f;

        switch (trc_to_load) {
        /* AVIF_TRANSFER_CHARACTERISTICS_BT470M */
        case 4:
            q_trc = QColorSpace::TransferFunction::Gamma;
            q_trc_gamma = 2.2f;
            break;
        /* AVIF_TRANSFER_CHARACTERISTICS_BT470BG */
        case 5:
            q_trc = QColorSpace::TransferFunction::Gamma;
            q_trc_gamma = 2.8f;
            break;
        /* AVIF_TRANSFER_CHARACTERISTICS_LINEAR */
        case 8:
            q_trc = QColorSpace::TransferFunction::Linear;
            break;
        /* AVIF_TRANSFER_CHARACTERISTICS_SRGB */
        case 13:
            q_trc =  QColorSpace::TransferFunction::SRgb;
            break;
        default:
            qWarning("CICP colorPrimaries: %d, transferCharacteristics: %d\nThe colorspace is unsupported by this plug-in yet.",
                     m_decoder->image->colorPrimaries, m_decoder->image->transferCharacteristics);
            q_trc = QColorSpace::TransferFunction::SRgb;
            break;
        }

        if (q_trc != QColorSpace::TransferFunction::Custom) {   //we create new colorspace using Qt
            switch (primaries_to_load) {
            /* AVIF_COLOR_PRIMARIES_BT709 */
            case 1:
                result.setColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, q_trc, q_trc_gamma));
                break;
            /* AVIF_COLOR_PRIMARIES_SMPTE432 */
            case 12:
                result.setColorSpace(QColorSpace(QColorSpace::Primaries::DciP3D65, q_trc, q_trc_gamma));
                break;
            default:
                result.setColorSpace(QColorSpace(whitePoint, redPoint, greenPoint, bluePoint, q_trc, q_trc_gamma));
                break;
            }
        }

        if (! result.colorSpace().isValid()) {
            qWarning("Invalid QColorSpace created from NCLX/CICP!\n");
        }
    }
#endif

    avifRGBImage rgb;
    rgb.width = m_decoder->image->width;
    rgb.height = m_decoder->image->height;
    rgb.depth = 8;
    rgb.rowBytes = result.bytesPerLine();
    rgb.pixels = result.bits();

    if (loadalpha) {
        rgb.format = AVIF_RGB_FORMAT_RGBA;
    } else {
        rgb.format = AVIF_RGB_FORMAT_RGB;
    }

    avifImageYUVToRGB(m_decoder->image, &rgb);

    if (m_decoder->image->transformFlags & AVIF_TRANSFORM_CLAP) {
        if ((m_decoder->image->clap.widthD > 0) && (m_decoder->image->clap.heightD > 0) &&
                (m_decoder->image->clap.horizOffD > 0) && (m_decoder->image->clap.vertOffD > 0)) {
            int  new_width, new_height, offx, offy;

            new_width = (int)((double)(m_decoder->image->clap.widthN)  / (m_decoder->image->clap.widthD) + 0.5);
            if (new_width > result.width()) {
                new_width = result.width();
            }

            new_height = (int)((double)(m_decoder->image->clap.heightN) / (m_decoder->image->clap.heightD) + 0.5);
            if (new_height > result.height()) {
                new_height = result.height();
            }

            if (new_width > 0 && new_height > 0) {

                offx = ((double)((int32_t) m_decoder->image->clap.horizOffN)) / (m_decoder->image->clap.horizOffD) +
                       (result.width() - new_width) / 2.0 + 0.5;
                if (offx < 0) {
                    offx = 0;
                } else if (offx > (result.width() - new_width)) {
                    offx = result.width() - new_width;
                }

                offy = ((double)((int32_t) m_decoder->image->clap.vertOffN)) / (m_decoder->image->clap.vertOffD) +
                       (result.height() - new_height) / 2.0 + 0.5;
                if (offy < 0) {
                    offy = 0;
                } else if (offy > (result.height() - new_height)) {
                    offy = result.height() - new_height;
                }

                result = result.copy(offx, offy, new_width, new_height);
            }
        }

        else { //Zero values, we need to avoid 0 divide.
            qWarning("ERROR: Wrong values in avifCleanApertureBox\n");
        }
    }

    if (m_decoder->image->transformFlags & AVIF_TRANSFORM_IROT) {
        QTransform transform;
        switch (m_decoder->image->irot.angle) {
        case 1:
            transform.rotate(-90);
            result = result.transformed(transform);
            break;
        case 2:
            transform.rotate(180);
            result = result.transformed(transform);
            break;
        case 3:
            transform.rotate(90);
            result = result.transformed(transform);
            break;
        }
    }

    if (m_decoder->image->transformFlags & AVIF_TRANSFORM_IMIR) {
        switch (m_decoder->image->imir.axis) {
        case 0: //vertical
            result = result.mirrored(false, true);
            break;
        case 1: //horizontal
            result = result.mirrored(true, false);
            break;
        }
    }

    // Image is converted for compatibility reasons to the most known 8bit formats
    if (loadalpha) {
        m_current_image = result.convertToFormat(QImage::Format_ARGB32);
    } else {
        m_current_image = result.convertToFormat(QImage::Format_RGB32);
    }

    m_must_jump_to_next_image = false;
    return true;
}

bool AVIFHandler::read(QImage *image)
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_must_jump_to_next_image) {
        jumpToNextImage();
    }

    *image = m_current_image;
    if (imageCount() >= 2) {
        m_must_jump_to_next_image = true;
    }
    return true;
}

bool AVIFHandler::write(const QImage &image)
{
    QImage tmpimage = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_RGBA8888 : QImage::Format_RGB888);

    int maxQuantizer = AVIF_QUANTIZER_WORST_QUALITY * (100 - qBound(0, m_quality, 100)) / 100;
    int maxQuantizerAlpha = 0;

    avifPixelFormat pixel_format = AVIF_PIXEL_FORMAT_YUV420;

    if (maxQuantizer < 20) {
        if (maxQuantizer < 10) {
            pixel_format = AVIF_PIXEL_FORMAT_YUV444;
        } else {
            pixel_format = AVIF_PIXEL_FORMAT_YUV422;
        }
    } else if (maxQuantizer > 40) {   //we decrease quality of aplha channel here
        maxQuantizerAlpha = maxQuantizer - 40;
    }

    avifImage *avif = avifImageCreate(tmpimage.width(), tmpimage.height(), 8, pixel_format);
    avif->yuvRange = AVIF_RANGE_FULL;

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    if (tmpimage.colorSpace().isValid()) {
        avifColorPrimaries primaries_to_save;
        avifTransferCharacteristics transfer_to_save;
        avifMatrixCoefficients matrix_to_save;

        switch (tmpimage.colorSpace().primaries()) {
        case QColorSpace::Primaries::SRgb:
            /* AVIF_COLOR_PRIMARIES_BT709 */
            primaries_to_save = (avifColorPrimaries) 1;
            /* AVIF_MATRIX_COEFFICIENTS_BT709 */
            matrix_to_save = (avifMatrixCoefficients) 1;
            break;
        case QColorSpace::Primaries::DciP3D65:
            /* AVIF_NCLX_COLOUR_PRIMARIES_P3, AVIF_NCLX_COLOUR_PRIMARIES_SMPTE432 */
            primaries_to_save = (avifColorPrimaries) 12;
            /* AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL */
            matrix_to_save = (avifMatrixCoefficients) 12;
            break;
        default:
            /* AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED */
            primaries_to_save = (avifColorPrimaries) 2;
            /* AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED */
            matrix_to_save = (avifMatrixCoefficients) 2;
            break;
        }

        switch (tmpimage.colorSpace().transferFunction()) {
        case QColorSpace::TransferFunction::Linear:
            /* AVIF_TRANSFER_CHARACTERISTICS_LINEAR */
            transfer_to_save = (avifTransferCharacteristics) 8;
            break;
        case QColorSpace::TransferFunction::Gamma:
            if (qAbs(tmpimage.colorSpace().gamma() - 2.2f) < 0.1f) {
                /* AVIF_TRANSFER_CHARACTERISTICS_BT470M */
                transfer_to_save = (avifTransferCharacteristics) 4;
            } else if (qAbs(tmpimage.colorSpace().gamma() - 2.8f) < 0.1f) {
                /* AVIF_TRANSFER_CHARACTERISTICS_BT470BG */
                transfer_to_save = (avifTransferCharacteristics) 5;
            } else {
                /* AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED */
                transfer_to_save = (avifTransferCharacteristics) 2;
            }
            break;
        case QColorSpace::TransferFunction::SRgb:
            /* AVIF_TRANSFER_CHARACTERISTICS_SRGB */
            transfer_to_save = (avifTransferCharacteristics) 13;
            break;
        default:
            /* AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED */
            transfer_to_save = (avifTransferCharacteristics) 2;
            break;
        }

        if ((primaries_to_save != 2) &&
                (transfer_to_save != 2)) {    //recognized primaries and trc
            avif->colorPrimaries = primaries_to_save;
            avif->transferCharacteristics = transfer_to_save;
            avif->matrixCoefficients = matrix_to_save;
        } else if ((primaries_to_save == 2) &&
                   (transfer_to_save != 2)) {    //other primaries but known trc
            avif->colorPrimaries = (avifColorPrimaries) 1; // AVIF_COLOR_PRIMARIES_BT709
            avif->matrixCoefficients = (avifMatrixCoefficients) 1; // AVIF_MATRIX_COEFFICIENTS_BT709

            switch (transfer_to_save) {
            case 8: // AVIF_TRANSFER_CHARACTERISTICS_LINEAR
                tmpimage.convertToColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Linear));
                break;
            case 4: // AVIF_TRANSFER_CHARACTERISTICS_BT470M
                tmpimage.convertToColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, 2.2f));
                break;
            case 5: // AVIF_TRANSFER_CHARACTERISTICS_BT470BG
                tmpimage.convertToColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, 2.8f));
                break;
            default: // AVIF_TRANSFER_CHARACTERISTICS_SRGB + any other
                tmpimage.convertToColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::SRgb));
                transfer_to_save = (avifTransferCharacteristics) 13;
                break;
            }

            avif->transferCharacteristics = transfer_to_save;

        } else if ((primaries_to_save != 2) &&
                   (transfer_to_save == 2)) {    //recognized primaries but other trc
            avif->colorPrimaries = primaries_to_save;
            avif->transferCharacteristics = (avifTransferCharacteristics) 13;
            avif->matrixCoefficients = matrix_to_save;

            tmpimage.convertToColorSpace(tmpimage.colorSpace().withTransferFunction(QColorSpace::TransferFunction::SRgb));

        } else { //unrecognized profile
            avif->colorPrimaries = (avifColorPrimaries) 1; // AVIF_COLOR_PRIMARIES_BT709
            avif->transferCharacteristics = (avifTransferCharacteristics) 13;
            avif->matrixCoefficients = (avifMatrixCoefficients) 1; // AVIF_MATRIX_COEFFICIENTS_BT709
            tmpimage.convertToColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::SRgb));
        }
    }
#endif

    avifRGBImage rgb;
    rgb.width = tmpimage.width();
    rgb.height = tmpimage.height();
    rgb.depth = 8;
    rgb.rowBytes = tmpimage.bytesPerLine();
    rgb.pixels = (uint8_t *) tmpimage.constBits();

    if (tmpimage.hasAlphaChannel()) {
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        avif->alphaRange = AVIF_RANGE_FULL;
    } else {
        rgb.format = AVIF_RGB_FORMAT_RGB;
    }

    avifResult res = avifImageRGBToYUV(avif, &rgb);
    if (res != AVIF_RESULT_OK) {
        qWarning("ERROR in avifImageRGBToYUV: %s\n", avifResultToString(res));
        return false;
    }

    avifRWData raw = AVIF_DATA_EMPTY;
    avifEncoder *encoder = avifEncoderCreate();
    encoder->maxThreads = QThread::idealThreadCount();
    encoder->minQuantizer = AVIF_QUANTIZER_BEST_QUALITY;
    encoder->maxQuantizer = maxQuantizer;

    if (tmpimage.hasAlphaChannel()) {
        encoder->minQuantizerAlpha = AVIF_QUANTIZER_LOSSLESS;
        encoder->maxQuantizerAlpha = maxQuantizerAlpha;
    }

    encoder->speed = 8;

    res = avifEncoderWrite(encoder, avif, &raw);
    avifEncoderDestroy(encoder);
    avifImageDestroy(avif);

    if (res == AVIF_RESULT_OK) {
        qint64 status = device()->write((const char *) raw.data, raw.size);
        avifRWDataFree(&raw);

        if (status > 0) {
            return true;
        } else if (status == -1) {
            qWarning("Write error: %s\n", qUtf8Printable(device()->errorString()));
            return false;
        }
    } else {
        qWarning("ERROR: Failed to encode: %s\n", avifResultToString(res));
    }

    return false;
}


QVariant AVIFHandler::option(ImageOption option) const
{
    if (!supportsOption(option) || !ensureParsed()) {
        return QVariant();
    }

    switch (option) {
    case Quality:
        return m_quality;
    case Size:
        return m_current_image.size();
    case Animation:
        if (imageCount() >= 2) {
            return true;
        } else {
            return false;
        }
    default:
        return QVariant();
    }
}

void AVIFHandler::setOption(ImageOption option, const QVariant &value)
{
    switch (option) {
    case Quality:
        m_quality = value.toInt();
        if (m_quality > 100) {
            m_quality = 100;
        } else if (m_quality < 0) {
            m_quality = 52;
        }
        return;
    default:
        break;
    }
    QImageIOHandler::setOption(option, value);
}

bool AVIFHandler::supportsOption(ImageOption option) const
{
    return option == Quality
           || option == Size
           || option == Animation;
}

int AVIFHandler::imageCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

    if (m_decoder->imageCount >= 1) {
        return m_decoder->imageCount;
    }
    return 0;
}

int AVIFHandler::currentImageNumber() const
{
    if (m_parseState == ParseAvifNotParsed) {
        return -1;
    }

    if (m_parseState == ParseAvifError || !m_decoder) {
        return 0;
    }

    return m_decoder->imageIndex;
}

bool AVIFHandler::jumpToNextImage()
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_decoder->imageCount < 2) {
        return true;
    }

    if (m_decoder->imageIndex >= m_decoder->imageCount - 1) {   //start from begining
        avifDecoderReset(m_decoder);
    }

    avifResult decodeResult = avifDecoderNextImage(m_decoder);

    if (decodeResult != AVIF_RESULT_OK) {
        qWarning("ERROR: Failed to decode Next image in sequence: %s\n", avifResultToString(decodeResult));
        m_parseState = ParseAvifError;
        return false;
    }

    if ((m_container_width != m_decoder->image->width) ||
            (m_container_height != m_decoder->image->height)) {
        qWarning("Decoded image sequence size (%dx%d) do not match declared container size (%dx%d)!\n",
                 m_decoder->image->width, m_decoder->image->height,
                 m_container_width, m_container_height);

        m_parseState = ParseAvifError;
        return false;
    }

    if (decode_one_frame()) {
        return true;
    } else {
        m_parseState = ParseAvifError;
        return false;
    }

}

bool AVIFHandler::jumpToImage(int imageNumber)
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_decoder->imageCount < 2) {   //not an animation
        if (imageNumber == 0) {
            return true;
        } else {
            return false;
        }
    }

    if (imageNumber < 0 || imageNumber >= m_decoder->imageCount) {   //wrong index
        return false;
    }

    if (imageNumber == m_decoder->imageCount) {   // we are here already
        return true;
    }

    avifResult decodeResult = avifDecoderNthImage(m_decoder, imageNumber);

    if (decodeResult != AVIF_RESULT_OK) {
        qWarning("ERROR: Failed to decode %d th Image in sequence: %s\n", imageNumber, avifResultToString(decodeResult));
        m_parseState = ParseAvifError;
        return false;
    }

    if ((m_container_width != m_decoder->image->width) ||
            (m_container_height != m_decoder->image->height)) {
        qWarning("Decoded image sequence size (%dx%d) do not match declared container size (%dx%d)!\n",
                 m_decoder->image->width, m_decoder->image->height,
                 m_container_width, m_container_height);

        m_parseState = ParseAvifError;
        return false;
    }

    if (decode_one_frame()) {
        return true;
    } else {
        m_parseState = ParseAvifError;
        return false;
    }
}

int AVIFHandler::nextImageDelay() const
{
    if (!ensureParsed()) {
        return 0;
    }

    if (m_decoder->imageCount < 2) {
        return 0;
    }

    int delay_ms = 1000.0 * m_decoder->duration / m_decoder->imageCount;
    if (delay_ms < 1) {
        delay_ms = 1;
    }
    return delay_ms;
}

int AVIFHandler::loopCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

    if (m_decoder->imageCount < 2) {
        return 0;
    }

    return 1;
}