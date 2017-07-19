/*
 * Blackmagic DeckLink input
 * Copyright (c) 2013-2014 Luca Barbato, Deti Fliegl
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

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPI.h>

extern "C" {
#include "config.h"
#include "libavformat/avformat.h"
#include "libavutil/avassert.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libavutil/mathematics.h"
#include "libavutil/reverse.h"
#if CONFIG_LIBZVBI
#include <libzvbi.h>
#endif
}

#include "decklink_common.h"
#include "decklink_dec.h"

static uint8_t calc_parity_and_line_offset(int line)
{
    uint8_t ret = (line < 313) << 5;
    if (line >= 7 && line <= 22)
        ret += line;
    if (line >= 320 && line <= 335)
        ret += (line - 313);
    return ret;
}

static void fill_data_unit_head(int line, uint8_t *tgt)
{
    tgt[0] = 0x02; // data_unit_id
    tgt[1] = 0x2c; // data_unit_length
    tgt[2] = calc_parity_and_line_offset(line); // field_parity, line_offset
    tgt[3] = 0xe4; // framing code
}

#if CONFIG_LIBZVBI
static uint8_t* teletext_data_unit_from_vbi_data(int line, uint8_t *src, uint8_t *tgt, vbi_pixfmt fmt)
{
    vbi_bit_slicer slicer;

    vbi_bit_slicer_init(&slicer, 720, 13500000, 6937500, 6937500, 0x00aaaae4, 0xffff, 18, 6, 42 * 8, VBI_MODULATION_NRZ_MSB, fmt);

    if (vbi_bit_slice(&slicer, src, tgt + 4) == FALSE)
        return tgt;

    fill_data_unit_head(line, tgt);

    return tgt + 46;
}

static uint8_t* teletext_data_unit_from_vbi_data_10bit(int line, uint8_t *src, uint8_t *tgt)
{
    uint8_t y[720];
    uint8_t *py = y;
    uint8_t *pend = y + 720;
    /* The 10-bit VBI data is packed in V210, but libzvbi only supports 8-bit,
     * so we extract the 8 MSBs of the luma component, that is enough for
     * teletext bit slicing. */
    while (py < pend) {
        *py++ = (src[1] >> 4) + ((src[2] & 15) << 4);
        *py++ = (src[4] >> 2) + ((src[5] & 3 ) << 6);
        *py++ = (src[6] >> 6) + ((src[7] & 63) << 2);
        src += 8;
    }
    return teletext_data_unit_from_vbi_data(line, y, tgt, VBI_PIXFMT_YUV420);
}
#endif

static uint8_t* teletext_data_unit_from_op47_vbi_packet(int line, uint16_t *py, uint8_t *tgt)
{
    int i;

    if (py[0] != 0x255 || py[1] != 0x255 || py[2] != 0x227)
        return tgt;

    fill_data_unit_head(line, tgt);

    py += 3;
    tgt += 4;

    for (i = 0; i < 42; i++)
       *tgt++ = ff_reverse[py[i] & 255];

    return tgt;
}

static int linemask_matches(int line, int64_t mask)
{
    int shift = -1;
    if (line >= 6 && line <= 22)
        shift = line - 6;
    if (line >= 318 && line <= 335)
        shift = line - 318 + 17;
    return shift >= 0 && ((1ULL << shift) & mask);
}

static uint8_t* teletext_data_unit_from_op47_data(uint16_t *py, uint16_t *pend, uint8_t *tgt, int64_t wanted_lines)
{
    if (py < pend - 9) {
        if (py[0] == 0x151 && py[1] == 0x115 && py[3] == 0x102) {       // identifier, identifier, format code for WST teletext
            uint16_t *descriptors = py + 4;
            int i;
            py += 9;
            for (i = 0; i < 5 && py < pend - 45; i++, py += 45) {
                int line = (descriptors[i] & 31) + (!(descriptors[i] & 128)) * 313;
                if (line && linemask_matches(line, wanted_lines))
                    tgt = teletext_data_unit_from_op47_vbi_packet(line, py, tgt);
            }
        }
    }
    return tgt;
}

