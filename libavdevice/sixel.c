/*
 * Copyright (c) 2014 Hayaki Saito
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

#include <stdio.h>
#include <unistd.h>
#include <sys/signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <sixel.h>
#include "avdevice.h"
#include "libavutil/pixdesc.h"
#include "libavformat/mux.h"
#include "libavutil/time.h"
#include "libavutil/mem.h"

#if !defined(SIXELAPI)
#  define LIBSIXEL_LEGACY_API
#  define SIXEL_OK    (0)
#  define SIXEL_FALSE (-1)
#  define SIXEL_SUCCEEDED(status) (((status) & 0x1000) == 0)
#  define SIXEL_FAILED(status)    (((status) & 0x1000) != 0)
typedef int SIXELSTATUS;
#endif

typedef struct SIXELContext {
    AVClass *class;
    AVRational time_base;   /* time base */
    int64_t    time_frame;  /* current time */
    AVRational framerate;
    int top;
    int left;
    int reqcolors;
    sixel_output_t *output;
    sixel_dither_t *dither;
    sixel_dither_t *testdither;
    int fixedpal;
    enum methodForDiffuse diffuse;
    int threshold;
    int dropframe;
    int ignoredelay;
    int encode_policy;
    int use_8bit_mode;
    int complexion_score;
    int method_for_rep;
    int64_t dropped_frames;
    int64_t rendered_frames;
    int64_t last_render_time;  /* Time when last frame was rendered */
    int64_t avg_render_time;   /* Moving average of render time */
    uint8_t *prev_frame;       /* Previous frame data for duplicate detection */
    int prev_frame_size;       /* Size of previous frame buffer */
    int64_t skipped_dup_frames; /* Count of skipped duplicate frames */
} SIXELContext;

static FILE *sixel_output_file = NULL;

static int detect_scene_change(SIXELContext *const c)
{
    int score;
    int i;
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    static unsigned int average_r = 0;
    static unsigned int average_g = 0;
    static unsigned int average_b = 0;
    static int previous_histgram_colors = 0;
    int histgram_colors = 0;
    int palette_colors = 0;
    unsigned char const* palette;

    histgram_colors = sixel_dither_get_num_of_histogram_colors(c->testdither);

    if (c->dither == NULL)
        goto detected;

    /* detect scene change if number of colors increses 20% */
    if (previous_histgram_colors * 6 < histgram_colors * 5)
        goto detected;

    /* detect scene change if number of colors decreses 20% */
    if (previous_histgram_colors * 4 > histgram_colors * 5)
        goto detected;

    palette_colors = sixel_dither_get_num_of_palette_colors(c->testdither);
    palette = sixel_dither_get_palette(c->testdither);

    /* compare color difference between current
     * palette and previous one */
    for (i = 0; i < palette_colors; i++) {
        r += palette[i * 3 + 0];
        g += palette[i * 3 + 1];
        b += palette[i * 3 + 2];
    }
    score = (r - average_r) * (r - average_r)
          + (g - average_g) * (g - average_g)
          + (b - average_b) * (b - average_b);
    if (score > c->threshold * palette_colors
                             * palette_colors)
        goto detected;

    return 0;

detected:
    previous_histgram_colors = histgram_colors;
    average_r = r;
    average_g = g;
    average_b = b;
    return 1;
}

static SIXELSTATUS prepare_static_palette(SIXELContext *const c,
                                          AVCodecParameters *const encctx)
{
    if (c->dither) {
        sixel_dither_set_body_only(c->dither, 1);
    } else {
        c->dither = sixel_dither_get(BUILTIN_XTERM256);
        if (c->dither == NULL)
            return SIXEL_FALSE;
        sixel_dither_set_diffusion_type(c->dither, c->diffuse);
        /* Enable palette optimization */
        sixel_dither_set_optimize_palette(c->dither, 1);
    }
    return SIXEL_OK;
}


