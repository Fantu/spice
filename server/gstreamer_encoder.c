/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 Jeremy White
   Copyright (C) 2015 Francois Gouget

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <<A HREF="http://www.gnu.org/licenses/">http://www.gnu.org/licenses/</A>>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "red_common.h"

typedef struct GstEncoder GstEncoder;
#define VIDEO_ENCODER_T GstEncoder
#include "video_encoder.h"


#define DEFAULT_FPS 30


typedef struct {
    SpiceBitmapFmt  spice_format;
    int             bpp;
    int             depth;
    int             endianness;
    int             blue_mask;
    int             green_mask;
    int             red_mask;
} SpiceFormatForGStreamer;

struct GstEncoder {
    VideoEncoder base;

    /* The GStreamer pipeline. If pipeline is NULL then the other pointers are
     * invalid.
     */
    GstElement *pipeline;
    GstCaps *src_caps;
    GstAppSrc *appsrc;
    GstElement *gstenc;
    GstAppSink *appsink;

    /* The frame counter for GStreamer buffers */
    int frame;

    /* Source video information */
    int width;
    int height;
    SpiceFormatForGStreamer *format;
    SpiceBitmapFmt spice_format;

    /* Rate control information */
    uint64_t bit_rate;

    /* Rate control callbacks */
    VideoEncoderRateControlCbs cbs;
    void *cbs_opaque;

    /* stats */
    uint64_t starting_bit_rate;
};

/* The source fps changes all the time so don't store it */
uint32_t get_source_fps(GstEncoder *encoder)
{
    return encoder->cbs.get_roundtrip_ms ?
        encoder->cbs.get_source_fps(encoder->cbs_opaque) : DEFAULT_FPS;
}

static SpiceFormatForGStreamer *map_format(SpiceBitmapFmt format)
{
    int i;
    static SpiceFormatForGStreamer format_map[] =  {
        { SPICE_BITMAP_FMT_RGBA, 32, 24, 4321, 0xff000000, 0xff0000, 0xff00},
        /* TODO: Check the other formats */
        { SPICE_BITMAP_FMT_32BIT, 32, 24, 4321, 0xff000000, 0xff0000, 0xff00},
        { SPICE_BITMAP_FMT_24BIT, 24, 24, 4321, 0xff0000, 0xff00, 0xff},
        { SPICE_BITMAP_FMT_16BIT, 16, 16, 4321, 0x001f, 0x03E0, 0x7C00},
    };

    for (i = 0; i < sizeof(format_map) / sizeof(format_map[0]); i++) {
        if (format_map[i].spice_format == format) {
            return &format_map[i];
        }
    }

    return NULL;
}

static void reset_pipeline(GstEncoder *encoder)
{
    if (!encoder->pipeline) {
        return;
    }

    gst_element_set_state(encoder->pipeline, GST_STATE_NULL);
    gst_caps_unref(encoder->src_caps);
    gst_object_unref(encoder->appsrc);
    gst_object_unref(encoder->gstenc);
    gst_object_unref(encoder->appsink);
    gst_object_unref(encoder->pipeline);
    encoder->pipeline = NULL;
}

static void adjust_bit_rate(GstEncoder *encoder)
{
    uint64_t raw_bit_rate;
    uint32_t fps, compression;

    fps = get_source_fps(encoder);
    raw_bit_rate = encoder->width * encoder->height * encoder->format->depth * fps;

    if (!encoder->bit_rate) {
        /* If no bit rate is available start with 10 Mbps and let the rate
         * control mechanisms trim it down.
         */
        encoder->bit_rate = 10 * 1024 * 1024;
        spice_debug("resetting the bit rate to 10Mbps");

    } else if (encoder->bit_rate < 20 * 1024) {
        /* Don't let the bit rate go below 20kbps. */
        encoder->bit_rate = 20 * 1024;
        spice_debug("bottoming the bit rate to 20kpbs");
    }

    compression = raw_bit_rate / encoder->bit_rate;
    if (compression < 10) {
        /* Even MJPEG should achieve good quality with a 10x compression level */
        encoder->bit_rate = raw_bit_rate / 10;
        spice_debug("capping the bit rate to %.2fMbps for a 10x compression level", ((double)encoder->bit_rate) / 1024 / 1024);

    } else {
        spice_debug("setting the bit rate to %.2fMbps for a %dx compression level", ((double)encoder->bit_rate) / 1024 / 1024, compression);
    }
}

