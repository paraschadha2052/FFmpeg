/*
 * FITS image decoder
 *
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

#include "avcodec.h"
#include "internal.h"

static int fits_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    AVFrame *p=data;
    const uint8_t *start, *ptr8 = avpkt->data;
    const uint16_t *ptr16;
    char simple, str_val[80];
    int bitpix, naxis, ret, i, j, data_min=INT_MAX, data_max=0, dim_size[999], dim_no, lines_read=0, rgb=0;
    uint8_t *dst8;
    uint16_t *dst16, t;
    uint32_t *dst32;
    uint64_t *dst64, size;

    if (sscanf(ptr8, "SIMPLE = %c", &simple) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing SIMPLE keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (simple == 'F') {
        av_log(avctx, AV_LOG_ERROR, "not a standard FITS file\n");
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;

    if (sscanf(ptr8, "BITPIX = %d", &bitpix) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing BITPIX keyword\n");
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;

    if (sscanf(ptr8, "NAXIS = %d", &naxis) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing NAXIS keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (naxis != 2 && naxis != 3) {
        av_log(avctx, AV_LOG_ERROR, "unsupported number of dimensions\n");
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;

    for (i = 0; i < naxis; i++) {
        if (sscanf(ptr8, "NAXIS%d = %d", &dim_no, &dim_size[i]) != 2) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword\n", i+1);
            return AVERROR_INVALIDDATA;
        }

        if (dim_no != i+1) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword\n", i+1);
            return AVERROR_INVALIDDATA;
        }

        ptr8 += 80;
        lines_read++;
    }

    avctx->width = dim_size[0];
    avctx->height = dim_size[1];
    size = dim_size[0]*dim_size[1];

    while (strncmp(ptr8, "END", 3)) {
        if (sscanf(ptr8, "CTYPE3 = '%s '", str_val) == 1) {
            if (strncmp(str_val, "RGB", 3) == 0)
                rgb = 1;
        }
        ptr8 += 80;
        lines_read++;
    }

    if (rgb && (naxis != 3 || (dim_size[2] != 3 && dim_size[2] != 4))) {
        av_log(avctx, AV_LOG_ERROR, "File contains RGB image but NAXIS = %d and NAXIS3 = %d\n", naxis, dim_size[2]);
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;
    lines_read %= 36;
    ptr8 += ((36 - lines_read) % 36) * 80;
    start = ptr8;

    if (rgb) {
        if (bitpix == 8)
            avctx->pix_fmt = AV_PIX_FMT_RGB32;
        else if (bitpix == 16)
            avctx->pix_fmt = AV_PIX_FMT_RGBA64;
        else {
            av_log(avctx, AV_LOG_ERROR, "unsupported BITPIX, %d\n", bitpix);
            return AVERROR_INVALIDDATA;
        }
    }
    else {
        if (bitpix == 8)
            avctx->pix_fmt = AV_PIX_FMT_GRAY8;
        else if (bitpix == 16)
            avctx->pix_fmt = AV_PIX_FMT_GRAY16;
        else {
            av_log(avctx, AV_LOG_ERROR, "unsupported BITPIX, %d\n", bitpix);
            return AVERROR_INVALIDDATA;
        }
    }

    if ((ret = ff_set_dimensions(avctx, avctx->width, avctx->height)) < 0)
        return ret;

    if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
        return ret;

    if (rgb) {
        if (bitpix == 8) {
            for (i = 0; i < avctx->height; i++) {
                dst32 = (uint32_t *)(p->data[0] + (avctx->height-i-1)* p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    *dst32++ = (((dim_size[2] == 3) ? 255: ptr8[size*3]) << 24) | (ptr8[0] << 16) | (ptr8[size] << 8) | ptr8[size*2];
                    ptr8++;
                }
            }
        }
        else if (bitpix == 16) {
            ptr16 = (uint16_t *) ptr8;
            for (i = 0; i < avctx->height; i++) {
                dst64 = (uint64_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    *dst64++ = ((unsigned long) ((dim_size[2] == 3) ? 65535: ptr16[size*3]) << 48) | ((unsigned long) ptr16[0] << 32) | (ptr16[size] << 16) | ptr16[size*2];
                    ptr16++;
                }
            }
            ptr8 = (uint8_t *) ptr16;
        }
    }
    else {
        if (bitpix == 8) {
            for (i = 0; i < avctx->height; i++) {
                for (j = 0; j < avctx->width; j++) {
                    if (ptr8[0] > data_max)
                        data_max = ptr8[0];
                    if (ptr8[0] < data_min)
                        data_min = ptr8[0];
                    ptr8++;
                }
            }

            ptr8 = start;
            for (i = 0; i < avctx->height; i++) {
                  /* FITS stores images with bottom row first. Therefore we have
                     to fill the image from bottom to top. */
                dst8 = (uint8_t *) (p->data[0] + (avctx->height-i-1)* p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    *dst8++ = ((ptr8[0] - data_min) * 255) / (data_max - data_min);
                    ptr8++;
                }
            }
        }
        else if (bitpix == 16) {
            for (i = 0; i < avctx->height; i++) {
                for (j = 0; j < avctx->width; j++) {
                    t = (ptr8[0] << 8) | ptr8[1];
                    if (t > data_max)
                        data_max = t;
                    if (t < data_min)
                        data_min = t;
                    ptr8 += 2;
                }
            }

            ptr8 = start;
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t = ((((ptr8[0] << 8) | ptr8[1]) - data_min) * 65535) / (data_max - data_min);
                    *dst16++ = t;
                    ptr8 += 2;
                }
            }
        }
    }

    p->key_frame = 1;
    p->pict_type = AV_PICTURE_TYPE_I;

    *got_frame = 1;

    return avpkt->size;
}

AVCodec ff_fits_decoder = {
    .name           = "fits",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FITS,
    .decode         = fits_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("FITS image")
};
