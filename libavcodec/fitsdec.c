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
#include "libavutil/opt.h"
#include "fits.h"

typedef struct FITSContext {
    const AVClass *class;
    int blank_val;
} FITSContext;

static int dict_set_if_not_null(AVDictionary ***metadata, char * keyword, char * value)
{
    if (metadata)
        av_dict_set(*metadata, keyword, value, 0);
    return 0;
}

int avpriv_fits_header_init(FITSHeader *header, FITSHeaderState state)
{
    header->state = state;
    header->naxis_index = 0;
    header->blank_found = 0;
    header->pcount = 0;
    header->gcount = 1;
    header->groups = 0;
    header->rgb = 0;
    header->image_extension = 0;
    header->bscale = 1.0;
    header->bzero = 0;
    header->data_min_found = 0;
    header->data_max_found = 0;
    return 0;
}

/**
 * Calculate the data_min and data_max values from the data.
 * This is called if the values are not present in the header.
 * @param ptr8 pointer to the data
 * @param header pointer to the header
 * @param end pointer to end of packet
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
#define CASE_N(a, t, rd) \
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
                ptr8 += abs(a) >> 3; \
            } \
        } \
        break

        CASE_N(-64, tdbl, av_int2double(AV_RB64(ptr8)));
        CASE_N(-32, tflt, av_int2float(AV_RB32(ptr8)));
        CASE_N(8, t8, ptr8[0]);
        CASE_N(16, t16, AV_RB16(ptr8));
        CASE_N(32, t32, AV_RB32(ptr8));
        CASE_N(64, t64, AV_RB64(ptr8));
        default:
            return AVERROR_INVALIDDATA;
    }
    return 0;
}

/**
 * Extract keyword and value from a header line (80 bytes) and store them in keyword and value strings respectively
 * @param ptr8 pointer to the data
 * @param keyword pointer to the char array in which keyword is to be stored
 * @param value pointer to the char array in which value is to be stored
 * @return 0 if calculated successfully otherwise AVERROR_INVALIDDATA
 */
