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
 * Extract keyword, value from a header line (80 bytes) and store them in keyword and value strings respectively
 * @param ptr8 pointer to the data
 * @param keyword pointer to the char array in which keyword is to be stored
 * @param value pointer to the char array in which value is to be stored
 * @param end pointer to end of packet
 * @return 0 if calculated successfully otherwise AVERROR_INVALIDDATA
 */
static int read_keyword_value(const uint8_t * ptr8, char * keyword, char * value, const uint8_t * end)
{
    int i;

    if (end - ptr8 < 80)
        return AVERROR_INVALIDDATA;

    for (i = 0; i < 8 && ptr8[i] != ' '; i++) {
        keyword[i] = ptr8[i];
    }
    keyword[i] = '\0';

    if (ptr8[8] == '=') {
        i = 10;
        while (i < 80 && ptr8[i] == ' ') {
            i++;
        }

        if (i < 80) {
            *value++ = ptr8[i];
            i++;
            if (ptr8[i-1] == '\'') {
                for (; i < 80 && ptr8[i] != '\''; i++) {
                    *value++ = ptr8[i];
                }
                *value++ = '\'';
            } else if (ptr8[i-1] == '(') {
                for (; i < 80 && ptr8[i] != ')'; i++) {
                    *value++ = ptr8[i];
                }
                *value++ = ')';
            } else {
                for (; i < 80 && ptr8[i] != ' ' && ptr8[i] != '/'; i++) {
                    *value++ = ptr8[i];
                }
            }
        }
    }
    *value = '\0';
    return 0;
}

/**
 * Read the fits header and store the values in FITSHeader pointed by header
 * @param avctx AVCodec context
 * @param ptr pointer to pointer to the data
 * @param header pointer to the FITSHeader
 * @param end pointer to end of packet
 * @param meta pointer to pointer to AVDictionary to store metadata
 * @return 0 if calculated successfully otherwise AVERROR_INVALIDDATA
 */
