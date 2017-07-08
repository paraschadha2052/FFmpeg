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

static int write_keyword_value(char * header, const char * keyword, int value)
{
    int i, len, ret;
    len = strlen(keyword);
    for (i = 0; i < len; i++) {
        header[i] = keyword[i];
    }
    for (; i < 8; i++) {
        header[i] = ' ';
    }
    header[8] = '=';
    header[9] = ' ';
    ret = snprintf(header + 10, 70, "%d", value);
    for (i = ret + 10; i < 80; i++) {
        header[i] = ' ';
    }
    return 0;
}

static int fits_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *pict, int *got_packet)
{
    AVFrame * const p = (AVFrame *)pict;
    FITSContext * fitsctx = avctx->priv_data;
    PutByteContext pbc;
    uint8_t *ptr;
    uint64_t header_size = 2880, data_size = 0, padded_data_size = 0, lines_written = 0;
    int ret, bitpix, naxis, naxis3 = 1, bzero = 0, i, j, k, t, rgb = 0;
    char header_line[80];

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
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_RGBA:
            bitpix = 8;
            naxis = 3;
            rgb = 1;
            if (avctx->pix_fmt == AV_PIX_FMT_RGB24) {
                naxis3 = 3;
            } else {
                naxis3 = 4;
            }
            break;
        case AV_PIX_FMT_RGB48BE:
        case AV_PIX_FMT_RGBA64BE:
            bitpix = 16;
            naxis = 3;
            bzero = 32768;
            rgb = 1;
            if (avctx->pix_fmt == AV_PIX_FMT_RGB48BE) {
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

    bytestream2_init_writer(&pbc, pkt->data, pkt->size);

    if (fitsctx->first_image) {
        strncpy(header_line, "SIMPLE  = ", 10);
        for (i = 10; i < 80; i++) {
            header_line[i] = ' ';
        }
        header_line[29] = 'T';
    } else {
        strncpy(header_line, "XTENSION= 'IMAGE   '", 20);
        for (i = 20; i < 80; i++) {
            header_line[i] = ' ';
        }
    }
    bytestream2_put_buffer(&pbc, header_line, 80);
    lines_written++;

    write_keyword_value(header_line, "BITPIX", bitpix);
    bytestream2_put_buffer(&pbc, header_line, 80);
    lines_written++;

    write_keyword_value(header_line, "NAXIS", naxis);
    bytestream2_put_buffer(&pbc, header_line, 80);
    lines_written++;

    write_keyword_value(header_line, "NAXIS1", avctx->width);
    bytestream2_put_buffer(&pbc, header_line, 80);
    lines_written++;

    write_keyword_value(header_line, "NAXIS2", avctx->height);
    bytestream2_put_buffer(&pbc, header_line, 80);
    lines_written++;

    if (rgb) {
        write_keyword_value(header_line, "NAXIS3", naxis3);
        bytestream2_put_buffer(&pbc, header_line, 80);
        lines_written++;
    }

    if (!fitsctx->first_image) {
        write_keyword_value(header_line, "PCOUNT", 0);
        bytestream2_put_buffer(&pbc, header_line, 80);
        lines_written++;

        write_keyword_value(header_line, "GCOUNT", 1);
        bytestream2_put_buffer(&pbc, header_line, 80);
        lines_written++;
    } else {
        fitsctx->first_image = 0;
    }

    if (bitpix == 16) {
        write_keyword_value(header_line, "BZERO", bzero);
        bytestream2_put_buffer(&pbc, header_line, 80);
        lines_written++;
    }

    if (rgb) {
        strncpy(header_line, "CTYPE3  = 'RGB     '", 20);
        for (i = 20; i < 80; i++) {
            header_line[i] = ' ';
        }
        bytestream2_put_buffer(&pbc, header_line, 80);
        lines_written++;
    }

    strncpy(header_line, "END", 3);
    for (i = 3; i < 80; i++) {
        header_line[i] = ' ';
    }
    bytestream2_put_buffer(&pbc, header_line, 80);
    lines_written++;

    t = 36 - lines_written;

    for (i = 0; i < 80; i++) {
        header_line[i] = ' ';
    }
    while (t--) {
        bytestream2_put_buffer(&pbc, header_line, 80);
    }

    if (rgb) {
        switch (avctx->pix_fmt) {
        #define case_n(cas, dref) \
            case cas: \
                for (k = 0; k < naxis3; k++) { \
                    for (i = 0; i < avctx->height; i++) { \
                        ptr = p->data[0] + (avctx->height - i - 1) * p->linesize[0] + k; \
                        for (j = 0; j < avctx->width; j++) { \
                            bytestream2_put_byte(&pbc, dref(ptr) - bzero); \
                            ptr += naxis3; \
                        } \
                    } \
                } \
                break
            case_n(AV_PIX_FMT_RGB24, *);
            case_n(AV_PIX_FMT_RGBA, *);
            case_n(AV_PIX_FMT_RGB48BE, AV_RB16);
            case_n(AV_PIX_FMT_RGBA64BE, AV_RB16);
        }
    } else {
        for (i = 0; i < avctx->height; i++) {
            ptr = p->data[0] + (avctx->height - i - 1) * p->linesize[0];
            if (bitpix == 16) {
                for (j = 0; j < avctx->width; j++) {
                    bytestream2_put_be16(&pbc, AV_RB16(ptr) - bzero);
                    ptr += 2;
                }
            } else {
                bytestream2_put_buffer(&pbc, ptr, avctx->width);
            }
        }
    }

    t = padded_data_size - data_size;
    while (t--) {
        bytestream2_put_byte(&pbc, 0);
    }

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
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_RGBA64BE,
                                                 AV_PIX_FMT_RGB48BE,
                                                 AV_PIX_FMT_RGBA,
                                                 AV_PIX_FMT_RGB24,
                                                 AV_PIX_FMT_GRAY16BE,
                                                 AV_PIX_FMT_GRAY8,
                                                 AV_PIX_FMT_NONE },
};