static uint8_t* teletext_data_unit_from_ancillary_packet(uint16_t *py, uint16_t *pend, uint8_t *tgt, int64_t wanted_lines, int allow_multipacket)
{
    uint16_t did = py[0];                                               // data id
    uint16_t sdid = py[1];                                              // secondary data id
    uint16_t dc = py[2] & 255;                                          // data count
    py += 3;
    pend = FFMIN(pend, py + dc);
    if (did == 0x143 && sdid == 0x102) {                                // subtitle distribution packet
        tgt = teletext_data_unit_from_op47_data(py, pend, tgt, wanted_lines);
    } else if (allow_multipacket && did == 0x143 && sdid == 0x203) {    // VANC multipacket
        py += 2;                                                        // priority, line/field
        while (py < pend - 3) {
            tgt = teletext_data_unit_from_ancillary_packet(py, pend, tgt, wanted_lines, 0);
            py += 4 + (py[2] & 255);                                    // ndid, nsdid, ndc, line/field
        }
    }
    return tgt;
}

static uint8_t* teletext_data_unit_from_vanc_data(uint8_t *src, uint8_t *tgt, int64_t wanted_lines)
{
    uint16_t y[1920];
    uint16_t *py = y;
    uint16_t *pend = y + 1920;
    /* The 10-bit VANC data is packed in V210, we only need the luma component. */
    while (py < pend) {
        *py++ = (src[1] >> 2) + ((src[2] & 15) << 6);
        *py++ =  src[4]       + ((src[5] &  3) << 8);
        *py++ = (src[6] >> 4) + ((src[7] & 63) << 4);
        src += 8;
    }
    py = y;
    while (py < pend - 6) {
        if (py[0] == 0 && py[1] == 0x3ff && py[2] == 0x3ff) {           // ancillary data flag
            py += 3;
            tgt = teletext_data_unit_from_ancillary_packet(py, pend, tgt, wanted_lines, 0);
            py += py[2] & 255;
        } else {
            py++;
        }
    }
    return tgt;
}

