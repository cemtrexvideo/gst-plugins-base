/* GStreamer
 * Copyright (C) 2004 Wim Taymans <wim@fluendo.com>
 * Copyright (C) 2006 Tim-Philipp Müller <tim centricular net>
 * Copyright (C) 2008 Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Based on the speexdec element.
 */

/**
 * SECTION:element-opusdec
 * @see_also: opusenc, oggdemux
 *
 * This element decodes a OPUS stream to raw integer audio.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch -v filesrc location=opus.ogg ! oggdemux ! opusdec ! audioconvert ! audioresample ! alsasink
 * ]| Decode an Ogg/Opus file. To create an Ogg/Opus file refer to the documentation of opusenc.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <gst/tag/tag.h>
#include "gstopusheader.h"
#include "gstopusdec.h"

GST_DEBUG_CATEGORY_STATIC (opusdec_debug);
#define GST_CAT_DEFAULT opusdec_debug

static GstStaticPadTemplate opus_dec_src_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw-int, "
        "rate = (int) { 8000, 12000, 16000, 24000, 48000 }, "
        "channels = (int) [ 1, 2 ], "
        "endianness = (int) BYTE_ORDER, "
        "signed = (boolean) true, " "width = (int) 16, " "depth = (int) 16")
    );

static GstStaticPadTemplate opus_dec_sink_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-opus")
    );

#define DEFAULT_USE_INBAND_FEC FALSE

enum
{
  PROP_0,
  PROP_USE_INBAND_FEC
};

GST_BOILERPLATE (GstOpusDec, gst_opus_dec, GstAudioDecoder,
    GST_TYPE_AUDIO_DECODER);

static GstFlowReturn gst_opus_dec_parse_header (GstOpusDec * dec,
    GstBuffer * buf);
static gboolean gst_opus_dec_start (GstAudioDecoder * dec);
static gboolean gst_opus_dec_stop (GstAudioDecoder * dec);
static GstFlowReturn gst_opus_dec_handle_frame (GstAudioDecoder * dec,
    GstBuffer * buffer);
static gboolean gst_opus_dec_set_format (GstAudioDecoder * bdec,
    GstCaps * caps);
static void gst_opus_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_opus_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);


static void
gst_opus_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&opus_dec_src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&opus_dec_sink_factory));
  gst_element_class_set_details_simple (element_class, "Opus audio decoder",
      "Codec/Decoder/Audio",
      "decode opus streams to audio",
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");
}

static void
gst_opus_dec_class_init (GstOpusDecClass * klass)
{
  GObjectClass *gobject_class;
  GstAudioDecoderClass *adclass;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  adclass = (GstAudioDecoderClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_opus_dec_set_property;
  gobject_class->get_property = gst_opus_dec_get_property;

  adclass->start = GST_DEBUG_FUNCPTR (gst_opus_dec_start);
  adclass->stop = GST_DEBUG_FUNCPTR (gst_opus_dec_stop);
  adclass->handle_frame = GST_DEBUG_FUNCPTR (gst_opus_dec_handle_frame);
  adclass->set_format = GST_DEBUG_FUNCPTR (gst_opus_dec_set_format);

  g_object_class_install_property (gobject_class, PROP_USE_INBAND_FEC,
      g_param_spec_boolean ("use-inband-fec", "Use in-band FEC",
          "Use forward error correction if available", DEFAULT_USE_INBAND_FEC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (opusdec_debug, "opusdec", 0,
      "opus decoding element");
}

static void
gst_opus_dec_reset (GstOpusDec * dec)
{
  dec->packetno = 0;
  if (dec->state) {
    opus_decoder_destroy (dec->state);
    dec->state = NULL;
  }

  gst_buffer_replace (&dec->streamheader, NULL);
  gst_buffer_replace (&dec->vorbiscomment, NULL);
  gst_buffer_replace (&dec->last_buffer, NULL);
  dec->primed = FALSE;

  dec->pre_skip = 0;
}

static void
gst_opus_dec_init (GstOpusDec * dec, GstOpusDecClass * g_class)
{
  dec->sample_rate = 0;
  dec->n_channels = 0;
  dec->use_inband_fec = FALSE;

  gst_opus_dec_reset (dec);
}

static gboolean
gst_opus_dec_start (GstAudioDecoder * dec)
{
  GstOpusDec *odec = GST_OPUS_DEC (dec);

  gst_opus_dec_reset (odec);

  /* we know about concealment */
  gst_audio_decoder_set_plc_aware (dec, TRUE);

  if (odec->use_inband_fec) {
    gst_audio_decoder_set_latency (dec, 2 * GST_MSECOND + GST_MSECOND / 2,
        120 * GST_MSECOND);
  }

  return TRUE;
}