static void scroll_on_demand(int pixelheight,
                             int specified_top,
                             int specified_left)
{
    struct winsize size = {0, 0, 0, 0};
    struct termios old_termios;
    struct termios new_termios;
    int top = 0;
    int left = 0;
    int cellheight;
    int scroll;
    fd_set rfds;
    struct timeval tv;
    int ret = 0;

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    if (size.ws_ypixel <= 0) {
        fprintf(sixel_output_file, "\033[H\0337");
        return;
    }
    /* set the terminal to cbreak mode */
    tcgetattr(STDIN_FILENO, &old_termios);
    memcpy(&new_termios, &old_termios, sizeof(old_termios));
    new_termios.c_lflag &= ~(ECHO | ICANON);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_termios);

    /* request cursor position report */
    fprintf(sixel_output_file, "\033[6n");
    /* wait 1 sec */
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    if (ret != (-1)) {
        if (scanf("\033[%d;%dR", &top, &left) == 2) {
            if (specified_top > 0)
                top = specified_top;
            if (specified_left > 0)
                left = specified_left;
            fprintf(sixel_output_file, "\033[%d;%dH", top, left);
            cellheight = pixelheight * size.ws_row / size.ws_ypixel + 1;
            scroll = cellheight + top - size.ws_row + 1;
            if (scroll > 0) {
                fprintf(sixel_output_file, "\033[%dS\033[%dA", scroll, scroll);
            }
            fprintf(sixel_output_file, "\0337");
        } else {
            if (specified_top > 0)
                top = specified_top;
            if (specified_left > 0)
                left = specified_left;
            if (top < 1)
                top = 1;
            if (left < 1)
                left = 1;
            fprintf(sixel_output_file, "\033[%d;%dH\0337", top, left);
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
}


static SIXELSTATUS prepare_dynamic_palette(SIXELContext *const c,
                                           AVCodecParameters *const encctx,
                                           AVPacket *const pkt)
{
    SIXELSTATUS status = SIXEL_FALSE;
    int pixelformat;
    
    /* Determine pixel format for libsixel */
    switch (encctx->format) {
        case AV_PIX_FMT_BGR24:
            pixelformat = SIXEL_PIXELFORMAT_BGR888;
            break;
        case AV_PIX_FMT_BGR0:
        case AV_PIX_FMT_BGRA:
            pixelformat = SIXEL_PIXELFORMAT_BGRA8888;
            break;
        case AV_PIX_FMT_RGB24:
        default:
            pixelformat = SIXEL_PIXELFORMAT_RGB888;
            break;
    }

    /* create histgram and construct color palette
     * with median cut algorithm.
     * Using LARGE_LUM for perceptually better color quantization
     * and REP_AVERAGE_PIXELS for more accurate representative colors. */
    status = sixel_dither_initialize(c->testdither, pkt->data,
                                     encctx->width, encctx->height, pixelformat,
                                     LARGE_LUM, 
                                     c->method_for_rep ? c->method_for_rep : REP_AVERAGE_PIXELS,
                                     QUALITY_FULL);
    if (SIXEL_FAILED(status))
        return status;

    /* Always create a new dither with fresh palette for each frame.
     * 
     * The original code had a scene change optimization that would reuse
     * the old dither palette when the scene didn't change (using body_only mode).
     * However, this causes color corruption when frames become stationary.
     * 
     * The issue appears to be that body_only mode in libsixel doesn't work
     * correctly for streaming video, possibly due to terminal state assumptions
     * or palette caching issues. Since correctness is more important than
     * the bandwidth savings from palette reuse, we always generate fresh palettes.
     */
    if (c->dither)
        sixel_dither_unref(c->dither);
    c->dither = c->testdither;
#if defined(LIBSIXEL_LEGACY_API)
    c->testdither = sixel_dither_create(c->reqcolors);
    if (c->testdither == NULL)
        return SIXEL_FALSE;
#else
    status = sixel_dither_new(&c->testdither, c->reqcolors, NULL);
    if (SIXEL_FAILED(status))
        return status;
#endif
    sixel_dither_set_diffusion_type(c->dither, c->diffuse);
    
    /* Set complexion score for better skin tone preservation */
    if (c->complexion_score > 0) {
        sixel_dither_set_complexion_score(c->dither, c->complexion_score);
    }
    
    /* Enable palette optimization to reduce palette to only used colors */
    sixel_dither_set_optimize_palette(c->dither, 1);

    return SIXEL_OK;
}

static int sixel_write(char *data, int size, void *priv)
{
    return fwrite(data, 1, size, (FILE *)priv);
}

static int sixel_write_header(AVFormatContext *s)
{
    SIXELContext *c = s->priv_data;
    AVCodecParameters *encctx = s->streams[0]->codecpar;
    SIXELSTATUS status = SIXEL_FALSE;

    if (s->nb_streams > 1
        || encctx->codec_type != AVMEDIA_TYPE_VIDEO
        || encctx->codec_id   != AV_CODEC_ID_RAWVIDEO) {
        av_log(s, AV_LOG_ERROR, "Only supports one rawvideo stream\n");
        return AVERROR(EINVAL);
    }

    if (encctx->format != AV_PIX_FMT_RGB24 && 
        encctx->format != AV_PIX_FMT_BGR24 &&
        encctx->format != AV_PIX_FMT_BGR0 &&
        encctx->format != AV_PIX_FMT_BGRA) {
        av_log(s, AV_LOG_ERROR,
               "Unsupported pixel format '%s', choose rgb24, bgr24, bgr0, or bgra\n",
               av_get_pix_fmt_name(encctx->format));
        return AVERROR(EINVAL);
    }

    if (!s->url || strcmp(s->url, "pipe:") == 0 || strcmp(s->url, "-")) {
        sixel_output_file = stdout;
#if defined(LIBSIXEL_LEGACY_API)
        c->output = sixel_output_create(sixel_write, stdout);
        status = c->output == NULL ? SIXEL_FALSE: SIXEL_OK;
#else
        status = sixel_output_new(&c->output, sixel_write, stdout, NULL);
#endif
    } else {
        sixel_output_file = fopen(s->url, "w");
#if defined(LIBSIXEL_LEGACY_API)
        c->output = sixel_output_create(sixel_write, sixel_output_file);
        status = c->output == NULL ? SIXEL_FALSE: SIXEL_OK;
#else
        status = sixel_output_new(&c->output, sixel_write, sixel_output_file, NULL);
#endif
    }

    if (SIXEL_FAILED(status)) {
#if !defined(LIBSIXEL_LEGACY_API)
        av_log(s, AV_LOG_ERROR, "%s\n", sixel_helper_format_error(status));
#endif
        return AVERROR_EXTERNAL;
    }

    /* Set encoding policy for compression */
    sixel_output_set_encode_policy(c->output, c->encode_policy);
    
    /* Use HLS color space for more perceptually uniform palette */
    sixel_output_set_palette_type(c->output, PALETTETYPE_HLS);
    
    /* Allow unlimited repeat counts for better RLE compression */
    sixel_output_set_gri_arg_limit(c->output, 0);
    
    /* Enable 8-bit mode for smaller escape sequences */
    if (c->use_8bit_mode) {
        sixel_output_set_8bit_availability(c->output, 1);
    }

    if (isatty(fileno(sixel_output_file))) {
        fprintf(sixel_output_file, "\033[?25l");      /* hide cursor */
    } else {
        c->ignoredelay = 1;
    }

    /* don't use private color registers for each frame. */
    fprintf(sixel_output_file, "\033[?1070l");

    c->dither = NULL;
#if defined(LIBSIXEL_LEGACY_API)
    c->testdither = sixel_dither_create(c->reqcolors);
    status = c->testdither == NULL ? SIXEL_FALSE: SIXEL_OK;
#else
    status = sixel_dither_new(&c->testdither, c->reqcolors, NULL);
#endif

    if (SIXEL_FAILED(status)) {
#if !defined(LIBSIXEL_LEGACY_API)
        av_log(s, AV_LOG_ERROR, "%s\n", sixel_helper_format_error(status));
#endif
        return AVERROR_EXTERNAL;
    }

    c->time_base = s->streams[0]->time_base;
    c->time_frame = av_gettime() / av_q2d(c->time_base);

    return 0;
}

static int sixel_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SIXELContext * const c = s->priv_data;
    AVCodecParameters * const encctx = s->streams[0]->codecpar;
    int64_t curtime, delay;
    int64_t render_start, render_end, render_time;
    struct timespec ts;
    static int dirty = 0;
    SIXELSTATUS status = SIXEL_FALSE;

    if (!c->ignoredelay) {
        /* calculate the time of the next frame */
        c->time_frame += INT64_C(1000000);
        curtime = av_gettime();
        delay = c->time_frame * av_q2d(c->time_base) - curtime;
        
        if (c->dropframe) {
            /* Aggressive frame dropping: drop if we're behind at all */
            if (delay <= 0) {
                c->dropped_frames++;
                return 0;
            }
            
            /* Also predict if we'll be late based on average render time */
            if (c->avg_render_time > 0) {
                int64_t predicted_finish = curtime + c->avg_render_time;
                int64_t frame_deadline = c->time_frame * av_q2d(c->time_base);
                
                if (predicted_finish > frame_deadline) {
                    /* We predict we'll be late - drop this frame preemptively */
                    c->dropped_frames++;
                    return 0;
                }
            }
            
            /* Don't sleep at all - render as fast as possible and let dropping keep us in sync */
        } else {
            /* Not in dropframe mode - use normal sleep logic */
            if (delay > 0) {
                ts.tv_sec = delay / 1000000;
                ts.tv_nsec = (delay % 1000000) * 1000;
                nanosleep(&ts, NULL);
            }
        }
    }

    /* Start measuring render time */
    render_start = av_gettime();
    
    /* Allocate or reallocate prev_frame buffer if needed */
    if (!c->prev_frame || pkt->size != c->prev_frame_size) {
        c->prev_frame = av_realloc(c->prev_frame, pkt->size);
        if (!c->prev_frame) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate duplicate detection buffer\n");
            c->prev_frame_size = 0;
            /* Continue without dup detection */
        } else {
            c->prev_frame_size = pkt->size;
        }
    }
    
    /* Check for duplicate frame using memcmp */
    if (c->prev_frame && pkt->size == c->prev_frame_size) {
        if (memcmp(pkt->data, c->prev_frame, pkt->size) == 0) {
            /* Frame is identical to previous - skip rendering */
            c->skipped_dup_frames++;
            return 0;
        }
    }

    if (dirty == 0) {
        scroll_on_demand(encctx->height, c->top, c->left);
        dirty = 1;
    }
    fprintf(sixel_output_file, "\0338");

    if (c->fixedpal) {
        status = prepare_static_palette(c, encctx);
    } else {
        status = prepare_dynamic_palette(c, encctx, pkt);
    }
    if (SIXEL_FAILED(status)) {
#if !defined(LIBSIXEL_LEGACY_API)
        av_log(s, AV_LOG_ERROR, "%s\n", sixel_helper_format_error(status));
#endif
        return AVERROR_EXTERNAL;
    }
    
    /* When dropframe is enabled, use scene detection to skip frames
     * that didn't meaningfully change */
    if (c->dropframe && !c->fixedpal && !detect_scene_change(c)) {
        /* No significant scene change detected - drop this frame */
        c->dropped_frames++;
        return 0;
    }
    
    /* Determine the correct pixel format for libsixel based on input format */
    int sixel_format;
    switch (encctx->format) {
        case AV_PIX_FMT_BGR24:
            sixel_format = SIXEL_PIXELFORMAT_BGR888;
            break;
        case AV_PIX_FMT_BGR0:
        case AV_PIX_FMT_BGRA:
            sixel_format = SIXEL_PIXELFORMAT_BGRA8888;
            break;
        case AV_PIX_FMT_RGB24:
        default:
            sixel_format = SIXEL_PIXELFORMAT_RGB888;
            break;
    }
    
    status = sixel_encode(pkt->data, encctx->width, encctx->height,
                          sixel_format,
                          c->dither, c->output);
    if (SIXEL_FAILED(status)) {
#if !defined(LIBSIXEL_LEGACY_API)
        av_log(s, AV_LOG_ERROR, "%s\n", sixel_helper_format_error(status));
#endif
        return AVERROR_EXTERNAL;
    }
    fflush(sixel_output_file);
    
    /* Save current frame for duplicate detection */
    if (c->prev_frame && c->prev_frame_size == pkt->size) {
        memcpy(c->prev_frame, pkt->data, pkt->size);
    }
    
    /* Measure render time and update moving average */
    render_end = av_gettime();
    render_time = render_end - render_start;
    
    /* Update moving average (exponential moving average with alpha=0.3) */
    if (c->avg_render_time == 0) {
        c->avg_render_time = render_time;  /* First frame */
    } else {
        c->avg_render_time = (c->avg_render_time * 7 + render_time * 3) / 10;
    }
    
    c->rendered_frames++;
    
    return 0;
}

