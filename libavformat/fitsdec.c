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

    if (AV_RB64(b) == 0x53494d504c452020 &&
        AV_RB64(b + 8) == 0x3D20202020202020 &&
        AV_RB64(b + 16) == 0x2020202020202020 &&
        AV_RB48(b + 24) == 0x202020202054)
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
    int bitpix, naxis, dim_no, i, naxisn[999], groups = 0, pcount = 0, gcount = 1, d;
    int64_t header_size = 0, data_size = 0, ret, t;
    char buf[81] = { 0 }, c;

    ret = avio_read(pb, buf, 80);
    if (ret < 0) {
        return ret;
    } else if (ret < 80) {
        return AVERROR_EOF;
    }
    fits->image = !strncmp(buf, "SIMPLE", 6) || !strncmp(buf, "XTENSION= 'IMAGE", 16);
    header_size += 80;

    ret = avio_read(pb, buf, 80);
    if (ret < 0) {
        return ret;
    } else if (ret < 80) {
        return AVERROR_EOF;
    }
    if (sscanf(buf, "BITPIX = %d", &bitpix) != 1)
        return AVERROR_INVALIDDATA;
    if (bitpix > 64 || bitpix < -64)
        return AVERROR_INVALIDDATA;
    header_size += 80;

    ret = avio_read(pb, buf, 80);
    if (ret < 0) {
        return ret;
    } else if (ret < 80) {
        return AVERROR_EOF;
    }
    if (sscanf(buf, "NAXIS = %d", &naxis) != 1)
        return AVERROR_INVALIDDATA;
    if (naxis < 0 || naxis > 999)
        return AVERROR_INVALIDDATA;
    header_size += 80;

    for (i = 0; i < naxis; i++) {
        ret = avio_read(pb, buf, 80);
        if (ret < 0) {
            return ret;
        } else if (ret < 80) {
            return AVERROR_EOF;
        }
        if (sscanf(buf, "NAXIS%d = %d", &dim_no, &naxisn[i]) != 2)
            return AVERROR_INVALIDDATA;
        if (dim_no != i+1)
            return AVERROR_INVALIDDATA;
        header_size += 80;
    }

    ret = avio_read(pb, buf, 80);
    if (ret < 0) {
        return ret;
    } else if (ret < 80) {
        return AVERROR_EOF;
    }
    header_size += 80;

    while (strncmp(buf, "END", 3)) {
        if (sscanf(buf, "PCOUNT = %d", &d) == 1) {
            pcount = d;
        } else if (sscanf(buf, "GCOUNT = %d", &d) == 1) {
            gcount = d;
        } else if (sscanf(buf, "GROUPS = %c", &c) == 1) {
            groups = (c == 'T');
        }

        ret = avio_read(pb, buf, 80);
        if (ret < 0) {
            return ret;
        } else if (ret < 80) {
            return AVERROR_EOF;
        }
        header_size += 80;
    }

    header_size = ((header_size + 2879) / 2880) * 2880;

    if (groups) {
        fits->image = 0;
        if (naxis > 1)
            data_size = 1;
        for (i = 1; i < naxis; i++) {
            if(naxisn[i] > LLONG_MAX / data_size)
                return AVERROR_INVALIDDATA;
            data_size *= naxisn[i];
        }
    } else if (naxis) {
        data_size = 1;
        for (i = 0; i < naxis; i++) {
            if(naxisn[i] > LLONG_MAX / data_size)
                return AVERROR_INVALIDDATA;
            data_size *= naxisn[i];
        }
    } else {
        fits->image = 0;
    }

    if(pcount > LLONG_MAX - data_size)
        return AVERROR_INVALIDDATA;
    data_size += pcount;

    t = (abs(bitpix) >> 3) * ((int64_t) gcount);
    if(data_size && t > LLONG_MAX / data_size)
        return AVERROR_INVALIDDATA;
    data_size *= t;

    if (!data_size) {
        fits->image = 0;
    } else {
        if(2879 > LLONG_MAX - data_size)
            return AVERROR_INVALIDDATA;
        data_size = ((data_size + 2879) / 2880) * 2880;
    }

    if(header_size > LLONG_MAX - data_size)
        return AVERROR_INVALIDDATA;

    return header_size + data_size;
}

static int fits_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int64_t size=0, pos, ret;
    FITSContext * fits = s->priv_data;

    fits->image = 0;
    pos = avio_tell(s->pb);
    while (!fits->image) {
        ret = avio_seek(s->pb, pos+size, SEEK_SET);
        if (ret < 0)
            return ret;

        if (avio_feof(s->pb))
            return AVERROR_EOF;

        pos += size;
        size = find_size(s->pb, fits);
        if (size < 0)
            return size;
    }

    ret = avio_seek(s->pb, pos, SEEK_SET);
    if (ret < 0)
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