static void avpacket_queue_init(AVFormatContext *avctx, AVPacketQueue *q)
{
    memset(q, 0, sizeof(AVPacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->avctx = avctx;
}

static void avpacket_queue_flush(AVPacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt   = NULL;
    q->first_pkt  = NULL;
    q->nb_packets = 0;
    q->size       = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void avpacket_queue_end(AVPacketQueue *q)
{
    avpacket_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static unsigned long long avpacket_queue_size(AVPacketQueue *q)
{
    unsigned long long size;
    pthread_mutex_lock(&q->mutex);
    size = q->size;
    pthread_mutex_unlock(&q->mutex);
    return size;
}

static int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    // Drop Packet if queue size is > 1GB
    if (avpacket_queue_size(q) >  1024 * 1024 * 1024 ) {
        av_log(q->avctx, AV_LOG_WARNING,  "Decklink input buffer overrun!\n");
        return -1;
    }
    /* duplicate the packet */
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }
    pkt1->pkt  = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;; ) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt     = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

class decklink_input_callback : public IDeckLinkInputCallback
{
public:
        decklink_input_callback(AVFormatContext *_avctx);
        ~decklink_input_callback();

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
        virtual ULONG STDMETHODCALLTYPE AddRef(void);
        virtual ULONG STDMETHODCALLTYPE  Release(void);
        virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
        virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

private:
        ULONG           m_refCount;
        pthread_mutex_t m_mutex;
        AVFormatContext *avctx;
        decklink_ctx    *ctx;
        int no_video;
        int64_t initial_video_pts;
        int64_t initial_audio_pts;
};

decklink_input_callback::decklink_input_callback(AVFormatContext *_avctx) : m_refCount(0)
{
    avctx = _avctx;
    decklink_cctx       *cctx = (struct decklink_cctx *)avctx->priv_data;
    ctx = (struct decklink_ctx *)cctx->ctx;
    no_video = 0;
    initial_audio_pts = initial_video_pts = AV_NOPTS_VALUE;
    pthread_mutex_init(&m_mutex, NULL);
}

decklink_input_callback::~decklink_input_callback()
{
    pthread_mutex_destroy(&m_mutex);
}

ULONG decklink_input_callback::AddRef(void)
{
    pthread_mutex_lock(&m_mutex);
    m_refCount++;
    pthread_mutex_unlock(&m_mutex);

    return (ULONG)m_refCount;
}

ULONG decklink_input_callback::Release(void)
{
    pthread_mutex_lock(&m_mutex);
    m_refCount--;
    pthread_mutex_unlock(&m_mutex);

    if (m_refCount == 0) {
        delete this;
        return 0;
    }

    return (ULONG)m_refCount;
}

static int64_t get_pkt_pts(IDeckLinkVideoInputFrame *videoFrame,
                           IDeckLinkAudioInputPacket *audioFrame,
                           int64_t wallclock,
                           DecklinkPtsSource pts_src,
                           AVRational time_base, int64_t *initial_pts)
{
    int64_t pts = AV_NOPTS_VALUE;
    BMDTimeValue bmd_pts;
    BMDTimeValue bmd_duration;
    HRESULT res = E_INVALIDARG;
    switch (pts_src) {
        case PTS_SRC_AUDIO:
            if (audioFrame)
                res = audioFrame->GetPacketTime(&bmd_pts, time_base.den);
            break;
        case PTS_SRC_VIDEO:
            if (videoFrame)
                res = videoFrame->GetStreamTime(&bmd_pts, &bmd_duration, time_base.den);
            break;
        case PTS_SRC_REFERENCE:
            if (videoFrame)
                res = videoFrame->GetHardwareReferenceTimestamp(time_base.den, &bmd_pts, &bmd_duration);
            break;
        case PTS_SRC_WALLCLOCK:
        {
            /* MSVC does not support compound literals like AV_TIME_BASE_Q
             * in C++ code (compiler error C4576) */
            AVRational timebase;
            timebase.num = 1;
            timebase.den = AV_TIME_BASE;
            pts = av_rescale_q(wallclock, timebase, time_base);
            break;
        }
    }
    if (res == S_OK)
        pts = bmd_pts / time_base.num;

    if (pts != AV_NOPTS_VALUE && *initial_pts == AV_NOPTS_VALUE)
        *initial_pts = pts;
    if (*initial_pts != AV_NOPTS_VALUE)
        pts -= *initial_pts;

    return pts;
}

HRESULT decklink_input_callback::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame *videoFrame, IDeckLinkAudioInputPacket *audioFrame)
{
    void *frameBytes;
    void *audioFrameBytes;
    BMDTimeValue frameTime;
    BMDTimeValue frameDuration;
    int64_t wallclock = 0;

    ctx->frameCount++;
    if (ctx->audio_pts_source == PTS_SRC_WALLCLOCK || ctx->video_pts_source == PTS_SRC_WALLCLOCK)
        wallclock = av_gettime_relative();

    // Handle Video Frame
    if (videoFrame) {
        AVPacket pkt;
        av_init_packet(&pkt);
        if (ctx->frameCount % 25 == 0) {
            unsigned long long qsize = avpacket_queue_size(&ctx->queue);
            av_log(avctx, AV_LOG_DEBUG,
                    "Frame received (#%lu) - Valid (%liB) - QSize %fMB\n",
                    ctx->frameCount,
                    videoFrame->GetRowBytes() * videoFrame->GetHeight(),
                    (double)qsize / 1024 / 1024);
        }

        videoFrame->GetBytes(&frameBytes);
        videoFrame->GetStreamTime(&frameTime, &frameDuration,
                                  ctx->video_st->time_base.den);

        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
            if (ctx->draw_bars && videoFrame->GetPixelFormat() == bmdFormat8BitYUV) {
                unsigned bars[8] = {
                    0xEA80EA80, 0xD292D210, 0xA910A9A5, 0x90229035,
                    0x6ADD6ACA, 0x51EF515A, 0x286D28EF, 0x10801080 };
                int width  = videoFrame->GetWidth();
                int height = videoFrame->GetHeight();
                unsigned *p = (unsigned *)frameBytes;

                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x += 2)
                        *p++ = bars[(x * 8) / width];
                }
            }

            if (!no_video) {
                av_log(avctx, AV_LOG_WARNING, "Frame received (#%lu) - No input signal detected "
                        "- Frames dropped %u\n", ctx->frameCount, ++ctx->dropped);
            }
            no_video = 1;
        } else {
            if (no_video) {
                av_log(avctx, AV_LOG_WARNING, "Frame received (#%lu) - Input returned "
                        "- Frames dropped %u\n", ctx->frameCount, ++ctx->dropped);
            }
            no_video = 0;
        }

        pkt.pts = get_pkt_pts(videoFrame, audioFrame, wallclock, ctx->video_pts_source, ctx->video_st->time_base, &initial_video_pts);
        pkt.dts = pkt.pts;

        pkt.duration = frameDuration;
        //To be made sure it still applies
        pkt.flags       |= AV_PKT_FLAG_KEY;
        pkt.stream_index = ctx->video_st->index;
        pkt.data         = (uint8_t *)frameBytes;
        pkt.size         = videoFrame->GetRowBytes() *
                           videoFrame->GetHeight();
        //fprintf(stderr,"Video Frame size %d ts %d\n", pkt.size, pkt.pts);

        if (!no_video && ctx->teletext_lines) {
            IDeckLinkVideoFrameAncillary *vanc;
            AVPacket txt_pkt;
            uint8_t txt_buf0[3531]; // 35 * 46 bytes decoded teletext lines + 1 byte data_identifier + 1920 bytes OP47 decode buffer
            uint8_t *txt_buf = txt_buf0;

            if (videoFrame->GetAncillaryData(&vanc) == S_OK) {
                int i;
                int64_t line_mask = 1;
                BMDPixelFormat vanc_format = vanc->GetPixelFormat();
                txt_buf[0] = 0x10;    // data_identifier - EBU_data
                txt_buf++;
#if CONFIG_LIBZVBI
                if (ctx->bmd_mode == bmdModePAL && (vanc_format == bmdFormat8BitYUV || vanc_format == bmdFormat10BitYUV)) {
                    av_assert0(videoFrame->GetWidth() == 720);
                    for (i = 6; i < 336; i++, line_mask <<= 1) {
                        uint8_t *buf;
                        if ((ctx->teletext_lines & line_mask) && vanc->GetBufferForVerticalBlankingLine(i, (void**)&buf) == S_OK) {
                            if (vanc_format == bmdFormat8BitYUV)
                                txt_buf = teletext_data_unit_from_vbi_data(i, buf, txt_buf, VBI_PIXFMT_UYVY);
                            else
                                txt_buf = teletext_data_unit_from_vbi_data_10bit(i, buf, txt_buf);
                        }
                        if (i == 22)
                            i = 317;
                    }
                }
#endif
                if (videoFrame->GetWidth() == 1920 && vanc_format == bmdFormat10BitYUV) {
                    int first_active_line = ctx->bmd_field_dominance == bmdProgressiveFrame ? 42 : 584;
                    for (i = 8; i < first_active_line; i++) {
                        uint8_t *buf;
                        if (vanc->GetBufferForVerticalBlankingLine(i, (void**)&buf) == S_OK)
                            txt_buf = teletext_data_unit_from_vanc_data(buf, txt_buf, ctx->teletext_lines);
                        if (ctx->bmd_field_dominance != bmdProgressiveFrame && i == 20)     // skip field1 active lines
                            i = 569;
                        if (txt_buf - txt_buf0 > 1611) {   // ensure we still have at least 1920 bytes free in the buffer
                            av_log(avctx, AV_LOG_ERROR, "Too many OP47 teletext packets.\n");
                            break;
                        }
                    }
                }
                vanc->Release();
                if (txt_buf - txt_buf0 > 1) {
                    int stuffing_units = (4 - ((45 + txt_buf - txt_buf0) / 46) % 4) % 4;
                    while (stuffing_units--) {
                        memset(txt_buf, 0xff, 46);
                        txt_buf[1] = 0x2c; // data_unit_length
                        txt_buf += 46;
                    }
                    av_init_packet(&txt_pkt);
                    txt_pkt.pts = pkt.pts;
                    txt_pkt.dts = pkt.dts;
                    txt_pkt.stream_index = ctx->teletext_st->index;
                    txt_pkt.data = txt_buf0;
                    txt_pkt.size = txt_buf - txt_buf0;
                    if (avpacket_queue_put(&ctx->queue, &txt_pkt) < 0) {
                        ++ctx->dropped;
                    }
                }
            }
        }

        if (avpacket_queue_put(&ctx->queue, &pkt) < 0) {
            ++ctx->dropped;
        }
    }

    // Handle Audio Frame
    if (audioFrame) {
        AVPacket pkt;
        BMDTimeValue audio_pts;
        av_init_packet(&pkt);

        //hack among hacks
        pkt.size = audioFrame->GetSampleFrameCount() * ctx->audio_st->codecpar->channels * (16 / 8);
        audioFrame->GetBytes(&audioFrameBytes);
        audioFrame->GetPacketTime(&audio_pts, ctx->audio_st->time_base.den);
        pkt.pts = get_pkt_pts(videoFrame, audioFrame, wallclock, ctx->audio_pts_source, ctx->audio_st->time_base, &initial_audio_pts);
        pkt.dts = pkt.pts;

        //fprintf(stderr,"Audio Frame size %d ts %d\n", pkt.size, pkt.pts);
        pkt.flags       |= AV_PKT_FLAG_KEY;
        pkt.stream_index = ctx->audio_st->index;
        pkt.data         = (uint8_t *)audioFrameBytes;

        if (avpacket_queue_put(&ctx->queue, &pkt) < 0) {
            ++ctx->dropped;
        }
    }

    return S_OK;
}

