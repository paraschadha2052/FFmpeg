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
 *
 * Specification: https://fits.gsfc.nasa.gov/fits_standard.html Version 3.0
 *
 * Support all 2d images alongwith, bzero, bscale and blank keywords.
 * RGBA images are supported as NAXIS3 = 3 or 4 i.e. Planes in RGBA order. Also CTYPE = 'RGB ' should be present.
 * Also to interpret data, values are linearly scaled using min-max scaling but not RGB images.
 */

#include "avcodec.h"
#include "internal.h"
#include <float.h>
#include "libavutil/intreadwrite.h"
#include "libavutil/intfloat.h"
#include "libavutil/dict.h"

/**
 * Structure to store the header keywords in FITS file
 */
typedef struct FITSHeader {
    char simple;
    int bitpix;
    int64_t blank;
    int blank_found;
    int naxis;
    int naxisn[3];
    int rgb; /**< 1 if file contains RGB image, 0 otherwise */
    int xtension;
    double bscale;
    double bzero;
    double data_min;
    double data_max;
} FITSHeader;

/**
 * Calculate the data_min and data_max values from the data.
 * This is called if the values are not present in the header.
 * @param ptr8 pointer to the data
 * @param header pointer to the header
 * @return 0 if calculated successfully otherwise AVERROR_INVALIDDATA
 */
static int fill_data_min_max(const uint8_t * ptr8, FITSHeader * header, const uint8_t * end)
{
    uint8_t t8;
    int16_t t16;
    int32_t t32;
    int64_t t64;
    float tflt;
    double tdbl;
    int i, j;

    header->data_min = DBL_MAX;
    header->data_max = DBL_MIN;
    switch (header->bitpix) {
#define case_n(a, t, rd) \
    case a: \
        for (i = 0; i < header->naxisn[1]; i++) { \
                for (j = 0; j < header->naxisn[0]; j++) { \
                    t = rd; \
                    if (!header->blank_found || t != header->blank) { \
                        if (t > header->data_max) \
                            header->data_max = t; \
                        if (t < header->data_min) \
                            header->data_min = t; \
                    } \
                    ptr8 += abs(a)/8; \
                } \
            } \
            break

        case_n(-64, tdbl, av_int2double(AV_RB64(ptr8)));
        case_n(-32, tflt, av_int2float(AV_RB32(ptr8)));
        case_n(8, t8, ptr8[0]);
        case_n(16, t16, AV_RB16(ptr8));
        case_n(32, t32, AV_RB32(ptr8));
        case_n(64, t64, AV_RB64(ptr8));
        default:
            return AVERROR_INVALIDDATA;
    }
    return 0;
}

/**
 * Read the fits header and store the values in FITSHeader pointed by header
 * @param avctx AVCodec context
 * @param ptr pointer to pointer to the data
 * @param header pointer to the FITSHeader
 * @param end pointer to end of packet
 * @return 0 if calculated successfully otherwise AVERROR_INVALIDDATA
 */