static int read_keyword_value(const uint8_t * ptr8, char * keyword, char * value)
{
    int i;

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

int avpriv_fits_header_parse_line(void *avcl, FITSHeader *header, const uint8_t line[80], AVDictionary ***metadata)
{
    int dim_no, ret;
    int64_t t;
    double d;
    char keyword[10], value[72], c;

    read_keyword_value(line, keyword, value);
    switch (header->state) {
        case STATE_SIMPLE:
            if (strncmp(keyword, "SIMPLE", 6)) {
                av_log(avcl, AV_LOG_ERROR, "expected SIMPLE keyword, found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (value[0] == 'F') {
                av_log(avcl, AV_LOG_WARNING, "not a standard FITS file\n");
            } else if (value[0] != 'T') {
                av_log(avcl, AV_LOG_ERROR, "invalid value of SIMPLE keyword, SIMPLE = %c\n", value[0]);
                return AVERROR_INVALIDDATA;
            }
            header->state = STATE_BITPIX;
            break;
        case STATE_XTENSION:
            if (strncmp(keyword, "XTENSION", 8)) {
                av_log(avcl, AV_LOG_ERROR, "expected XTENSION keyword, found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (!strncmp(value, "'IMAGE   '", 10)) {
                header->image_extension = 1;
            }

            header->state = STATE_BITPIX;
            break;
        case STATE_BITPIX:
            if (strncmp(keyword, "BITPIX", 6)) {
                av_log(avcl, AV_LOG_ERROR, "expected BITPIX keyword, found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (sscanf(value, "%d", &header->bitpix) != 1) {
                av_log(avcl, AV_LOG_ERROR, "invalid value of BITPIX keyword, %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }
            dict_set_if_not_null(metadata, keyword, value);
            header->state = STATE_NAXIS;
            break;
        case STATE_NAXIS:
            if (strncmp(keyword, "NAXIS", 5)) {
                av_log(avcl, AV_LOG_ERROR, "expected NAXIS keyword, found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (sscanf(value, "%d", &header->naxis) != 1) {
                av_log(avcl, AV_LOG_ERROR, "invalid value of NAXIS keyword, %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            dict_set_if_not_null(metadata, keyword, value);
            if (header->naxis) {
                header->state = STATE_NAXIS_N;
            } else {
                if(header->image_extension) {
                    header->state = STATE_PCOUNT;
                } else {
                    header->state = STATE_REST;
                }
            }
            break;
        case STATE_NAXIS_N:
            ret = sscanf(keyword, "NAXIS%d", &dim_no);
            if (ret != 1 || dim_no != header->naxis_index + 1) {
                av_log(avcl, AV_LOG_ERROR, "expected NAXIS%d keyword, found %s = %s\n", header->naxis_index + 1, keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (sscanf(value, "%d", &header->naxisn[header->naxis_index]) != 1) {
                av_log(avcl, AV_LOG_ERROR, "invalid value of NAXIS%d keyword, %s = %s\n", header->naxis_index + 1, keyword, value);
                return AVERROR_INVALIDDATA;
            }

            dict_set_if_not_null(metadata, keyword, value);
            header->naxis_index++;
            if (header->naxis_index == header->naxis) {
                if(header->image_extension) {
                    header->state = STATE_PCOUNT;
                } else {
                    header->state = STATE_REST;
                }
            }
            break;
        case STATE_PCOUNT:
            if (strncmp(keyword, "PCOUNT", 6)) {
                av_log(avcl, AV_LOG_ERROR, "expected PCOUNT keyword, found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (sscanf(value, "%d", &header->pcount) != 1) {
                av_log(avcl, AV_LOG_ERROR, "invalid value of PCOUNT keyword, %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (header->pcount) {
                av_log(avcl, AV_LOG_ERROR, "expected PCOUNT = 0 but found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            header->state = STATE_GCOUNT;
            break;
        case STATE_GCOUNT:
            if (strncmp(keyword, "GCOUNT", 6)) {
                av_log(avcl, AV_LOG_ERROR, "expected GCOUNT keyword, found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (sscanf(value, "%d", &header->gcount) != 1) {
                av_log(avcl, AV_LOG_ERROR, "invalid value of GCOUNT keyword, %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            if (header->gcount != 1) {
                av_log(avcl, AV_LOG_ERROR, "expected GCOUNT = 1 but found %s = %s\n", keyword, value);
                return AVERROR_INVALIDDATA;
            }

            header->state = STATE_REST;
            break;
        case STATE_REST:
            if (!strncmp(keyword, "BLANK", 5) && sscanf(value, "%"SCNd64"", &t) == 1) {
                header->blank = t;
                header->blank_found = 1;
            } else if (!strncmp(keyword, "BSCALE", 6) && sscanf(value, "%lf", &d) == 1) {
                header->bscale = d;
            } else if (!strncmp(keyword, "BZERO", 5) && sscanf(value, "%lf", &d) == 1) {
                header->bzero = d;
            } else if (!strncmp(keyword, "CTYPE3", 6) && !strncmp(value, "'RGB", 4)) {
                header->rgb = 1;
            } else if (!strncmp(keyword, "DATAMAX", 7) && sscanf(value, "%lf", &d) == 1) {
                header->data_max_found = 1;
                header->data_max = d;
            } else if (!strncmp(keyword, "DATAMIN", 7) && sscanf(value, "%lf", &d) == 1) {
                header->data_min_found = 1;
                header->data_min = d;
            } else if (!strncmp(keyword, "END\0", 4)) {
                return 1;
            } else if (!strncmp(keyword, "GROUPS", 6) && sscanf(value, "%c", &c) == 1) {
                header->groups = (c == 'T');
            } else if (!header->image_extension) {
                if (!strncmp(keyword, "GCOUNT", 6) && sscanf(value, "%"SCNd64"", &t) == 1) {
                    header->gcount = t;
                } else if (!strncmp(keyword, "PCOUNT", 6) && sscanf(value, "%"SCNd64"", &t) == 1) {
                    header->pcount = t;
                }
            }
            dict_set_if_not_null(metadata, keyword, value);
            break;
    }
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
                            const uint8_t * end, AVDictionary **metadata)
{
    const uint8_t *ptr8 = *ptr;
    int lines_read, i, ret;
    uint64_t size, t;

    lines_read = 1; // to account for first header line, SIMPLE or XTENSION which is not included in packet...
    avpriv_fits_header_init(header, STATE_BITPIX);
    do {
        if (end - ptr8 < 80)
            return AVERROR_INVALIDDATA;
        ret = avpriv_fits_header_parse_line(avctx, header, ptr8, &metadata);
        ptr8 += 80;
        lines_read++;
    } while (!ret);
    if (ret < 0)
        return ret;

    lines_read %= 36;
    t = ((36 - lines_read) % 36) * 80;
    if (end - ptr8 < t)
        return AVERROR_INVALIDDATA;
    ptr8 += t;

    if (header->rgb && (header->naxis != 3 || (header->naxisn[2] != 3 && header->naxisn[2] != 4))) {
        av_log(avctx, AV_LOG_ERROR, "File contains RGB image but NAXIS = %d and NAXIS3 = %d\n", header->naxis, header->naxisn[2]);
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

    size = abs(header->bitpix) >> 3;
    for (i = 0; i < header->naxis; i++) {
        if (header->naxisn[i] > ULLONG_MAX / size) {
            av_log(avctx, AV_LOG_ERROR, "unsupported size of FITS image");
            return AVERROR_INVALIDDATA;
        }
        size *= header->naxisn[i];
    }

    if (end - ptr8 < size)
        return AVERROR_INVALIDDATA;
    *ptr = ptr8;

    if (!header->rgb && (!header->data_min_found || !header->data_max_found)) {
        ret = fill_data_min_max(ptr8, header, end);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "invalid BITPIX, %d\n", header->bitpix);
            return ret;
        }
    } else {
        /*
         * instead of applying bscale and bzero to every element,
         * we can do inverse transformation on data_min and data_max
         */
        header->data_min = (header->data_min - header->bzero) / header->bscale;
        header->data_max = (header->data_max - header->bzero) / header->bscale;
    }

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
    int ret, i, j, k;
    int map[] = {2, 0, 1, 3}; // mapping from GBRA -> RGBA as RGBA is to be stored in FITS file..
    uint8_t *dst8;
    uint16_t *dst16;
    uint64_t t;
    FITSHeader header;
    FITSContext * fitsctx = avctx->priv_data;

    end = ptr8 + avpkt->size;
    p->metadata = NULL;
    ret = fits_read_header(avctx, &ptr8, &header, end, &p->metadata);
    if (ret < 0)
        return ret;

    if (header.rgb) {
        if (header.bitpix == 8) {
            if (header.naxisn[2] == 3) {
                avctx->pix_fmt = AV_PIX_FMT_GBRP;
            } else {
                avctx->pix_fmt = AV_PIX_FMT_GBRAP;
            }
        } else if (header.bitpix == 16) {
            if (header.naxisn[2] == 3) {
                avctx->pix_fmt = AV_PIX_FMT_GBRP16;
            } else {
                avctx->pix_fmt = AV_PIX_FMT_GBRAP16;
            }
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
#define CASE_RGB(cas, dst, type, dref) \
    case cas: \
        for (k = 0; k < header.naxisn[2]; k++) { \
            for (i = 0; i < avctx->height; i++) { \
                dst = (type *) (p->data[map[k]] + (avctx->height - i - 1) * p->linesize[map[k]]); \
                for (j = 0; j < avctx->width; j++) { \
                    t32 = dref(ptr8); \
                    if (!header.blank_found || t32 != header.blank) { \
                        t = t32 * header.bscale + header.bzero; \
                    } else { \
                        t = fitsctx->blank_val; \
                    } \
                    *dst++ = (type) t; \
                    ptr8 += cas >> 3; \
                } \
            } \
        } \
        break

            CASE_RGB(8, dst8, uint8_t, *);
            CASE_RGB(16, dst16, uint16_t, AV_RB16);
        }
    } else {
        switch (header.bitpix) {
#define CASE_GRAY(cas, dst, type, t, rd) \
    case cas: \
        for (i = 0; i < avctx->height; i++) { \
            dst = (type *) (p->data[0] + (avctx->height-i-1)* p->linesize[0]); \
            for (j = 0; j < avctx->width; j++) { \
                t = rd; \
                if (!header.blank_found || t != header.blank) { \
                    t = ((t - header.data_min) * ((1 << (sizeof(type) * 8)) - 1)) / (header.data_max - header.data_min); \
                } else { \
                    t = fitsctx->blank_val; \
                } \
                *dst++ = (type) t; \
                ptr8 += abs(cas) >> 3; \
            } \
        } \
        break

            CASE_GRAY(-64, dst16, uint16_t, tdbl, av_int2double(AV_RB64(ptr8)));
            CASE_GRAY(-32, dst16, uint16_t, tflt, av_int2float(AV_RB32(ptr8)));
            CASE_GRAY(8, dst8, uint8_t, t8, ptr8[0]);
            CASE_GRAY(16, dst16, uint16_t, t16, AV_RB16(ptr8));
            CASE_GRAY(32, dst16, uint16_t, t32, AV_RB32(ptr8));
            CASE_GRAY(64, dst16, uint16_t, t64, AV_RB64(ptr8));
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

static const AVOption fits_options[] = {
    { "blank_value", "value that is used to replace BLANK pixels in data array", offsetof(FITSContext, blank_val), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 65535, AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM},
    { NULL },
};

static const AVClass fits_decoder_class = {
    .class_name = "FITS decoder",
    .item_name  = av_default_item_name,
    .option     = fits_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_fits_decoder = {
    .name           = "fits",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FITS,
    .priv_data_size = sizeof(FITSContext),
    .decode         = fits_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("Flexible Image Transport System"),
    .priv_class     = &fits_decoder_class
};