HRESULT decklink_input_callback::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode,
    BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

static HRESULT decklink_start_input(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    ctx->input_callback = new decklink_input_callback(avctx);
    ctx->dli->SetCallback(ctx->input_callback);
    return ctx->dli->StartStreams();
}

extern "C" {

av_cold int ff_decklink_read_close(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    if (ctx->capture_started) {
        ctx->dli->StopStreams();
        ctx->dli->DisableVideoInput();
        ctx->dli->DisableAudioInput();
    }

    ff_decklink_cleanup(avctx);
    avpacket_queue_end(&ctx->queue);

    av_freep(&cctx->ctx);

    return 0;
}

av_cold int ff_decklink_read_header(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx;
    AVStream *st;
    HRESULT result;
    char fname[1024];
    char *tmp;
    int mode_num = 0;
    int ret;

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->teletext_lines = cctx->teletext_lines;
    ctx->preroll      = cctx->preroll;
    ctx->duplex_mode  = cctx->duplex_mode;
    if (cctx->video_input > 0 && (unsigned int)cctx->video_input < FF_ARRAY_ELEMS(decklink_video_connection_map))
        ctx->video_input = decklink_video_connection_map[cctx->video_input];
    if (cctx->audio_input > 0 && (unsigned int)cctx->audio_input < FF_ARRAY_ELEMS(decklink_audio_connection_map))
        ctx->audio_input = decklink_audio_connection_map[cctx->audio_input];
    ctx->audio_pts_source = cctx->audio_pts_source;
    ctx->video_pts_source = cctx->video_pts_source;
    ctx->draw_bars = cctx->draw_bars;
    cctx->ctx = ctx;

    /* Check audio channel option for valid values: 2, 8 or 16 */
    switch (cctx->audio_channels) {
        case 2:
        case 8:
        case 16:
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Value of channels option must be one of 2, 8 or 16\n");
            return AVERROR(EINVAL);
    }

    /* List available devices. */
    if (ctx->list_devices) {
        ff_decklink_list_devices(avctx);
        return AVERROR_EXIT;
    }

    strcpy (fname, avctx->filename);
    tmp=strchr (fname, '@');
    if (tmp != NULL) {
        av_log(avctx, AV_LOG_WARNING, "The @mode syntax is deprecated and will be removed. Please use the -format_code option.\n");
        mode_num = atoi (tmp+1);
        *tmp = 0;
    }

    ret = ff_decklink_init_device(avctx, fname);
    if (ret < 0)
        return ret;

    /* Get input device. */
    if (ctx->dl->QueryInterface(IID_IDeckLinkInput, (void **) &ctx->dli) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not open input device from '%s'\n",
               avctx->filename);
        ret = AVERROR(EIO);
        goto error;
    }

    /* List supported formats. */
    if (ctx->list_formats) {
        ff_decklink_list_formats(avctx, DIRECTION_IN);
        ret = AVERROR_EXIT;
        goto error;
    }

    if (mode_num > 0 || cctx->format_code) {
        if (ff_decklink_set_format(avctx, DIRECTION_IN, mode_num) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not set mode number %d or format code %s for %s\n",
                mode_num, (cctx->format_code) ? cctx->format_code : "(unset)", fname);
            ret = AVERROR(EIO);
            goto error;
        }
    }