static int fits_read_header(AVCodecContext *avctx, const uint8_t **ptr, FITSHeader * header,
                            const uint8_t * end, AVDictionary **meta)
{
    const uint8_t *ptr8 = *ptr;
    int lines_read = 0, i, dim_no, t, data_min_found = 0, data_max_found = 0, ret;
    uint64_t size=1;
    double d;
    AVDictionary *metadata = NULL;
    char keyword[10], value[72];

    header->blank_found = 0;
    header->bscale = 1.0;
    header->bzero = 0;
    header->rgb = 0;

    if (end - ptr8 < 80)
        return AVERROR_INVALIDDATA;

    if (sscanf(ptr8, "SIMPLE = %c", &header->simple) == 1) {
        if (header->simple == 'F') {
            av_log(avctx, AV_LOG_WARNING, "not a standard FITS file\n");
            av_dict_set(&metadata, "SIMPLE", "F", 0);
        } else if (header->simple != 'T') {
            av_log(avctx, AV_LOG_ERROR, "invalid SIMPLE value, SIMPLE = %c\n", header->simple);
            return AVERROR_INVALIDDATA;
        } else {
            av_dict_set(&metadata, "SIMPLE", "T", 0);
        }
        header->xtension = 0;
    } else if (!strncmp(ptr8, "XTENSION= 'IMAGE", 16)) {
        header->xtension = 1;
        av_dict_set(&metadata, "XTENSION", "'IMAGE   '", 0);
    } else {
        av_log(avctx, AV_LOG_ERROR, "missing SIMPLE keyword or invalid XTENSION\n");
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

    av_dict_set_int(&metadata, "BITPIX", header->bitpix, 0);
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

    av_dict_set_int(&metadata, "NAXIS", header->naxis, 0);
    ptr8 += 80;
    lines_read++;

    for (i = 0; i < header->naxis; i++) {
        if (end - ptr8 < 80)
            return AVERROR_INVALIDDATA;

        if (sscanf(ptr8, "NAXIS%d = %d", &dim_no, &header->naxisn[i]) != 2 || dim_no != i+1) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword\n", i+1);
            return AVERROR_INVALIDDATA;
        }

        ret = snprintf(keyword, 10, "NAXIS%d", dim_no);
        if (ret < 0 || ret >= 10) {
            return AVERROR_INVALIDDATA;
        }

        av_dict_set_int(&metadata, keyword, header->naxisn[i], 0);
        size *= header->naxisn[i];
        ptr8 += 80;
        lines_read++;
    }

    if (end - ptr8 < 80)
        return AVERROR_INVALIDDATA;

    while (strncmp(ptr8, "END", 3)) {
        if (sscanf(ptr8, "BLANK = %d", &t) == 1) {
            header->blank = t;
            header->blank_found = 1;
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

        if (ptr8[8] == '=') {
            for (i = 0; i < 8 && ptr8[i] != ' '; i++) {
                keyword[i] = ptr8[i];
            }
            keyword[i] = '\0';

            t = 0;
            i = 10;
            while (i < 80 && ptr8[i] == ' ')
                i++;

            if (i < 80) {
                value[t] = ptr8[i];
                i++;
                t++;
                if (ptr8[i-1] == '\'') {
                    while (i < 80 && ptr8[i] != '\'') {
                        value[t] = ptr8[i];
                        i++;
                        t++;
                    }
                    value[t] = '\'';
                    t++;
                } else if (ptr8[i-1] == '(') {
                    while (i < 80 && ptr8[i] != ')') {
                        value[t] = ptr8[i];
                        i++;
                        t++;
                    }
                    value[t] = ')';
                    t++;
                } else {
                    while (i < 80 && ptr8[i] != ' ' && ptr8[i] != '/') {
                        value[t] = ptr8[i];
                        i++;
                        t++;
                    }
                }
            }

            value[t] = '\0';
            av_dict_set(&metadata, keyword, value, 0);
        }

        ptr8 += 80;
        lines_read++;

        if (end - ptr8 < 80)
            return AVERROR_INVALIDDATA;
    }

    if (!header->rgb && header->naxis != 2) {
        av_log(avctx, AV_LOG_ERROR, "unsupported number of dimensions, NAXIS = %d\n", header->naxis);
        return AVERROR_INVALIDDATA;
    }

    if (header->blank_found && (header->bitpix == -32 || header->bitpix == -64)) {
        av_log(avctx, AV_LOG_WARNING, "BLANK keyword found but BITPIX = %d\n. Ignoring BLANK", header->bitpix);
        header->blank_found = 0;
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

    *meta = metadata;
    return 0;
}

static int fits_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    AVFrame *p=data;
    const uint8_t *ptr8 = avpkt->data, *end;
    uint8_t t8;
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
    FITSHeader header;

    end = ptr8 + avpkt->size;
    if ((ret = fits_read_header(avctx, &ptr8, &header, end, &p->metadata)) < 0)
        return ret;

    size = (header.naxisn[0]) * (header.naxisn[1]);

    if (header.rgb) {
        if (header.bitpix == 8) {
            avctx->pix_fmt = AV_PIX_FMT_RGB32;
        } else if (header.bitpix == 16) {
            avctx->pix_fmt = AV_PIX_FMT_RGBA64;
        } else {
            av_log(avctx, AV_LOG_ERROR, "unsupported BITPIX = %d\n", header.bitpix);
            return AVERROR_INVALIDDATA;
        }
    } else {
        if (header.bitpix == 8) {
            avctx->pix_fmt = AV_PIX_FMT_GRAY8;
        } else {
            avctx->pix_fmt = AV_PIX_FMT_GRAY16;
        }
    }

    if ((ret = ff_set_dimensions(avctx, header.naxisn[0], header.naxisn[1])) < 0)
        return ret;

    if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
        return ret;

    if (header.rgb) {
        if (header.bitpix == 8) {
            for (i = 0; i < avctx->height; i++) {
                /*
                 * FITS stores images with bottom row first. Therefore we have
                 * to fill the image from bottom to top.
                 */
                dst32 = (uint32_t *)(p->data[0] + (avctx->height-i-1)* p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    if (header.naxisn[2] == 4) {
                        t = ptr8[size * 3];
                        if (!header.blank_found || t != header.blank) {
                            t = t * header.bscale + header.bzero;
                        } else {
                            t = 0;
                        }
                        a = t << 24;
                    } else {
                        a = (255 << 24);
                    }

                    t = ptr8[0];
                    if (!header.blank_found || t != header.blank) {
                        t = t * header.bscale + header.bzero;
                    } else {
                        t = 0;
                    }
                    r = t << 16;

                    t = ptr8[size];
                    if (!header.blank_found || t != header.blank) {
                        t = t * header.bscale + header.bzero;
                    } else {
                        t = 0;
                    }
                    g = t << 8;

                    t = ptr8[size * 2];
                    if (!header.blank_found || t != header.blank) {
                        t = t * header.bscale + header.bzero;
                    } else {
                        t = 0;
                    }
                    b = t;

                    *dst32++ = ((uint32_t)a) | ((uint32_t)r) | ((uint32_t)g) | ((uint32_t)b);
                    ptr8++;
                }
            }
        } else if (header.bitpix == 16) {
            // not tested ....
            for (i = 0; i < avctx->height; i++) {
                dst64 = (uint64_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {

                    if (header.naxisn[2] == 4) {
                        t = ((ptr8[size * 3] << 8) | ptr8[size * 3 + 1]);
                        if (!header.blank_found || t != header.blank) {
                            t = t * header.bscale + header.bzero;
                        } else {
                            t = 0;
                        }
                        a = t << 48;
                    } else {
                        a = 65535ULL << 48;
                    }

                    t = ptr8[0] << 8 | ptr8[1];
                    if (!header.blank_found || t != header.blank) {
                        t = t * header.bscale + header.bzero;
                    } else {
                        t = 0;
                    }
                    r = t << 32;

                    t = ptr8[size] << 8 | ptr8[size + 1];
                    if (!header.blank_found || t != header.blank) {
                        t = t * header.bscale + header.bzero;
                    } else {
                        t = 0;
                    }
                    g = t << 16;

                    t = ptr8[size * 2] << 8 | ptr8[size * 2 + 1];
                    if (!header.blank_found || t != header.blank) {
                        t = t * header.bscale + header.bzero;
                    } else {
                        t = 0;
                    }
                    b = t;

                    *dst64++ = a | r | g | b;
                    ptr8 += 2;
                }
            }
        }
    } else {
        if (header.bitpix == 8) {
            for (i = 0; i < avctx->height; i++) {
                dst8 = (uint8_t *) (p->data[0] + (avctx->height-i-1)* p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t8 = ptr8[0];
                    if (!header.blank_found || t8 != header.blank) {
                        t8 = ((t8 - header.data_min) * 255) / (header.data_max - header.data_min);
                    } else {
                        t8 = 0;
                    }
                    *dst8++ = t8;
                    ptr8 += 1;
                }
            }
        } else if (header.bitpix == 16) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t16 = AV_RB16(ptr8);
                    if (!header.blank_found || t16 != header.blank) {
                        t16 = ((t16 - header.data_min) * 65535) / (header.data_max - header.data_min);
                    } else {
                        t16 = 0;
                    }
                    *dst16++ = t16;
                    ptr8 += 2;
                }
            }
        } else if (header.bitpix == 32) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t32 = AV_RB32(ptr8);
                    if (!header.blank_found || t32 != header.blank) {
                        t16 = ((t32 - header.data_min) * 65535) / (header.data_max - header.data_min);
                    } else {
                        t16 = 0;
                    }
                    *dst16++ = t16;
                    ptr8 += 4;
                }
            }
        } else if (header.bitpix == 64) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    t64 = AV_RB64(ptr8);
                    if (!header.blank_found || t64 != header.blank) {
                        t16 = ((t64 - header.data_min) * 65535) / (header.data_max - header.data_min);
                    } else {
                        t16 = 0;
                    }
                    *dst16++ = t16;
                    ptr8 += 8;
                }
            }
        } else if (header.bitpix == -32) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    tflt = av_int2float(AV_RB32(ptr8));
                    if (!header.blank_found || tflt != header.blank) {
                        tflt = ((tflt - header.data_min) * 65535) / (header.data_max - header.data_min);
                    } else {
                        tflt = 0;
                    }
                    *dst16++ = tflt;
                    ptr8 += 4;
                }
            }
        } else if (header.bitpix == -64) {
            for (i = 0; i < avctx->height; i++) {
                dst16 = (uint16_t *)(p->data[0] + (avctx->height-i-1) * p->linesize[0]);
                for (j = 0; j < avctx->width; j++) {
                    tdbl = av_int2double(AV_RB64(ptr8));
                    if (!header.blank_found || tdbl != header.blank) {
                        tdbl = ((tdbl - header.data_min) * 65535) / (header.data_max - header.data_min);
                    } else {
                        tdbl = 0;
                    }
                    *dst16++ = tdbl;
                    ptr8 += 8;
                }
            }
        } else {
            av_log(avctx, AV_LOG_ERROR, "invalid BITPIX, %d\n", header.bitpix);
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
    .decode         = fits_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("Flexible Image Transport System")
};
