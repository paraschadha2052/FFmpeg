/*
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

#ifndef AVCODEC_FITS_H
#define AVCODEC_FITS_H

typedef enum FITSHeaderState {
    STATE_SIMPLE,
    STATE_XTENSION,
    STATE_BITPIX,
    STATE_NAXIS,
    STATE_NAXIS_N,
    STATE_REST,
} FITSHeaderState;

/**
 * Structure to store the header keywords in FITS file
 */
typedef struct FITSHeader {
    FITSHeaderState state;
    unsigned naxis_index;
    char simple;
    int bitpix;
    int64_t blank;
    int blank_found;
    int naxis;
    int naxisn[999];
    int rgb; /**< 1 if file contains RGB image, 0 otherwise */
    double bscale;
    double bzero;
    double data_min;
    double data_max;
} FITSHeader;

//ToDo: Add Comments

int avpriv_fits_header_init(FITSHeader *header, FITSHeaderState state);

int avpriv_fits_header_parse_line(FITSHeader *header, uint8_t line[80], AVDictionary ***metadata);

#endif /* AVCODEC_FITS_H */