static int sixel_write_trailer(AVFormatContext *s)
{
    return 0;
}

static void sixel_deinit(AVFormatContext *s) {
    SIXELContext * const c = s->priv_data;

    /* Print final statistics */
    if (c->dropframe) {
        int64_t total = c->rendered_frames + c->dropped_frames + c->skipped_dup_frames;
        double drop_pct = total > 0 ? (100.0 * c->dropped_frames / total) : 0.0;
        double dup_pct = total > 0 ? (100.0 * c->skipped_dup_frames / total) : 0.0;
        av_log(s, AV_LOG_INFO, "SIXEL final stats: rendered=%lld dropped=%lld (%.1f%%) skipped_dup=%lld (%.1f%%) total=%lld avg_render_time=%lld us\n",
               (long long)c->rendered_frames, (long long)c->dropped_frames, drop_pct, 
               (long long)c->skipped_dup_frames, dup_pct,
               (long long)total, (long long)c->avg_render_time);
    }

    if (isatty(fileno(sixel_output_file))) {
        fprintf(sixel_output_file,
                "\033\\"      /* terminate DCS sequence */
                "\033[?25h"); /* show cursor */
    }

    fflush(sixel_output_file);
    if (sixel_output_file && sixel_output_file != stdout) {
        fclose(sixel_output_file);
        sixel_output_file = NULL;
    }
    
    /* Free duplicate detection buffer */
    if (c->prev_frame) {
        av_free(c->prev_frame);
        c->prev_frame = NULL;
    }
    
    if (c->output) {
        sixel_output_unref(c->output);
        c->output = NULL;
    }
    if (c->testdither) {
        sixel_dither_unref(c->testdither);
        c->testdither = NULL;
    }
    if (c->dither) {
        sixel_dither_unref(c->dither);
        c->dither = NULL;
    }
}