static gboolean
gst_opus_dec_stop (GstAudioDecoder * dec)
{
  GstOpusDec *odec = GST_OPUS_DEC (dec);

  gst_opus_dec_reset (odec);

  return TRUE;
}

static GstFlowReturn
gst_opus_dec_parse_header (GstOpusDec * dec, GstBuffer * buf)
{
  g_return_val_if_fail (gst_opus_header_is_header (buf, "OpusHead", 8),
      GST_FLOW_ERROR);
  g_return_val_if_fail (GST_BUFFER_SIZE (buf) >= 19, GST_FLOW_ERROR);

  dec->pre_skip = GST_READ_UINT16_LE (GST_BUFFER_DATA (buf) + 10);
  GST_INFO_OBJECT (dec, "Found pre-skip of %u samples", dec->pre_skip);

  return GST_FLOW_OK;
}


static GstFlowReturn
gst_opus_dec_parse_comments (GstOpusDec * dec, GstBuffer * buf)
{
  return GST_FLOW_OK;
}

static void
gst_opus_dec_setup_from_peer_caps (GstOpusDec * dec)
{
  GstPad *srcpad, *peer;
  GstStructure *s;
  GstCaps *caps;
  const GstCaps *template_caps;
  const GstCaps *peer_caps;

  srcpad = GST_AUDIO_DECODER_SRC_PAD (dec);
  peer = gst_pad_get_peer (srcpad);

  if (peer) {
    template_caps = gst_pad_get_pad_template_caps (srcpad);
    peer_caps = gst_pad_get_caps (peer);
    GST_DEBUG_OBJECT (dec, "Peer caps: %" GST_PTR_FORMAT, peer_caps);
    caps = gst_caps_intersect (template_caps, peer_caps);
    gst_pad_fixate_caps (peer, caps);
    GST_DEBUG_OBJECT (dec, "Fixated caps: %" GST_PTR_FORMAT, caps);

    s = gst_caps_get_structure (caps, 0);
    if (!gst_structure_get_int (s, "channels", &dec->n_channels)) {
      dec->n_channels = 2;
      GST_WARNING_OBJECT (dec, "Failed to get channels, using default %d",
          dec->n_channels);
    } else {
      GST_DEBUG_OBJECT (dec, "Got channels %d", dec->n_channels);
    }
    if (!gst_structure_get_int (s, "rate", &dec->sample_rate)) {
      dec->sample_rate = 48000;
      GST_WARNING_OBJECT (dec, "Failed to get rate, using default %d",
          dec->sample_rate);
    } else {
      GST_DEBUG_OBJECT (dec, "Got sample rate %d", dec->sample_rate);
    }

    gst_pad_set_caps (GST_AUDIO_DECODER_SRC_PAD (dec), caps);
  } else {
    GST_WARNING_OBJECT (dec, "Failed to get src pad peer");
  }
}