static int fits_read_header(AVCodecContext *avctx, const uint8_t **ptr, FITSHeader * header,
                            const uint8_t * end, AVDictionary **meta)
{
    const uint8_t *ptr8 = *ptr;
    int lines_read = 0, i, dim_no, data_min_found = 0, data_max_found = 0, ret;
    int64_t t, size = 1;
    double d;
    AVDictionary *metadata = NULL;
    char keyword[10], value[72];

    header->blank_found = 0;
    header->bscale = 1.0;
    header->bzero = 0;
    header->rgb = 0;

    if ((ret = read_keyword_value(ptr8, keyword, value, end)) < 0)
        return ret;
    ptr8 += 80;
    lines_read++;

    if (!strncmp(keyword, "SIMPLE", 6)) {
        header->simple = value[0];
        if (header->simple == 'F') {
            av_log(avctx, AV_LOG_WARNING, "not a standard FITS file\n");
        } else if (header->simple != 'T') {
            av_log(avctx, AV_LOG_ERROR, "invalid SIMPLE value, SIMPLE = %c\n", header->simple);
            return AVERROR_INVALIDDATA;
        }
        header->xtension = 0;
    } else if (!strncmp(keyword, "XTENSION", 8) && !strncmp(value, "'IMAGE   '", 10)) {
        header->xtension = 1;
    } else {
        av_log(avctx, AV_LOG_ERROR, "missing SIMPLE keyword or invalid XTENSION\n");
        return AVERROR_INVALIDDATA;
    }

    av_dict_set(&metadata, keyword, value, 0);

    if ((ret = read_keyword_value(ptr8, keyword, value, end)) < 0)
        return ret;
    ptr8 += 80;
    lines_read++;

    if (!strncmp(keyword, "BITPIX", 6) && sscanf(value, "%d", &header->bitpix) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing BITPIX or invalid value of BITPIX, found %s = %s\n", keyword, value);
        return AVERROR_INVALIDDATA;
    }

    size = abs(header->bitpix) >> 3;
    av_dict_set(&metadata, keyword, value, 0);

    if ((ret = read_keyword_value(ptr8, keyword, value, end)) < 0)
        return ret;
    ptr8 += 80;
    lines_read++;

    if (!strncmp(keyword, "NAXIS", 5) && sscanf(value, "%d", &header->naxis) != 1) {
        av_log(avctx, AV_LOG_ERROR, "missing NAXIS or invalid value of NAXIS, found %s = %s\n", keyword, value);
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

    av_dict_set(&metadata, keyword, value, 0);

    for (i = 0; i < header->naxis; i++) {

        if ((ret = read_keyword_value(ptr8, keyword, value, end)) < 0)
            return ret;
        ptr8 += 80;
        lines_read++;

        if (sscanf(keyword, "NAXIS%d", &dim_no) != 1) {
            av_log(avctx, AV_LOG_ERROR, "missing NAXIS%d keyword", i+1);
            return AVERROR_INVALIDDATA;
        }

        if(dim_no != i+1) {
            av_log(avctx, AV_LOG_ERROR, "expected NAXIS%d keyword, found %s = %s\n", i+1, keyword, value);
            return AVERROR_INVALIDDATA;
        }

        if(sscanf(value, "%d", &header->naxisn[i]) != 1) {
            av_log(avctx, AV_LOG_ERROR, "invalid value of NAXIS%d = %s\n", i+1, value);
            return AVERROR_INVALIDDATA;
        }

        av_dict_set(&metadata, keyword, value, 0);
        size *= header->naxisn[i];

        if (size <= 0) {
            av_log(avctx, AV_LOG_ERROR, "unsupported size of FITS image");
            return AVERROR_INVALIDDATA;
        }
    }

    if ((ret = read_keyword_value(ptr8, keyword, value, end)) < 0)
        return ret;
    ptr8 += 80;
    lines_read++;

    while (strncmp(keyword, "END", 3)) {
        if (!strncmp(keyword, "BLANK", 5) && sscanf(value, "%ld", &t) == 1) {
            header->blank = t;
            header->blank_found = 1;
        } else if (!strncmp(keyword, "BSCALE", 6) && sscanf(value, "%lf", &d) == 1) {
            header->bscale = d;
        } else if (!strncmp(keyword, "BZERO", 5) && sscanf(value, "%lf", &d) == 1) {
            header->bzero = d;
        } else if (!strncmp(keyword, "DATAMAX", 7) && sscanf(value, "%lf", &d) == 1) {
            data_max_found = 1;
            header->data_max = d;
        } else if (!strncmp(keyword, "DATAMIN", 7) && sscanf(value, "%lf", &d) == 1) {
            data_min_found = 1;
            header->data_min = d;
        } else if (!strncmp(keyword, "CTYPE3", 6) && !strncmp(value, "'RGB", 4)) {
            header->rgb = 1;
            if (header->naxis != 3 || (header->naxisn[2] != 3 && header->naxisn[2] != 4)) {
                av_log(avctx, AV_LOG_ERROR, "File contains RGB image but NAXIS = %d and NAXIS3 = %d\n", header->naxis, header->naxisn[2]);
                return AVERROR_INVALIDDATA;
            }
        }

        av_dict_set(&metadata, keyword, value, 0);

        if ((ret = read_keyword_value(ptr8, keyword, value, end)) < 0)
            return ret;
        ptr8 += 80;
        lines_read++;
    }

    if (!header->rgb && header->naxis != 2) {
        av_log(avctx, AV_LOG_ERROR, "unsupported number of dimensions, NAXIS = %d\n", header->naxis);
        return AVERROR_INVALIDDATA;
    }

    if (header->blank_found && (header->bitpix == -32 || header->bitpix == -64)) {
        av_log(avctx, AV_LOG_WARNING, "BLANK keyword found but BITPIX = %d\n. Ignoring BLANK", header->bitpix);
        header->blank_found = 0;
    }

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
            return ret;
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

    /*
     * FITS stores images with bottom row first. Therefore we have
     * to fill the image from bottom to top.
     */
    if (header.rgb) {
        switch(header.bitpix) {
        #define case_rgb(cas, dst, type, dref) \
            case cas: \
                for (i = 0; i < avctx->height; i++) { \
                    dst = (type *) (p->data[0] + (avctx->height-i-1)* p->linesize[0]); \
                    for (j = 0; j < avctx->width; j++) { \
                        if (header.naxisn[2] == 4) { \
                            t = dref(ptr8 + size * 3); \
                            if (!header.blank_found || t != header.blank) { \
                                t = t * header.bscale + header.bzero; \
                            } else { \
                                t = 0; \
                            } \
                            a = t << (cas * 3); \
                        } else { \
                            a = (((1ULL << cas) - 1) << (cas * 3)); \
                        } \
                        t = dref(ptr8); \
                        if (!header.blank_found || t != header.blank) { \
                            t = t * header.bscale + header.bzero; \
                        } else { \
                            t = 0; \
                        } \
                        r = t << (cas * 2); \
                        t = dref(ptr8 + size); \
                        if (!header.blank_found || t != header.blank) { \
                            t = t * header.bscale + header.bzero; \
                        } else { \
                            t = 0; \
                        } \
                        g = t << cas; \
                        t = dref(ptr8 + size * 2); \
                        if (!header.blank_found || t != header.blank) { \
                            t = t * header.bscale + header.bzero; \
                        } else { \
                            t = 0; \
                        } \
                        b = t; \
                        *dst++ = ((type)a) | ((type)r) | ((type)g) | ((type)b); \
                        ptr8 += cas/8; \
                    } \
                } \
                break

            case_rgb(8, dst32, uint32_t, *);
            case_rgb(16, dst64, uint64_t, AV_RB16);
        }
    } else {
        switch (header.bitpix) {
        #define case_gray(cas, dst, type, t, rd) \
            case cas: \
                for (i = 0; i < avctx->height; i++) { \
                    dst = (type *) (p->data[0] + (avctx->height-i-1)* p->linesize[0]); \
                    for (j = 0; j < avctx->width; j++) { \
                        t = rd; \
                        if (!header.blank_found || t != header.blank) { \
                            t = ((t - header.data_min) * ((1 << (sizeof(type) * 8)) - 1)) / (header.data_max - header.data_min); \
                        } else { \
                            t = 0; \
                        } \
                        *dst++ = (type) t; \
                        ptr8 += abs(cas)/8; \
                    } \
                } \
                break

            case_gray(-64, dst16, uint16_t, tdbl, av_int2double(AV_RB64(ptr8)));
            case_gray(-32, dst16, uint16_t, tflt, av_int2float(AV_RB32(ptr8)));
            case_gray(8, dst8, uint8_t, t8, ptr8[0]);
            case_gray(16, dst16, uint16_t, t16, AV_RB16(ptr8));
            case_gray(32, dst16, uint16_t, t32, AV_RB32(ptr8));
            case_gray(64, dst16, uint16_t, t64, AV_RB64(ptr8));
            default:
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