static gboolean set_appsrc_caps(GstEncoder *encoder)
{
    GstCaps *new_caps = gst_caps_new_simple("video/x-raw-rgb",
        "bpp", G_TYPE_INT, encoder->format->bpp,
        "depth", G_TYPE_INT, encoder->format->depth,
        "width", G_TYPE_INT, encoder->width,
        "height", G_TYPE_INT, encoder->height,
        "endianness", G_TYPE_INT, encoder->format->endianness,
        "red_mask", G_TYPE_INT, encoder->format->red_mask,
        "green_mask", G_TYPE_INT, encoder->format->green_mask,
        "blue_mask", G_TYPE_INT, encoder->format->blue_mask,
        "framerate", GST_TYPE_FRACTION, get_source_fps(encoder), 1,
        NULL);
    if (!new_caps) {
        spice_warning("GStreamer error: could not create the source caps");
        reset_pipeline(encoder);
        return FALSE;
    }
    g_object_set(G_OBJECT(encoder->appsrc), "caps", new_caps, NULL);
    if (encoder->src_caps) {
        gst_caps_unref(encoder->src_caps);
    }
    encoder->src_caps = new_caps;
    return TRUE;
}

static gboolean construct_pipeline(GstEncoder *encoder, const SpiceBitmap *bitmap)
{
    gboolean no_clock = FALSE;
    const gchar* gstenc_name;
    switch (encoder->base.codec_type)
    {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        gstenc_name = "ffenc_mjpeg";
        no_clock = TRUE;
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        gstenc_name = "vp8enc";
        break;
    default:
        spice_warning("unsupported codec type %d", encoder->base.codec_type);
        return FALSE;
    }

    GError *err = NULL;
    gchar *desc = g_strdup_printf("appsrc name=src format=2 do-timestamp=true ! ffmpegcolorspace ! %s name=encoder ! appsink name=sink", gstenc_name);
    spice_debug("GStreamer pipeline: %s", desc);
    encoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(desc);
    if (!encoder->pipeline) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }
    encoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "src"));
    encoder->gstenc = gst_bin_get_by_name(GST_BIN(encoder->pipeline), "encoder");
    encoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "sink"));

    /* Set the encoder initial bit rate, and ask for a low latency */
    adjust_bit_rate(encoder);
    g_object_set(G_OBJECT(encoder->gstenc), "bitrate", encoder->bit_rate, NULL);
    if (encoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_VP8) {
        /* See http://www.webmproject.org/docs/encoder-parameters/ */
        g_object_set(G_OBJECT(encoder->gstenc),
                     "mode", 1, /* CBR */
                     "max-latency", 0,
                     "speed", 7,
                     "resize-allowed", TRUE,
                     "threads", g_get_num_processors() - 1,
                     NULL);
    }

    /* Set the source caps */
    encoder->src_caps = NULL;
    if (!set_appsrc_caps(encoder)) {
        return FALSE;
    }

    /* Start playing */
    if (no_clock) {
        spice_debug("removing the pipeline clock");
        gst_pipeline_use_clock(GST_PIPELINE(encoder->pipeline), NULL);
    }
    spice_debug("setting state to PLAYING");
    if (gst_element_set_state(encoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spice_warning("GStreamer error: unable to set the pipeline to the playing state");
        reset_pipeline(encoder);
        return FALSE;
    }
    return TRUE;
}