#if !CONFIG_LIBZVBI
    if (ctx->teletext_lines && ctx->bmd_mode == bmdModePAL) {
        av_log(avctx, AV_LOG_ERROR, "Libzvbi support is needed for capturing SD PAL teletext, please recompile FFmpeg.\n");
        ret = AVERROR(ENOSYS);
        goto error;
    }
#endif

    /* Setup streams. */
    st = avformat_new_stream(avctx, NULL);
    if (!st) {
        av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
        ret = AVERROR(ENOMEM);
        goto error;
    }
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_PCM_S16LE;
    st->codecpar->sample_rate = bmdAudioSampleRate48kHz;
    st->codecpar->channels    = cctx->audio_channels;
    avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */
    ctx->audio_st=st;

    st = avformat_new_stream(avctx, NULL);
    if (!st) {
        av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
        ret = AVERROR(ENOMEM);
        goto error;
    }
    st->codecpar->codec_type  = AVMEDIA_TYPE_VIDEO;
    st->codecpar->width       = ctx->bmd_width;
    st->codecpar->height      = ctx->bmd_height;

    st->time_base.den      = ctx->bmd_tb_den;
    st->time_base.num      = ctx->bmd_tb_num;
    av_stream_set_r_frame_rate(st, av_make_q(st->time_base.den, st->time_base.num));

    if (cctx->v210) {
        st->codecpar->codec_id    = AV_CODEC_ID_V210;
        st->codecpar->codec_tag   = MKTAG('V', '2', '1', '0');
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 64, st->time_base.den, st->time_base.num * 3);
    } else {
        st->codecpar->codec_id    = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->format      = AV_PIX_FMT_UYVY422;
        st->codecpar->codec_tag   = MKTAG('U', 'Y', 'V', 'Y');
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 16, st->time_base.den, st->time_base.num);
    }

    avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */

    ctx->video_st=st;

    if (ctx->teletext_lines) {
        st = avformat_new_stream(avctx, NULL);
        if (!st) {
            av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
            ret = AVERROR(ENOMEM);
            goto error;
        }
        st->codecpar->codec_type  = AVMEDIA_TYPE_SUBTITLE;
        st->time_base.den         = ctx->bmd_tb_den;
        st->time_base.num         = ctx->bmd_tb_num;
        st->codecpar->codec_id    = AV_CODEC_ID_DVB_TELETEXT;
        avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */
        ctx->teletext_st = st;
    }

    av_log(avctx, AV_LOG_VERBOSE, "Using %d input audio channels\n", ctx->audio_st->codecpar->channels);
    result = ctx->dli->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, ctx->audio_st->codecpar->channels);

    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot enable audio input\n");
        ret = AVERROR(EIO);
        goto error;
    }

    result = ctx->dli->EnableVideoInput(ctx->bmd_mode,
                                        cctx->v210 ? bmdFormat10BitYUV : bmdFormat8BitYUV,
                                        bmdVideoInputFlagDefault);

    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot enable video input\n");
        ret = AVERROR(EIO);
        goto error;
    }

    avpacket_queue_init (avctx, &ctx->queue);

    if (decklink_start_input (avctx) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot start input stream\n");
        ret = AVERROR(EIO);
        goto error;
    }

    return 0;

error:
    ff_decklink_cleanup(avctx);
    return ret;
}

int ff_decklink_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVFrame *frame = ctx->video_st->codec->coded_frame;

    avpacket_queue_get(&ctx->queue, pkt, 1);
    if (frame && (ctx->bmd_field_dominance == bmdUpperFieldFirst || ctx->bmd_field_dominance == bmdLowerFieldFirst)) {
        frame->interlaced_frame = 1;
        if (ctx->bmd_field_dominance == bmdUpperFieldFirst) {
            frame->top_field_first = 1;
        }
    }

    return 0;
}

} /* extern "C" */