#define OFFSET(x) offsetof(SIXELContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "left",            "left position",           OFFSET(left),            AV_OPT_TYPE_INT,    {.i64 = 0},                  0, 256,  ENC },
    { "top",             "top position",            OFFSET(top),             AV_OPT_TYPE_INT,    {.i64 = 0},                  0, 256,  ENC },
    { "reqcolors",       "number of colors",        OFFSET(reqcolors),       AV_OPT_TYPE_INT,    {.i64 = 16},                 2, 256,  ENC },
    { "fixedpal",        "use fixed palette",       OFFSET(fixedpal),        AV_OPT_TYPE_INT,    {.i64 = 0},                  0, 1,    ENC, "fixedpal" },
    { "true",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = 1},                  0, 0,    ENC, "fixedpal" },
    { "false",           NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = 0},                  0, 0,    ENC, "fixedpal" },
    { "diffuse",         "dithering method",        OFFSET(diffuse),         AV_OPT_TYPE_INT,    {.i64 = DIFFUSE_AUTO},       0, 8,    ENC, "diffuse" },
    { "auto",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_AUTO},       0, 0,    ENC, "diffuse" },
    { "none",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_NONE},       0, 0,    ENC, "diffuse" },
    { "atkinson",        NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_ATKINSON},   0, 0,    ENC, "diffuse" },
    { "fs",              NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_FS},         0, 0,    ENC, "diffuse" },
    { "jajuni",          NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_JAJUNI},     0, 0,    ENC, "diffuse" },
    { "stucki",          NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_STUCKI},     0, 0,    ENC, "diffuse" },
    { "burkes",          NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_BURKES},     0, 0,    ENC, "diffuse" },
    { "a_dither",        NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_A_DITHER},   0, 0,    ENC, "diffuse" },
    { "x_dither",        NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = DIFFUSE_X_DITHER},   0, 0,    ENC, "diffuse" },
    { "encode-policy",   "encoding policy",         OFFSET(encode_policy),   AV_OPT_TYPE_INT,    {.i64 = 2},                  0, 2,    ENC, "encode_policy" },
    { "auto",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = 0},                  0, 0,    ENC, "encode_policy" },
    { "fast",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = 1},                  0, 0,    ENC, "encode_policy" },
    { "size",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = 2},                  0, 0,    ENC, "encode_policy" },
    { "8bit-mode",       "use 8-bit control codes", OFFSET(use_8bit_mode),   AV_OPT_TYPE_BOOL,   {.i64 = 0},                  0, 1,    ENC },
    { "complexion-score", "skin tone improvement", OFFSET(complexion_score), AV_OPT_TYPE_INT,    {.i64 = 0},                  0, 100,  ENC },
    { "method-for-rep",  "color selection method",  OFFSET(method_for_rep),  AV_OPT_TYPE_INT,    {.i64 = REP_AVERAGE_PIXELS}, 0, 3,    ENC, "method_for_rep" },
    { "auto",            NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = REP_AUTO},           0, 0,    ENC, "method_for_rep" },
    { "center-box",      NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = REP_CENTER_BOX},     0, 0,    ENC, "method_for_rep" },
    { "average-colors",  NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = REP_AVERAGE_COLORS}, 0, 0,    ENC, "method_for_rep" },
    { "average-pixels",  NULL,                      0,                       AV_OPT_TYPE_CONST,  {.i64 = REP_AVERAGE_PIXELS}, 0, 0,    ENC, "method_for_rep" },
    { "dropframe",       "drop late frames",       OFFSET(dropframe),        AV_OPT_TYPE_BOOL,   {.i64 = 0},                  0, 1,    ENC, "dropframe" },
    { "ignoredelay",     "ignore frame timestamp", OFFSET(ignoredelay),      AV_OPT_TYPE_BOOL,   {.i64 = 0},                  0, 1,    ENC, "ignoredelay" },
