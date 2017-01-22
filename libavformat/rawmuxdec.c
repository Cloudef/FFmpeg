/*
 * Rawmux demuxer
 * Copyright (c) 2017 Jari Vetoniemi <mailroxas@gmail.com>
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
 * Rawmux demuxer
 */

#include "libavcodec/raw.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avassert.h"
#include "avformat.h"
#include "internal.h"

enum stream {
    STREAM_NONE,
    STREAM_VIDEO,
    STREAM_AUDIO,
};

static const char HEADER[] = { 'r', 'a', 'w', 'm', 'u', 'x' };

static int rawmux_read_probe(AVProbeData *p)
{
    if (p->buf_size < sizeof(HEADER) || memcmp(HEADER, p->buf, sizeof(HEADER)))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int rawmux_read_header(AVFormatContext *avctx)
{
    AVIOContext *pb = avctx->pb;
    uint8_t version;

    avio_skip(pb, sizeof(HEADER));
    version = avio_r8(pb);

    if (version != 1) {
        av_log(avctx, AV_LOG_ERROR, "Invalid version %u\n", version);
        return AVERROR_INVALIDDATA;
    }

    for (enum stream t; (t = avio_r8(pb)) != STREAM_NONE;) {
        AVStream *st;

        if (t > STREAM_AUDIO) {
            av_log(avctx, AV_LOG_ERROR, "Invalid stream type %u\n", t);
            return AVERROR_INVALIDDATA;
        }

        st = avformat_new_stream(avctx, 0);
        if (!st)
            return AVERROR(ENOMEM);

        if (st->index > 255) {
            av_log(avctx, AV_LOG_ERROR, "Too many streams (max 255)\n");
            return AVERROR_INVALIDDATA;
        }

        if (t == STREAM_VIDEO) {
            char pix_fmt_name[32] = {0};
            AVRational tb;
            int size;

            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;

            avio_get_str(pb, sizeof(pix_fmt_name), pix_fmt_name, sizeof(pix_fmt_name));
            st->codecpar->format = av_get_pix_fmt(pix_fmt_name);

            if (st->codecpar->format == AV_PIX_FMT_NONE) {
                av_log(avctx, AV_LOG_ERROR, "Invalid pixel format '%s' for stream %u\n", pix_fmt_name, st->index);
                return AVERROR_INVALIDDATA;
            }

            tb.num = avio_rl32(pb);
            tb.den = avio_rl32(pb);
            avpriv_set_pts_info(st, 64, tb.num, tb.den);

            st->codecpar->width = avio_rl32(pb);
            st->codecpar->height = avio_rl32(pb);

            size = av_image_get_buffer_size(st->codecpar->format, st->codecpar->width, st->codecpar->height, 1);
            if (size < 0)
                return size;

            st->codecpar->bit_rate = av_rescale_q(size, (AVRational){8,1}, st->time_base);
        } else if (t == STREAM_AUDIO) {
            char codec_name[32] = { 'p', 'c', 'm', '_', };
            AVCodec *codec;

            avio_get_str(pb, sizeof(codec_name) - 4, codec_name + 4, sizeof(codec_name) - 4);
            codec = avcodec_find_decoder_by_name(codec_name);

            if (!codec || codec->type != AVMEDIA_TYPE_AUDIO) {
                av_log(avctx, AV_LOG_ERROR, "Invalid codec '%s' for stream %u\n", codec_name, st->index);
                return AVERROR_INVALIDDATA;
            }

            st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_id = codec->id;
            st->codecpar->sample_rate = avio_rl32(pb);
            st->codecpar->channels = avio_r8(pb);
            st->codecpar->bits_per_coded_sample = av_get_bits_per_sample(st->codecpar->codec_id);
            av_assert0(st->codecpar->bits_per_coded_sample > 0);

            st->codecpar->block_align = st->codecpar->bits_per_coded_sample * st->codecpar->channels / 8;
            avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
        }
    }

    return 0;
}

static int rawmux_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int stream, size, ret;
    int64_t pts;

    stream = avio_r8(avctx->pb);
    size = avio_rl32(avctx->pb);
    pts = avio_rl64(avctx->pb);

    ret = av_get_packet(avctx->pb, pkt, size);
    if (ret < 0) {
        return ret;
    }

    pkt->pts = pkt->dts = pts;
    pkt->stream_index = stream;
    return 0;
}

AVInputFormat ff_rawmux_demuxer = {
    .name           = "rawmux",
    .long_name      = NULL_IF_CONFIG_SMALL("raw Media Container"),
    .read_probe     = rawmux_read_probe,
    .read_header    = rawmux_read_header,
    .read_packet    = rawmux_read_packet,
};
