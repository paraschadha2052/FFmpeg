/*
 * FITS demuxer
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
 * FITS demuxer.
 */

#include "libavutil/intreadwrite.h"
#include "internal.h"
#include "libavutil/opt.h"

typedef struct FITSContext {
    const AVClass *class;
    AVRational framerate;
    int image;
    int64_t pts;
} FITSContext;

static int fits_probe(AVProbeData *p)
{
    const uint8_t *b = p->buf;

    if (AV_RB32(b) == 0x53494d50 && AV_RB16(b+4) == 0x4c45)
        return AVPROBE_SCORE_MAX - 1;
    return 0;
}

static int fits_read_header(AVFormatContext *s)
{
    AVStream *st = avformat_new_stream(s, NULL);
    FITSContext * fits = s->priv_data;

    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_FITS;

    avpriv_set_pts_info(st, 64, fits->framerate.den, fits->framerate.num);
    fits->pts = 0;
    return 0;
}

static int64_t find_size(AVIOContext * pb, FITSContext * fits)
{
    int bitpix, naxis, dim_no, i, naxisn[999], groups=0;
    int64_t header_size = 0, data_size=0, ret, pcount=0, gcount=1, d;
    char buf[80], c;

    if((ret = avio_read(pb, buf, 80)) != 80)
        return AVERROR_EOF;
    if(!strncmp(buf, "SIMPLE", 6) || !strncmp(buf, "XTENSION= 'IMAGE", 16))
        fits->image = 1;
    else
        fits->image = 0;
    header_size += 80;

    if((ret = avio_read(pb, buf, 80)) != 80)
        return AVERROR_EOF;
    if (sscanf(buf, "BITPIX = %d", &bitpix) != 1)
        return AVERROR_INVALIDDATA;
    header_size += 80;

    if((ret = avio_read(pb, buf, 80)) != 80)
        return AVERROR_EOF;
    if (sscanf(buf, "NAXIS = %d", &naxis) != 1)
        return AVERROR_INVALIDDATA;
    header_size += 80;

    for (i = 0; i < naxis; i++) {
        if((ret = avio_read(pb, buf, 80)) != 80)
            return AVERROR_EOF;
        if (sscanf(buf, "NAXIS%d = %d", &dim_no, &naxisn[i]) != 2 || dim_no != i+1)
            return AVERROR_INVALIDDATA;
        header_size += 80;
    }

    if((ret = avio_read(pb, buf, 80)) != 80)
        return AVERROR_EOF;
    header_size += 80;

    while (strncmp(buf, "END", 3)) {
        if (sscanf(buf, "PCOUNT = %ld", &d) == 1) {
            pcount = d;
        } else if (sscanf(buf, "GCOUNT = %ld", &d) == 1) {
            gcount = d;
        } else if (sscanf(buf, "GROUPS = %c", &c) == 1) {
            if (c == 'T') {
                groups = 1;
            }
        }

        if((ret = avio_read(pb, buf, 80)) != 80)
            return AVERROR_EOF;
        header_size += 80;
    }

    header_size = ceil(header_size/2880.0)*2880;

    if(groups) {
        fits->image = 0;
        if (naxis > 1)
            data_size = 1;
        for(i = 1; i < naxis; i++) {
            data_size *= naxisn[i];
        }
    } else if (naxis) {
        data_size = 1;
        for(i = 0; i < naxis; i++) {
            data_size *= naxisn[i];
        }
    } else {
        fits->image = 0;
    }

    data_size += pcount;
    data_size *= (abs(bitpix) >> 3) * gcount;

    if(data_size == 0)
        fits->image = 0;
    else
        data_size = ceil(data_size/2880.0)*2880;

    return header_size + data_size;
}

static int fits_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int64_t size=0, pos, ret;
    FITSContext * fits = s->priv_data;

    fits->image = 0;
    pos = avio_tell(s->pb);
    while(!fits->image) {
        if((ret = avio_seek(s->pb, pos+size, SEEK_SET)) < 0)
            return ret;

        if (avio_feof(s->pb))
            return AVERROR_EOF;

        pos = avio_tell(s->pb);
        if((size = find_size(s->pb, fits)) < 0)
            return size;
    }

    if((ret = avio_seek(s->pb, pos, SEEK_SET)) < 0)
        return ret;

    ret = av_get_packet(s->pb, pkt, size);
    if (ret != size) {
        if (ret > 0) av_packet_unref(pkt);
        return AVERROR(EIO);
    }

    pkt->stream_index = 0;
    pkt->flags |= AV_PKT_FLAG_KEY;
    pkt->pos = pos;
    pkt->size = size;
    pkt->pts = fits->pts;
    fits->pts++;

    return size;
}

static const AVOption fits_options[] = {
    { "framerate", "set the framerate", offsetof(FITSContext, framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "1"}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM},
    { NULL },
};

static const AVClass fits_demuxer_class = {
    .class_name = "FITS demuxer",
    .item_name  = av_default_item_name,
    .option     = fits_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_fits_demuxer = {
    .name           = "fits",
    .long_name      = NULL_IF_CONFIG_SMALL("Flexible Image Transport System"),
    .priv_data_size = sizeof(FITSContext),
    .read_probe     = fits_probe,
    .read_header    = fits_read_header,
    .read_packet    = fits_read_packet,
    .priv_class     = &fits_demuxer_class,
    .raw_codec_id   = AV_CODEC_ID_FITS,
};