static void reconfigure_pipeline(GstEncoder *encoder)
{
    if (encoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_VP8) {
        /* vp8enc gets confused if we try to reconfigure the pipeline */
        reset_pipeline(encoder);

    } else if (gst_element_set_state(encoder->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
             !set_appsrc_caps(encoder) ||
             gst_element_set_state(encoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spice_debug("GStreamer error: the pipeline reconfiguration failed, rebuilding it instead");
        reset_pipeline(encoder); /* we can rebuild it... */
    }
}

static int push_raw_frame(GstEncoder *encoder, const SpiceBitmap *bitmap,
                          const SpiceRect *src, int top_down)
{
    const uint32_t stream_height = src->bottom - src->top;
    const uint32_t stream_stride = (src->right - src->left) * encoder->format->bpp / 8;
    uint32_t len = stream_stride * stream_height;
    GstBuffer *buffer = gst_buffer_new_and_alloc(len);
    uint8_t *dst = GST_BUFFER_DATA(buffer);

    /* Note that we should not reorder the lines, even if top_down is false.
     * It just changes the number of lines to skip at the start of the bitmap.
     */
    const uint32_t skip_lines = top_down ? src->top : bitmap->y - (src->bottom - 0);
    uint32_t chunk_offset = bitmap->stride * skip_lines;
    SpiceChunks *chunks = bitmap->data;
    uint32_t chunk = 0;

    if (stream_stride == bitmap->stride) {
        /* We can copy the bitmap chunk by chunk */
        while (len && chunk < chunks->num_chunks) {
            /* Make sure that the chunk is not padded */
            if (chunks->chunk[chunk].len % bitmap->stride != 0) {
                gst_buffer_unref(buffer);
                return VIDEO_ENCODER_FRAME_UNSUPPORTED;
            }
            if (chunk_offset >= chunks->chunk[chunk].len) {
                chunk_offset -= chunks->chunk[chunk].len;
                chunk++;
                continue;
            }

            uint8_t *src = chunks->chunk[chunk].data + chunk_offset;
            uint32_t thislen = MIN(chunks->chunk[chunk].len - chunk_offset, len);
            memcpy(dst, src, thislen);
            dst += thislen;
            len -= thislen;
            chunk_offset = 0;
            chunk++;
        }
        spice_assert(len == 0);

    } else {
        /* We have to do a line-by-line copy because for each we have to leave
         * out pixels on the left or right.
         */
        chunk_offset += src->left * encoder->format->bpp / 8;
        for (int l = 0; l < stream_height; l++) {
            /* We may have to move forward by more than one chunk the first
             * time around.
             */
            while (chunk_offset >= chunks->chunk[chunk].len) {
                /* Make sure that the chunk is not padded */
                if (chunks->chunk[chunk].len % bitmap->stride != 0) {
                    gst_buffer_unref(buffer);
                    return VIDEO_ENCODER_FRAME_UNSUPPORTED;
                }
                chunk_offset -= chunks->chunk[chunk].len;
                chunk++;
            }

            /* Copy the line */
            uint8_t *src = chunks->chunk[chunk].data + chunk_offset;
            memcpy(dst, src, stream_stride);
            dst += stream_stride;
            chunk_offset += bitmap->stride;
        }
        spice_assert(dst - GST_BUFFER_DATA(buffer) == len);
    }

    gst_buffer_set_caps(buffer, encoder->src_caps);
    GST_BUFFER_OFFSET(buffer) = encoder->frame++;

    GstFlowReturn ret = gst_app_src_push_buffer(encoder->appsrc, buffer);
    if (ret != GST_FLOW_OK) {
        spice_debug("GStreamer error: unable to push source buffer (%d)", ret);
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    return VIDEO_ENCODER_FRAME_ENCODE_DONE;
}

static int pull_compressed_buffer(GstEncoder *encoder,
                                  uint8_t **outbuf, size_t *outbuf_size,
                                  int *data_size)
{
    GstBuffer *buffer = gst_app_sink_pull_buffer(encoder->appsink);
    if (buffer) {
        int len = GST_BUFFER_SIZE(buffer);
        spice_assert(outbuf && outbuf_size);
        if (!*outbuf || *outbuf_size < len) {
            *outbuf = spice_realloc(*outbuf, len);
            *outbuf_size = len;
        }
        /* TODO Try to avoid this copy by changing the GstBuffer handling */
        memcpy(*outbuf, GST_BUFFER_DATA(buffer), len);
        gst_buffer_unref(buffer);
        *data_size = len;
        return VIDEO_ENCODER_FRAME_ENCODE_DONE;
    }
    return VIDEO_ENCODER_FRAME_UNSUPPORTED;
}

static void gst_encoder_destroy(GstEncoder *encoder)
{
    reset_pipeline(encoder);
    free(encoder);
}

static int gst_encoder_encode_frame(GstEncoder *encoder,
                                    const SpiceBitmap *bitmap,
                                    int width, int height,
                                    const SpiceRect *src, int top_down,
                                    uint32_t frame_mm_time,
                                    uint8_t **outbuf, size_t *outbuf_size,
                                    int *data_size)
{
    int rc;

    if (!encoder->pipeline || width != encoder->width ||
        height != encoder->height || encoder->spice_format != bitmap->format) {
        spice_debug("video format change: width %d -> %d, height %d -> %d, format %d -> %d", encoder->width, width, encoder->height, height, encoder->spice_format, bitmap->format);
        encoder->format = map_format(bitmap->format);
        if (!encoder->format) {
            spice_debug("unable to map format type %d", bitmap->format);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        encoder->spice_format = bitmap->format;
        encoder->width = width;
        encoder->height = height;
        if (encoder->pipeline) {
            reconfigure_pipeline(encoder);
        }
    }
    if (!encoder->pipeline && !construct_pipeline(encoder, bitmap)) {
        return VIDEO_ENCODER_FRAME_DROP;
    }

    rc = push_raw_frame(encoder, bitmap, src, top_down);
    if (rc == VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        rc = pull_compressed_buffer(encoder, outbuf, outbuf_size, data_size);
    }
    return rc;
}

void gst_encoder_client_stream_report(GstEncoder *encoder,
                                      uint32_t num_frames, uint32_t num_drops,
                                      uint32_t start_frame_mm_time,
                                      uint32_t end_frame_mm_time,
                                      int32_t end_frame_delay,
                                      uint32_t audio_delay)
{
    spice_debug("client report: #frames %u, #drops %d, duration %u video-delay %d audio-delay %u",
                num_frames, num_drops,
                end_frame_mm_time - start_frame_mm_time,
                end_frame_delay, audio_delay);
}

void gst_encoder_notify_server_frame_drop(GstEncoder *encoder)
{
    spice_debug("server frame drop");
}

uint64_t gst_encoder_get_bit_rate(GstEncoder *encoder)
{
    return encoder->bit_rate;
}

void gst_encoder_get_stats(GstEncoder *encoder, VideoEncoderStats *stats)
{
    uint64_t raw_bit_rate = encoder->width * encoder->height * encoder->format->bpp * get_source_fps(encoder);

    spice_assert(encoder != NULL && stats != NULL);
    stats->starting_bit_rate = encoder->starting_bit_rate;
    stats->cur_bit_rate = encoder->bit_rate;

    /* Use the compression level as a proxy for the quality */
    stats->avg_quality = 100.0 - raw_bit_rate / encoder->bit_rate;
    if (stats->avg_quality < 0) {
        stats->avg_quality = 0;
    }
}

GstEncoder *create_gstreamer_encoder(SpiceVideoCodecType codec_type, uint64_t starting_bit_rate, VideoEncoderRateControlCbs *cbs, void *cbs_opaque)
{
    GstEncoder *encoder;

    spice_assert(!cbs || (cbs && cbs->get_roundtrip_ms && cbs->get_source_fps));
    if (codec_type != SPICE_VIDEO_CODEC_TYPE_MJPEG &&
        codec_type != SPICE_VIDEO_CODEC_TYPE_VP8) {
        spice_warning("unsupported codec type %d", codec_type);
        return NULL;
    }

    gst_init(NULL, NULL);

    encoder = spice_new0(GstEncoder, 1);
    encoder->base.destroy = &gst_encoder_destroy;
    encoder->base.encode_frame = &gst_encoder_encode_frame;
    encoder->base.client_stream_report = &gst_encoder_client_stream_report;
    encoder->base.notify_server_frame_drop = &gst_encoder_notify_server_frame_drop;
    encoder->base.get_bit_rate = &gst_encoder_get_bit_rate;
    encoder->base.get_stats = &gst_encoder_get_stats;
    encoder->base.codec_type = codec_type;
    encoder->pipeline = NULL;

    if (cbs) {
        encoder->cbs = *cbs;
    } else {
        encoder->cbs.get_roundtrip_ms = NULL;
    }
    encoder->cbs_opaque = cbs_opaque;
    encoder->bit_rate = encoder->starting_bit_rate = starting_bit_rate;

    return encoder;
}
