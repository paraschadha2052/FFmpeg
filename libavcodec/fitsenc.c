/*
 * FITS image encoder
 * Copyright (c) 2017 Paras Chadha
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * FITS image encoder
 *
 * Specification: https://fits.gsfc.nasa.gov/fits_standard.html Version 3.0
 *
 * RGBA images are encoded as planes in RGBA order. So, NAXIS3 is 3 or 4 for them.
 * Also CTYPE3 = 'RGB ' is added to the header to distinguish them from 3d images.
 */

#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"

typedef struct FITSContext {
    int first_image;
} FITSContext;

static av_cold int fits_encode_init(AVCodecContext *avctx)
{
    FITSContext * fitsctx = avctx->priv_data;
    fitsctx->first_image = 1;
    return 0;
}

static int write_keyword_value(uint8_t **bytestream, const char *keyword, int value)
{
    int len, ret;
    uint8_t *header = *bytestream;
    len = strlen(keyword);

    memset(header, ' ', 80);
    memcpy(header, keyword, len);
    header[8] = '=';
    header[9] = ' ';
    header += 10;
    ret = snprintf(header, 70, "%d", value);
    header[ret] = ' ';

    *bytestream += 80;
    return 0;
}

static int fits_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *pict, int *got_packet)
{
    AVFrame * const p = (AVFrame *)pict;
    FITSContext *fitsctx = avctx->priv_data;
    uint8_t *bytestream, *bytestream_start, *ptr;
    uint64_t header_size = 2880, data_size = 0, padded_data_size = 0;
    int ret, bitpix, naxis, naxis3 = 1, bzero = 0, i, j, k, t, rgb = 0;
    static const int map[] = {2, 0, 1, 3}; // mapping from GBRA -> RGBA as RGBA is to be stored in FITS file..

    switch (avctx->pix_fmt) {
        case AV_PIX_FMT_GRAY8:
            bitpix = 8;
            naxis = 2;
            break;
        case AV_PIX_FMT_GRAY16BE:
            bitpix = 16;
            naxis = 2;
            bzero = 32768;
            break;
        case AV_PIX_FMT_GBRP:
        case AV_PIX_FMT_GBRAP:
            bitpix = 8;
            naxis = 3;
            rgb = 1;
            if (avctx->pix_fmt == AV_PIX_FMT_GBRP) {
                naxis3 = 3;
            } else {
                naxis3 = 4;
            }
            break;
        case AV_PIX_FMT_GBRP16BE:
        case AV_PIX_FMT_GBRAP16BE:
            bitpix = 16;
            naxis = 3;
            bzero = 32768;
            rgb = 1;
            if (avctx->pix_fmt == AV_PIX_FMT_GBRP16BE) {
                naxis3 = 3;
            } else {
                naxis3 = 4;
            }
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "unsupported pixel format\n");
            return AVERROR(EINVAL);
    }

    data_size = (bitpix >> 3) * avctx->height * avctx->width * naxis3;
    padded_data_size = ((data_size + 2879) / 2880 ) * 2880;

    if ((ret = ff_alloc_packet2(avctx, pkt, header_size + padded_data_size, 0)) < 0)
        return ret;

    bytestream_start =
    bytestream       = pkt->data;

    if (fitsctx->first_image) {
        memcpy(bytestream, "SIMPLE  = ", 10);
        memset(bytestream + 10, ' ', 70);
        bytestream[29] = 'T';
    } else {
        memcpy(bytestream, "XTENSION= 'IMAGE   '", 20);
        memset(bytestream + 20, ' ', 60);
    }
    bytestream += 80;

    write_keyword_value(&bytestream, "BITPIX", bitpix);         // no of bits per pixel
    write_keyword_value(&bytestream, "NAXIS", naxis);           // no of dimensions of image
    write_keyword_value(&bytestream, "NAXIS1", avctx->width);   // first dimension i.e. width
    write_keyword_value(&bytestream, "NAXIS2", avctx->height);  // second dimension i.e. height

    if (rgb)
        write_keyword_value(&bytestream, "NAXIS3", naxis3);     // third dimension to store RGBA planes

    if (!fitsctx->first_image) {
        write_keyword_value(&bytestream, "PCOUNT", 0);
        write_keyword_value(&bytestream, "GCOUNT", 1);
    } else {
        fitsctx->first_image = 0;
    }

    /*
     * Since FITS does not support unsigned 16 bit integers,
     * BZERO = 32768 is used to store unsigned 16 bit integers as
     * signed integers so that it can be read properly.
     */
    if (bitpix == 16)
        write_keyword_value(&bytestream, "BZERO", bzero);

    if (rgb) {
        memcpy(bytestream, "CTYPE3  = 'RGB     '", 20);
        memset(bytestream + 20, ' ', 60);
        bytestream += 80;
    }

    memcpy(bytestream, "END", 3);
    memset(bytestream + 3, ' ', 77);
    bytestream += 80;

    t = header_size - (bytestream - bytestream_start);
    memset(bytestream, ' ', t);
    bytestream += t;

    if (rgb) {
        switch (avctx->pix_fmt) {
            case AV_PIX_FMT_GBRP:
            case AV_PIX_FMT_GBRAP:
                for (k = 0; k < naxis3; k++) {
                    for (i = 0; i < avctx->height; i++) {
                        ptr = p->data[map[k]] + (avctx->height - i - 1) * p->linesize[map[k]];
                        memcpy(bytestream, ptr, avctx->width);
                        bytestream += avctx->width;
                    }
                }
                break;
            case AV_PIX_FMT_GBRP16BE:
            case AV_PIX_FMT_GBRAP16BE:
                for (k = 0; k < naxis3; k++) {
                    for (i = 0; i < avctx->height; i++) {
                        ptr = p->data[map[k]] + (avctx->height - i - 1) * p->linesize[map[k]];
                        for (j = 0; j < avctx->width; j++) {
                            bytestream_put_be16(&bytestream, AV_RB16(ptr) - bzero);
                            ptr += 2;
                        }
                    }
                }
                break;
        }
    } else {
        for (i = 0; i < avctx->height; i++) {
            ptr = p->data[0] + (avctx->height - i - 1) * p->linesize[0];
            if (bitpix == 16) {
                for (j = 0; j < avctx->width; j++) {
                    bytestream_put_be16(&bytestream, AV_RB16(ptr) - bzero);
                    ptr += 2;
                }
            } else {
                memcpy(bytestream, ptr, avctx->width);
                bytestream += avctx->width;
            }
        }
    }

    t = padded_data_size - data_size;
    memset(bytestream, 0, t);
    bytestream += t;

    pkt->size   = bytestream - bytestream_start;
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;

    return 0;
}

AVCodec ff_fits_encoder = {
    .name           = "fits",
    .long_name      = NULL_IF_CONFIG_SMALL("Flexible Image Transport System"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FITS,
    .priv_data_size = sizeof(FITSContext),
    .init           = fits_encode_init,
    .encode2        = fits_encode_frame,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_GBRAP16BE,
                                                 AV_PIX_FMT_GBRP16BE,
                                                 AV_PIX_FMT_GBRP,
                                                 AV_PIX_FMT_GBRAP,
                                                 AV_PIX_FMT_GBRP,
                                                 AV_PIX_FMT_GRAY16BE,
                                                 AV_PIX_FMT_GRAY8,
                                                 AV_PIX_FMT_NONE },
};