#if 0  /* for debugging */
    { "scene-threshold", "scene change threshold", OFFSET(threshold),   AV_OPT_TYPE_INT,    {.i64 = 500},              0, 10000,ENC },
    { "true",            NULL,                     0,                   AV_OPT_TYPE_CONST,  {.i64 = 1},                0, 0,    ENC, "dropframe" },
    { "false",           NULL,                     0,                   AV_OPT_TYPE_CONST,  {.i64 = 0},                0, 0,    ENC, "dropframe" },
    { "true",            NULL,                     0,                   AV_OPT_TYPE_CONST,  {.i64 = 1},                0, 0,    ENC, "ignoredelay" },
    { "false",           NULL,                     0,                   AV_OPT_TYPE_CONST,  {.i64 = 0},                0, 0,    ENC, "ignoredelay" },
#endif
    { NULL },
};

static const AVClass sixel_class = {
    .class_name = "sixel_outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

static const AVClass sixel_vfr_class = {
    .class_name = "sixel_vfr_outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_sixel_muxer = {
    .p.name         = "sixel",
    .p.long_name    = NULL_IF_CONFIG_SMALL("SIXEL terminal device"),
    .priv_data_size = sizeof(SIXELContext),
    .p.audio_codec  = AV_CODEC_ID_NONE,
    .p.video_codec  = AV_CODEC_ID_RAWVIDEO,
    .write_header   = sixel_write_header,
    .write_packet   = sixel_write_packet,
    .write_trailer  = sixel_write_trailer,
    .deinit         = sixel_deinit,
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class     = &sixel_class,
};

const FFOutputFormat ff_sixel_vfr_muxer = {
    .p.name         = "sixel_vfr",
    .p.long_name    = NULL_IF_CONFIG_SMALL("SIXEL terminal device (variable framerate)"),
    .priv_data_size = sizeof(SIXELContext),
    .p.audio_codec  = AV_CODEC_ID_NONE,
    .p.video_codec  = AV_CODEC_ID_RAWVIDEO,
    .write_header   = sixel_write_header,
    .write_packet   = sixel_write_packet,
    .write_trailer  = sixel_write_trailer,
    .deinit         = sixel_deinit,
    .p.flags        = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &sixel_vfr_class,
};
