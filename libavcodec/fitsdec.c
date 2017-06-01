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
    const uint8_t *start, *ptr = avpkt->data;
    char simple;
    int bitpix, naxis, ret, i, j, data_min=INT_MAX, data_max=0, dim_size[999], temp, lines_read=0;
    uint16_t *dst, t;
    uint8_t *dst8;

    if (sscanf(ptr, "SIMPLE = %c", &simple) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing SIMPLE keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (simple == 'F') {
        av_log(avctx, AV_LOG_ERROR, "not a standard FITS file\n");
        return AVERROR_INVALIDDATA;
    }

    ptr += 80;
    lines_read++;

    if (sscanf(ptr, "BITPIX = %d", &bitpix) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing BITPIX keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (bitpix == 8)
        avctx->pix_fmt = AV_PIX_FMT_GRAY8;
    else if (bitpix == 16)
        avctx->pix_fmt = AV_PIX_FMT_GRAY16;
    else {
        av_log(avctx, AV_LOG_ERROR, "unsupported BITPIX, %d\n", bitpix);
        return AVERROR_INVALIDDATA;
    }

    ptr += 80;
    lines_read++;

    if(sscanf(ptr, "NAXIS = %d", &naxis) != 1){
        av_log(avctx, AV_LOG_ERROR, "missing NAXIS keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (naxis != 2) {
        av_log(avctx, AV_LOG_ERROR, "unsupported number of dimensions\n");
        return AVERROR_INVALIDDATA;
    }

    ptr += 80;
    lines_read++;

    for (i = 0; i < naxis; i++) {
        if (sscanf(ptr, "NAXIS%d = %d", &temp, &dim_size[i]) != 2) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword\n", i+1);
            return AVERROR_INVALIDDATA;
        }

        if (temp != i+1) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword\n", i+1);
            return AVERROR_INVALIDDATA;
        }

        ptr += 80;
        lines_read++;
    }

    avctx->width = dim_size[0];
    avctx->height = dim_size[1];

    while (strncmp(ptr, "END", 3)) {
        ptr += 80;
        lines_read++;
    }

    ptr += 80;
    lines_read++;
    lines_read %= 36;

    ptr += ((36 - lines_read) % 36) * 80;

    start = ptr;

    if ((ret = ff_set_dimensions(avctx, avctx->width, avctx->height)) < 0)
        return ret;

    if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
        return ret;

    if ( bitpix == 8 ) {
        for (i = 0; i < avctx->height; i++) {
            for (j = 0; j < avctx->width; j++) {
                if (ptr[0] > data_max)
                    data_max = ptr[0];
                if (ptr[0] < data_min)
                    data_min = ptr[0];
                ptr++;
            }
        }

        ptr = start;
        for (i = 0; i < avctx->height; i++) {
            dst8 = (uint8_t *)(p->data[0] + (avctx->height-i-1)* p->linesize[0]);
            for (j = 0; j < avctx->width; j++) {
                *dst8++ = ((ptr[0] - data_min) * 255) / (data_max - data_min);
                ptr++;
            }
        }
    }
    else if (bitpix == 16) {
        for (i = 0; i < avctx->height; i++) {
            for (j = 0; j < avctx->width; j++) {
                t = (ptr[0] << 8) | ptr[1];
                if (t > data_max)
                    data_max = t;
                if (t < data_min)
                    data_min = t;
                ptr += 2;
            }
        }

        ptr = start;
        for (i = 0; i < avctx->height; i++) {
            dst = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
            for (j = 0; j < avctx->width; j++) {
                t = ((((ptr[0] << 8) | ptr[1]) - data_min) * 65535) / (data_max - data_min);
                *dst++ = t;
                ptr += 2;
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