static GstFlowReturn
opus_dec_chain_parse_data (GstOpusDec * dec, GstBuffer * buffer)
{
  GstFlowReturn res = GST_FLOW_OK;
  gint size;
  guint8 *data;
  GstBuffer *outbuf;
  gint16 *out_data;
  int n, err;
  int samples;
  unsigned int packet_size;
  GstBuffer *buf;

  if (dec->state == NULL) {
    gst_opus_dec_setup_from_peer_caps (dec);

    GST_DEBUG_OBJECT (dec, "Creating decoder with %d channels, %d Hz",
        dec->n_channels, dec->sample_rate);
    dec->state = opus_decoder_create (dec->sample_rate, dec->n_channels, &err);
    if (!dec->state || err != OPUS_OK)
      goto creation_failed;
  }

  if (buffer) {
    GST_DEBUG_OBJECT (dec, "Received buffer of size %u",
        GST_BUFFER_SIZE (buffer));
  } else {
    GST_DEBUG_OBJECT (dec, "Received missing buffer");
  }

  /* if using in-band FEC, we introdude one extra frame's delay as we need
     to potentially wait for next buffer to decode a missing buffer */
  if (dec->use_inband_fec && !dec->primed) {
    GST_DEBUG_OBJECT (dec, "First buffer received in FEC mode, early out");
    goto done;
  }

  /* That's the buffer we'll be sending to the opus decoder. */
  buf = dec->use_inband_fec && dec->last_buffer ? dec->last_buffer : buffer;

  if (buf) {
    data = GST_BUFFER_DATA (buf);
    size = GST_BUFFER_SIZE (buf);
    GST_DEBUG_OBJECT (dec, "Using buffer of size %u", size);
  } else {
    /* concealment data, pass NULL as the bits parameters */
    GST_DEBUG_OBJECT (dec, "Using NULL buffer");
    data = NULL;
    size = 0;
  }

  /* use maximum size (120 ms) as the number of returned samples is
     not constant over the stream. */
  samples = 120 * dec->sample_rate / 1000;
  packet_size = samples * dec->n_channels * 2;

  res = gst_pad_alloc_buffer_and_set_caps (GST_AUDIO_DECODER_SRC_PAD (dec),
      GST_BUFFER_OFFSET_NONE, packet_size,
      GST_PAD_CAPS (GST_AUDIO_DECODER_SRC_PAD (dec)), &outbuf);

  if (res != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (dec, "buf alloc flow: %s", gst_flow_get_name (res));
    return res;
  }

  out_data = (gint16 *) GST_BUFFER_DATA (outbuf);

  if (dec->use_inband_fec) {
    if (dec->last_buffer) {
      /* normal delayed decode */
      n = opus_decode (dec->state, data, size, out_data, samples, 0);
    } else {
      /* FEC reconstruction decode */
      n = opus_decode (dec->state, data, size, out_data, samples, 1);
    }
  } else {
    /* normal decode */
    n = opus_decode (dec->state, data, size, out_data, samples, 0);
  }

  if (n < 0) {
    GST_ELEMENT_ERROR (dec, STREAM, DECODE, ("Decoding error: %d", n), (NULL));
    return GST_FLOW_ERROR;
  }
  GST_DEBUG_OBJECT (dec, "decoded %d samples", n);
  GST_BUFFER_SIZE (outbuf) = n * 2 * dec->n_channels;

  /* Skip any samples that need skipping */
  if (dec->pre_skip > 0) {
    guint scaled_pre_skip = dec->pre_skip * dec->sample_rate / 48000;
    guint skip = scaled_pre_skip > n ? n : scaled_pre_skip;
    guint scaled_skip = skip * 48000 / dec->sample_rate;
    GST_BUFFER_SIZE (outbuf) -= skip * 2 * dec->n_channels;
    GST_BUFFER_DATA (outbuf) += skip * 2 * dec->n_channels;
    dec->pre_skip -= scaled_skip;
    GST_INFO_OBJECT (dec,
        "Skipping %u samples (%u at 48000 Hz, %u left to skip)", skip,
        scaled_skip, dec->pre_skip);

    if (GST_BUFFER_SIZE (outbuf) == 0) {
      gst_buffer_unref (outbuf);
      outbuf = NULL;
    }
  }

  res = gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (dec), outbuf, 1);

  if (res != GST_FLOW_OK)
    GST_DEBUG_OBJECT (dec, "flow: %s", gst_flow_get_name (res));

done:
  if (dec->use_inband_fec) {
    gst_buffer_replace (&dec->last_buffer, buffer);
    dec->primed = TRUE;
  }

  return res;

creation_failed:
  GST_ERROR_OBJECT (dec, "Failed to create Opus decoder: %d", err);
  return GST_FLOW_ERROR;
}

