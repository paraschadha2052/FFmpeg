/*
 * FITS muxer
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
 * FITS muxer.
 */

#include "internal.h"

static int fits_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

AVOutputFormat ff_fits_muxer = {
    .name         = "fits",
    .long_name    = NULL_IF_CONFIG_SMALL("Flexible Image Transport System"),
    .extensions   = "fits",
    .audio_codec  = AV_CODEC_ID_NONE,
    .video_codec  = AV_CODEC_ID_FITS,
    .write_packet = fits_write_packet,
};
