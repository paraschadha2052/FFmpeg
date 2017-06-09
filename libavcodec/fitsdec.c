/*
 * FITS image decoder
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
 * FITS image decoder
 * It supports all 2-d images alongwith, bzero, bscale and blank keywords.
 * RGBA images are supported as NAXIS3 = 3 or 4 i.e. Planes in RGBA order. Also CTYPE = 'RGB ' should be present.
 * It currently does not support XTENSION keyword.
 * Also to interpret data, values are linearly scaled using min-max scaling but not RGB images.
 */

#include "avcodec.h"
#include "internal.h"
#include <float.h>
#include "libavutil/intreadwrite.h"

/**
 * Structure to store the header keywords in FITS file
 */
typedef struct FITSContext {
    char simple;
    int bitpix;
    int blank;
    int naxis;
    int naxisn[3];
    int rgb; /**< 1 if file contains RGB image, 0 otherwise */
    double bscale;
    double bzero;
    double data_min;
    double data_max;
} FITSDecContext;

/**
 * function calculates the data_min and data_max values from the data.
 * This is called if the values are not present in the header.
 * @param ptr8 - pointer to the data
 * @param header - pointer to the header
 * @return 1, if calculated successfully, otherwise AVERROR_INVALIDDATA
 */
static int fill_data_min_max(const uint8_t * ptr8, FITSDecContext * header, const uint8_t * end)
{
    int16_t t16;
    int32_t t32;
    int64_t t64;
    float tflt;
    double tdbl;
    int i, j;

    header->data_min = DBL_MAX;
    header->data_max = DBL_MIN;
    switch (header->bitpix) {
        case -64:
            t64 = AV_RB64(ptr8);
            memcpy(&tdbl, &t64, 8);
            for (i = 0; i < header->naxisn[1]; i++) {
                for (j = 0; j < header->naxisn[0]; j++) {
                    t64 = AV_RB64(ptr8);
                    memcpy(&tdbl, &t64, 8);
                    if (tdbl > header->data_max)
                        header->data_max = tdbl;
                    if (tdbl < header->data_min)
                        header->data_min = tdbl;
                    ptr8 += 8;
                }
            }
            return 1;
        case -32:
            t32 = AV_RB32(ptr8);
            memcpy(&tflt, &t32, 4);
            for (i = 0; i < header->naxisn[1]; i++) {
                for (j = 0; j < header->naxisn[0]; j++) {
                    t32 = AV_RB32(ptr8);
                    memcpy(&tflt, &t32, 4);
                    if (tflt > header->data_max)
                        header->data_max = tflt;
                    if (tflt < header->data_min)
                        header->data_min = tflt;
                    ptr8 += 4;
                }
            }
            return 1;
        case 8:
            for (i = 0; i < header->naxisn[1]; i++) {
                for (j = 0; j < header->naxisn[0]; j++) {
                    if (ptr8[0] != header->blank) {
                        if (ptr8[0] > header->data_max)
                            header->data_max = ptr8[0];
                        if (ptr8[0] < header->data_min)
                            header->data_min = ptr8[0];
                    }
                    ptr8++;
                }
            }
            return 1;
        case 16:
            t16 = AV_RB16(ptr8);
            for (i = 0; i < header->naxisn[1]; i++) {
                for (j = 0; j < header->naxisn[0]; j++) {
                    t16 = AV_RB16(ptr8);
                    if (t16 != header->blank) {
                        if (t16 > header->data_max)
                            header->data_max = t16;
                        if (t16 < header->data_min)
                            header->data_min = t16;
                    }
                    ptr8 += 2;
                }
            }
            return 1;
        case 32:
            t32 = AV_RB32(ptr8);
            for (i = 0; i < header->naxisn[1]; i++) {
                for (j = 0; j < header->naxisn[0]; j++) {
                    t32 = AV_RB32(ptr8);
                    if (t32 != header->blank) {
                        if (t32 > header->data_max)
                            header->data_max = t32;
                        if (t32 < header->data_min)
                            header->data_min = t32;
                    }
                    ptr8 += 4;
                }
            }
            return 1;
        case 64:
            t64 = AV_RB64(ptr8);
            for (i = 0; i < header->naxisn[1]; i++) {
                for (j = 0; j < header->naxisn[0]; j++) {
                    t64 = AV_RB64(ptr8);
                    if (t64 != header->blank) {
                        if (t64 > header->data_max)
                            header->data_max = t64;
                        if (t64 < header->data_min)
                            header->data_min = t64;
                    }
                    ptr8 += 8;
                }
            }
            return 1;
        default:
            return AVERROR_INVALIDDATA;
    }
    return 1;
}