static gboolean
gst_opus_dec_set_format (GstAudioDecoder * bdec, GstCaps * caps)
{
  GstOpusDec *dec = GST_OPUS_DEC (bdec);
  gboolean ret = TRUE;
  GstStructure *s;
  const GValue *streamheader;

  GST_DEBUG_OBJECT (dec, "set_format: %" GST_PTR_FORMAT, caps);

  s = gst_caps_get_structure (caps, 0);
  if ((streamheader = gst_structure_get_value (s, "streamheader")) &&
      G_VALUE_HOLDS (streamheader, GST_TYPE_ARRAY) &&
      gst_value_array_get_size (streamheader) >= 2) {
    const GValue *header, *vorbiscomment;
    GstBuffer *buf;
    GstFlowReturn res = GST_FLOW_OK;

    header = gst_value_array_get_value (streamheader, 0);
    if (header && G_VALUE_HOLDS (header, GST_TYPE_BUFFER)) {
      buf = gst_value_get_buffer (header);
      res = gst_opus_dec_parse_header (dec, buf);
      if (res != GST_FLOW_OK)
        goto done;
      gst_buffer_replace (&dec->streamheader, buf);
    }

    vorbiscomment = gst_value_array_get_value (streamheader, 1);
    if (vorbiscomment && G_VALUE_HOLDS (vorbiscomment, GST_TYPE_BUFFER)) {
      buf = gst_value_get_buffer (vorbiscomment);
      res = gst_opus_dec_parse_comments (dec, buf);
      if (res != GST_FLOW_OK)
        goto done;
      gst_buffer_replace (&dec->vorbiscomment, buf);
    }
  }

done:
  return ret;
}

static gboolean
memcmp_buffers (GstBuffer * buf1, GstBuffer * buf2)
{
  gsize size1, size2;

  size1 = GST_BUFFER_SIZE (buf1);
  size2 = GST_BUFFER_SIZE (buf2);

  if (size1 != size2)
    return FALSE;

  return !memcmp (GST_BUFFER_DATA (buf1), GST_BUFFER_DATA (buf2), size1);
}

static GstFlowReturn
gst_opus_dec_handle_frame (GstAudioDecoder * adec, GstBuffer * buf)
{
  GstFlowReturn res;
  GstOpusDec *dec;

  /* no fancy draining */
  if (G_UNLIKELY (!buf))
    return GST_FLOW_OK;

  dec = GST_OPUS_DEC (adec);
  GST_LOG_OBJECT (dec,
      "Got buffer ts %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buf)));

  /* If we have the streamheader and vorbiscomment from the caps already
   * ignore them here */
  if (dec->streamheader && dec->vorbiscomment) {
    if (memcmp_buffers (dec->streamheader, buf)) {
      GST_DEBUG_OBJECT (dec, "found streamheader");
      gst_audio_decoder_finish_frame (adec, NULL, 1);
      res = GST_FLOW_OK;
    } else if (memcmp_buffers (dec->vorbiscomment, buf)) {
      GST_DEBUG_OBJECT (dec, "found vorbiscomments");
      gst_audio_decoder_finish_frame (adec, NULL, 1);
      res = GST_FLOW_OK;
    } else {
      res = opus_dec_chain_parse_data (dec, buf);
    }
  } else {
    /* Otherwise fall back to packet counting and assume that the
     * first two packets might be the headers, checking magic. */
    switch (dec->packetno) {
      case 0:
        if (gst_opus_header_is_header (buf, "OpusHead", 8)) {
          GST_DEBUG_OBJECT (dec, "found streamheader");
          res = gst_opus_dec_parse_header (dec, buf);
          gst_audio_decoder_finish_frame (adec, NULL, 1);
        } else {
          res = opus_dec_chain_parse_data (dec, buf);
        }
        break;
      case 1:
        if (gst_opus_header_is_header (buf, "OpusTags", 8)) {
          GST_DEBUG_OBJECT (dec, "counted vorbiscomments");
          res = gst_opus_dec_parse_comments (dec, buf);
          gst_audio_decoder_finish_frame (adec, NULL, 1);
        } else {
          res = opus_dec_chain_parse_data (dec, buf);
        }
        break;
      default:
      {
        res = opus_dec_chain_parse_data (dec, buf);
        break;
      }
    }
  }

  dec->packetno++;

  return res;
}

static void
gst_opus_dec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstOpusDec *dec = GST_OPUS_DEC (object);

  switch (prop_id) {
    case PROP_USE_INBAND_FEC:
      g_value_set_boolean (value, dec->use_inband_fec);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_opus_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOpusDec *dec = GST_OPUS_DEC (object);

  switch (prop_id) {
    case PROP_USE_INBAND_FEC:
      dec->use_inband_fec = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