/**
 * function reads the fits header and stores the values in FITSDecContext pointed by header
 * @param avctx - AVCodec context
 * @param ptr - pointer to pointer to the data
 * @param header - pointer to the FITSDecContext
 * @param end - pointer to end of packet
 * @return 1, if calculated successfully, otherwise AVERROR_INVALIDDATA
 */
static int fits_read_header(AVCodecContext *avctx, const uint8_t **ptr, FITSDecContext * header, const uint8_t * end)
{
    const uint8_t *ptr8 = *ptr;
    int lines_read = 0, i, dim_no, t, data_min_found = 0, data_max_found = 0, ret;
    uint64_t size=1;
    double d;

    header->blank = 0;
    header->bscale = 1.0;
    header->bzero = 0;
    header->rgb = 0;

    if (end - ptr8 < 80)
        return AVERROR_INVALIDDATA;

    if (sscanf(ptr8, "SIMPLE = %c", &header->simple) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing SIMPLE keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (header->simple == 'F') {
        av_log(avctx, AV_LOG_WARNING, "not a standard FITS file\n");
    } else if (header->simple != 'T') {
        av_log(avctx, AV_LOG_ERROR, "invalid SIMPLE value, SIMPLE = %c\n", header->simple);
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;

    if (end - ptr8 < 80)
        return AVERROR_INVALIDDATA;

    if (sscanf(ptr8, "BITPIX = %d", &header->bitpix) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing BITPIX keyword\n");
        return AVERROR_INVALIDDATA;
    }

    size = abs(header->bitpix) >> 3;
    ptr8 += 80;
    lines_read++;

    if (end - ptr8 < 80)
        return AVERROR_INVALIDDATA;

    if (sscanf(ptr8, "NAXIS = %d", &header->naxis) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing NAXIS keyword\n");
        return AVERROR_INVALIDDATA;
    }

    if (!header->naxis) {
        av_log(avctx, AV_LOG_ERROR, "No image data found, NAXIS = %d\n", header->naxis);
        return AVERROR_INVALIDDATA;
    }

    if (header->naxis != 2 && header->naxis != 3) {
        av_log(avctx, AV_LOG_ERROR, "unsupported number of dimensions, NAXIS = %d\n", header->naxis);
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;

    for (i = 0; i < header->naxis; i++) {
        if (end - ptr8 < 80)
            return AVERROR_INVALIDDATA;

        if (sscanf(ptr8, "NAXIS%d = %d", &dim_no, &header->naxisn[i]) != 2 || dim_no != i+1) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword\n", i+1);
            return AVERROR_INVALIDDATA;
        }

        size *= header->naxisn[i];
        ptr8 += 80;
        lines_read++;
    }

    if (end - ptr8 < 80)
            return AVERROR_INVALIDDATA;

    while (strncmp(ptr8, "END", 3)) {
        if (sscanf(ptr8, "BLANK = %d", &t) == 1) {
            header->blank = t;
        } else if (sscanf(ptr8, "BSCALE = %lf", &d) == 1) {
            header->bscale = d;
        } else if (sscanf(ptr8, "BZERO = %lf", &d) == 1) {
            header->bzero = d;
        } else if (sscanf(ptr8, "DATAMAX = %lf", &d) == 1) {
            data_max_found = 1;
            header->data_max = d;
        } else if (sscanf(ptr8, "DATAMIN = %lf", &d) == 1) {
            data_min_found = 1;
            header->data_min = d;
        } else if (!strncmp(ptr8, "CTYPE3  = 'RGB", 14)) {
            header->rgb = 1;
            if (header->naxis != 3 || (header->naxisn[2] != 3 && header->naxisn[2] != 4)) {
                av_log(avctx, AV_LOG_ERROR, "File contains RGB image but NAXIS = %d and NAXIS3 = %d\n", header->naxis, header->naxisn[2]);
                return AVERROR_INVALIDDATA;
            }
        }
        ptr8 += 80;
        lines_read++;

        if (end - ptr8 < 80)
            return AVERROR_INVALIDDATA;
    }

    if (!header->rgb && header->naxis != 2){
        av_log(avctx, AV_LOG_ERROR, "unsupported number of dimensions, NAXIS = %d\n", header->naxis);
        return AVERROR_INVALIDDATA;
    }

    ptr8 += 80;
    lines_read++;
    lines_read %= 36;

    t = ((36 - lines_read) % 36) * 80;
    if (end - ptr8 < t)
        return AVERROR_INVALIDDATA;
    ptr8 += t;
    *ptr = ptr8;

    if (end - ptr8 < size)
        return AVERROR_INVALIDDATA;

    if (!header->rgb && (!data_min_found || !data_max_found)) {
        if ((ret = fill_data_min_max(ptr8, header, end)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "invalid BITPIX, %d\n", header->bitpix);
            return AVERROR_INVALIDDATA;
        }
    } else {
        /*
         * instead of applying bscale and bzero to every element, we can do inverse transformation on data_min and
         * data_max
         */
        header->data_min = (header->data_min - header->bzero) / header->bscale;
        header->data_max = (header->data_max - header->bzero) / header->bscale;
    }
    return 1;
}

static int fits_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    AVFrame *p=data;
    const uint8_t *ptr8 = avpkt->data, *end;
    int16_t t16;
    int32_t t32;
    int64_t t64;
    float   tflt;
    double  tdbl;
    int ret, i, j;
    uint8_t *dst8;
    uint16_t *dst16;
    uint32_t *dst32;
    uint64_t *dst64, size, r, g, b, a, t;
    FITSDecContext * header = avctx->priv_data;

    end = ptr8 + avpkt->size;
    if (ret = fits_read_header(avctx, &ptr8, header, end) < 0)
        return ret;

    size = (header->naxisn[0]) * (header->naxisn[1]);

    if (header->rgb) {
        if (header->bitpix == 8) {
            avctx->pix_fmt = AV_PIX_FMT_RGB32;
        } else if (header->bitpix == 16) {
            avctx->pix_fmt = AV_PIX_FMT_RGBA64;
        } else {
            av_log(avctx, AV_LOG_ERROR, "unsupported BITPIX = %d\n", header->bitpix);
            return AVERROR_INVALIDDATA;
        }
    } else {
        if (header->bitpix == 8) {
            avctx->pix_fmt = AV_PIX_FMT_GRAY8;
        } else {
            avctx->pix_fmt = AV_PIX_FMT_GRAY16;
        }
    }

    if ((ret = ff_set_dimensions(avctx, header->naxisn[0], header->naxisn[1])) < 0)
        return ret;

    if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
        return ret;

    if (header->rgb) {
        if (header->bitpix == 8) {
            for (i = 0; i < avctx->height; i++) {
                /*
                 * FITS stores images with bottom row first. Therefore we have
                 * to fill the image from bottom to top.
                 */
                dst32 = (uint32_t *)(p->data[0] + (avctx->height-i-1)* p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    if (header->naxisn[2] == 4) {
                        if (ptr8[size * 3] != header->blank)
                            t = ptr8[size * 3] * header->bscale + header->bzero;
                        a = t << 24;
                    } else {
                        a = (255 << 24);
                    }

                    if (ptr8[0] != header->blank)
                        t = ptr8[0] * header->bscale + header->bzero;
                    r = t << 16;

                    if (ptr8[size] != header->blank)
                        t = ptr8[size] * header->bscale + header->bzero;
                    g = t << 8;

                    if (ptr8[size * 2] != header->blank)
                        t = ptr8[size * 2] * header->bscale + header->bzero;
                    b = t;

                    *dst32++ = ((uint32_t)a) | ((uint32_t)r) | ((uint32_t)g) | ((uint32_t)b);
                    ptr8++;
                }
            }
        } else if (header->bitpix == 16) {
            // not tested ....
            for (i = 0; i < avctx->height; i++) {
                dst64 = (uint64_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {

                    if (header->naxisn[2] == 4) {
                        t = ((ptr8[size * 3] << 8) | ptr8[size * 3 + 1]);
                        if (t != header->blank)
                            t = t*header->bscale + header->bzero;
                        a = t << 48;
                    } else {
                        a = 65535ULL << 48;
                    }

                    t = ptr8[0] << 8 | ptr8[1];
                    if (t != header->blank)
                        t = t*header->bscale + header->bzero;
                    r = t << 32;

                    t = ptr8[size] << 8 | ptr8[size + 1];
                    if (t != header->blank)
                        t = t*header->bscale + header->bzero;
                    g = t << 16;

                    t = ptr8[size * 2] << 8 | ptr8[size * 2 + 1];
                    if (t != header->blank)
                        t = t*header->bscale + header->bzero;
                    b = t;

                    *dst64++ = a | r | g | b;
                    ptr8 += 2;
                }
            }
        }
    } else {
        if (header->bitpix == 8) {
            for (i = 0; i < avctx->height; i++) {
                dst8 = (uint8_t *) (p->data[0] + (avctx->height-i-1)* p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    if (ptr8[0] != header->blank) {
                        *dst8++ = ((ptr8[0] - header->data_min) * 255) / (header->data_max - header->data_min);
                    } else {
                        *dst8++ = ptr8[0];
                    }
                    ptr8++;
                }
            }
        } else if (header->bitpix == 16) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t16 = AV_RB16(ptr8);
                    if (t16 != header->blank)
                        t16 = ((t16 - header->data_min) * 65535) / (header->data_max - header->data_min);
                    *dst16++ = t16;
                    ptr8 += 2;
                }
            }
        } else if (header->bitpix == 32) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t32 = AV_RB32(ptr8);
                    if (t32 != header->blank)
                        t16 = ((t32 - header->data_min) * 65535) / (header->data_max - header->data_min);
                    *dst16++ = t16;
                    ptr8 += 4;
                }
            }
        } else if (header->bitpix == 64) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t64 = AV_RB64(ptr8);
                    if (t64 != header->blank)
                        t16 = ((t64 - header->data_min) * 65535) / (header->data_max - header->data_min);
                    *dst16++ = t16;
                    ptr8 += 8;
                }
            }
        } else if (header->bitpix == -32) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t32 = AV_RB32(ptr8);
                    memcpy(&tflt, &t32, 4);
                    *dst16++ = ((tflt - header->data_min) * 65535) / (header->data_max - header->data_min);
                    ptr8 += 4;
                }
            }
        } else if (header->bitpix == -64) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t64 = AV_RB64(ptr8);
                    memcpy(&tdbl, &t64, 8);
                    *dst16++ = ((tdbl - header->data_min) * 65535) / (header->data_max - header->data_min);
                    ptr8 += 8;
                }
            }
        } else {
            av_log(avctx, AV_LOG_ERROR, "invalid BITPIX, %d\n", header->bitpix);
            return AVERROR_INVALIDDATA;
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
    .priv_data_size = sizeof(FITSDecContext),
    .decode         = fits_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("FITS image")
};
