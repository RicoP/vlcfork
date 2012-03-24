/*****************************************************************************
 * demux.c : V4L2 raw video demux module for vlc
 *****************************************************************************
 * Copyright (C) 2002-2011 the VideoLAN team
 *
 * Authors: Benjamin Pracht <bigben at videolan dot org>
 *          Richard Hosking <richard at hovis dot net>
 *          Antoine Cellerier <dionoea at videolan d.t org>
 *          Dennis Lou <dlou99 at yahoo dot com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <math.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#include <vlc_common.h>
#include <vlc_demux.h>
#include <vlc_fs.h>

#include "v4l2.h"

static int DemuxControl( demux_t *, int, va_list );
static int Demux( demux_t * );
static int InitVideo (demux_t *, int);

int DemuxOpen( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;

    demux_sys_t *sys = calloc( 1, sizeof( demux_sys_t ) );
    if( unlikely(sys == NULL) )
        return VLC_ENOMEM;
    demux->p_sys = sys;

    ParseMRL( obj, demux->psz_location );

    char *path = var_InheritString (obj, CFG_PREFIX"dev");
    if (unlikely(path == NULL))
        goto error; /* probably OOM */
    msg_Dbg (obj, "opening device '%s'", path);

    int rawfd = vlc_open (path, O_RDWR);
    if (rawfd == -1)
    {
        msg_Err (obj, "cannot open device '%s': %m", path);
        free (path);
        goto error;
    }
    free (path);

    int fd = v4l2_fd_open (rawfd, 0);
    if (fd == -1)
    {
        msg_Warn (obj, "cannot initialize user-space library: %m");
        /* fallback to direct kernel mode anyway */
        fd = rawfd;
    }

    if (InitVideo (demux, fd))
    {
        v4l2_close (fd);
        goto error;
    }

    sys->i_fd = fd;
    sys->controls = ControlsInit (VLC_OBJECT(demux), fd);
    demux->pf_demux = Demux;
    demux->pf_control = DemuxControl;
    demux->info.i_update = 0;
    demux->info.i_title = 0;
    demux->info.i_seekpoint = 0;
    return VLC_SUCCESS;
error:
    free (sys);
    return VLC_EGENERIC;
}

typedef struct
{
    uint32_t v4l2;
    vlc_fourcc_t vlc;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
} vlc_v4l2_fmt_t;

/* NOTE: Currently vlc_v4l2_fmt_rank() assumes format are sorted in order of
 * decreasing preference. */
static const vlc_v4l2_fmt_t v4l2_fmts[] =
{
    /* Planar YUV 4:2:0 */
    { V4L2_PIX_FMT_YUV420,   VLC_CODEC_I420, 0, 0, 0 },
    { V4L2_PIX_FMT_YVU420,   VLC_CODEC_YV12, 0, 0, 0 },
    { V4L2_PIX_FMT_YUV422P,  VLC_CODEC_I422, 0, 0, 0 },
    /* Packed YUV 4:2:2 */
    { V4L2_PIX_FMT_YUYV,     VLC_CODEC_YUYV, 0, 0, 0 },
    { V4L2_PIX_FMT_UYVY,     VLC_CODEC_UYVY, 0, 0, 0 },
    { V4L2_PIX_FMT_YVYU,     VLC_CODEC_YVYU, 0, 0, 0 },
    { V4L2_PIX_FMT_VYUY,     VLC_CODEC_VYUY, 0, 0, 0 },

    { V4L2_PIX_FMT_YUV411P,  VLC_CODEC_I411, 0, 0, 0 },

    { V4L2_PIX_FMT_YUV410,   VLC_CODEC_I410, 0, 0, 0 },
//  { V4L2_PIX_FMT_YVU410     },

//  { V4L2_PIX_FMT_NV24,      },
//  { V4L2_PIX_FMT_NV42,      },
//  { V4L2_PIX_FMT_NV16,     VLC_CODEC_NV16, 0, 0, 0 },
//  { V4L2_PIX_FMT_NV61,     VLC_CODEC_NV61, 0, 0, 0 },
    { V4L2_PIX_FMT_NV12,     VLC_CODEC_NV12, 0, 0, 0 },
    { V4L2_PIX_FMT_NV21,     VLC_CODEC_NV21, 0, 0, 0 },

    /* V4L2-documented but VLC-unsupported misc. YUV formats */
//  { V4L2_PIX_FMT_Y41P       },
//  { V4L2_PIX_FMT_NV12MT,    },
//  { V4L2_PIX_FMT_M420,      },

    /* Packed RGB */
#ifdef WORDS_BIGENDIAN
    { V4L2_PIX_FMT_RGB32,    VLC_CODEC_RGB32,   0xFF00, 0xFF0000, 0xFF000000 },
    { V4L2_PIX_FMT_BGR32,    VLC_CODEC_RGB32, 0xFF000000, 0xFF0000,   0xFF00 },
    { V4L2_PIX_FMT_RGB24,    VLC_CODEC_RGB24,   0xFF0000, 0x00FF00, 0x0000FF },
    { V4L2_PIX_FMT_BGR24,    VLC_CODEC_RGB24,   0x0000FF, 0x00FF00, 0xFF0000 },
//  { V4L2_PIX_FMT_BGR666,    },
//  { V4L2_PIX_FMT_RGB565,    },
    { V4L2_PIX_FMT_RGB565X,  VLC_CODEC_RGB16,     0x001F,   0x07E0,   0xF800 },
//  { V4L2_PIX_FMT_RGB555,    },
    { V4L2_PIX_FMT_RGB555X,  VLC_CODEC_RGB15,     0x001F,   0x03E0,   0x7C00 },
//  { V4L2_PIX_FMT_RGB444,   VLC_CODEC_RGB12,     0x000F,   0xF000,   0x0F00 },
#else
    { V4L2_PIX_FMT_RGB32,    VLC_CODEC_RGB32,   0x0000FF, 0x00FF00, 0xFF0000 },
    { V4L2_PIX_FMT_BGR32,    VLC_CODEC_RGB32,   0xFF0000, 0x00FF00, 0x0000FF },
    { V4L2_PIX_FMT_RGB24,    VLC_CODEC_RGB24,   0x0000FF, 0x00FF00, 0xFF0000 },
    { V4L2_PIX_FMT_BGR24,    VLC_CODEC_RGB24,   0xFF0000, 0x00FF00, 0x0000FF },
//  { V4L2_PIX_FMT_BGR666,    },
    { V4L2_PIX_FMT_RGB565,   VLC_CODEC_RGB16,     0x001F,   0x07E0,   0xF800 },
//  { V4L2_PIX_FMT_RGB565X,   },
    { V4L2_PIX_FMT_RGB555,   VLC_CODEC_RGB15,     0x001F,   0x03E0,   0x7C00 },
//  { V4L2_PIX_FMT_RGB555X,   },
//  { V4L2_PIX_FMT_RGB444,   VLC_CODEC_RGB12,     0x0F00,   0x00F0,   0x000F },
#endif
//  { V4L2_PIX_FMT_RGB332,   VLC_CODEC_RGB8,        0xC0,     0x38,     0x07 },

    /* Bayer (sub-sampled RGB). Not supported. */
//  { V4L2_PIX_FMT_SBGGR16,  }
//  { V4L2_PIX_FMT_SRGGB12,  }
//  { V4L2_PIX_FMT_SGRBG12,  }
//  { V4L2_PIX_FMT_SGBRG12,  }
//  { V4L2_PIX_FMT_SBGGR12,  }
//  { V4L2_PIX_FMT_SRGGB10,  }
//  { V4L2_PIX_FMT_SGRBG10,  }
//  { V4L2_PIX_FMT_SGBRG10,  }
//  { V4L2_PIX_FMT_SBGGR10,  }
//  { V4L2_PIX_FMT_SBGGR8,   }
//  { V4L2_PIX_FMT_SGBRG8,   }
//  { V4L2_PIX_FMT_SGRBG8,   }
//  { V4L2_PIX_FMT_SRGGB8,   }

    /* Compressed data types */
    { V4L2_PIX_FMT_JPEG,   VLC_CODEC_MJPG, 0, 0, 0 },
    { V4L2_PIX_FMT_H264,   VLC_CODEC_H264, 0, 0, 0 },
    /* FIXME: fill p_extra for avc1... */
//  { V4L2_PIX_FMT_H264_NO_SC, VLC_FOURCC('a','v','c','1'), 0, 0, 0 }
    { V4L2_PIX_FMT_MPEG4,  VLC_CODEC_MP4V, 0, 0, 0 },
    { V4L2_PIX_FMT_XVID,   VLC_CODEC_MP4V, 0, 0, 0 },
    { V4L2_PIX_FMT_H263,   VLC_CODEC_H263, 0, 0, 0 },
    { V4L2_PIX_FMT_MPEG2,  VLC_CODEC_MPGV, 0, 0, 0 },
    { V4L2_PIX_FMT_MPEG1,  VLC_CODEC_MPGV, 0, 0, 0 },
    { V4L2_PIX_FMT_VC1_ANNEX_G, VLC_CODEC_VC1, 0, 0, 0 },
    { V4L2_PIX_FMT_VC1_ANNEX_L, VLC_CODEC_VC1, 0, 0, 0 },
    //V4L2_PIX_FMT_MPEG -> use access

    /* Reserved formats */
    { V4L2_PIX_FMT_MJPEG,   VLC_CODEC_MJPG, 0, 0, 0 },
    //V4L2_PIX_FMT_DV -> use access

    /* Grey scale */
//  { V4L2_PIX_FMT_Y16,       },
//  { V4L2_PIX_FMT_Y12,       },
//  { V4L2_PIX_FMT_Y10,       },
//  { V4L2_PIX_FMT_Y10BPACK,  },
    { V4L2_PIX_FMT_GREY,     VLC_CODEC_GREY, 0, 0, 0 },
};

static const vlc_v4l2_fmt_t *vlc_from_v4l2_fourcc (uint32_t fourcc)
{
     for (size_t i = 0; i < sizeof (v4l2_fmts) / sizeof (v4l2_fmts[0]); i++)
         if (v4l2_fmts[i].v4l2 == fourcc)
             return v4l2_fmts + i;
     return NULL;
}

static size_t vlc_v4l2_fmt_rank (const vlc_v4l2_fmt_t *fmt)
{
    if (fmt == NULL)
        return SIZE_MAX;

    ptrdiff_t d = fmt - v4l2_fmts;
    assert (d >= 0);
    assert (d < (ptrdiff_t)(sizeof (v4l2_fmts) / sizeof (v4l2_fmts[0])));
    return d;
}

static vlc_fourcc_t var_InheritFourCC (vlc_object_t *obj, const char *varname)
{
    char *str = var_InheritString (obj, varname);
    if (str == NULL)
        return 0;

    vlc_fourcc_t fourcc = vlc_fourcc_GetCodecFromString (VIDEO_ES, str);
    if (fourcc == 0)
        msg_Err (obj, "invalid codec %s", str);
    free (str);
    return fourcc;
}
#define var_InheritFourCC(o, v) var_InheritFourCC(VLC_OBJECT(o), v)

/**
 * List of V4L2 chromas were confident enough to use as fallbacks if the
 * user hasn't provided a --v4l2-chroma value.
 *
 * Try YUV chromas first, then RGB little endian and MJPEG as last resort.
 */
static const uint32_t p_chroma_fallbacks[] =
{ V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YVU420, V4L2_PIX_FMT_YUV422P,
  V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_BGR24,
  V4L2_PIX_FMT_BGR32, V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_JPEG };

static int InitVideo (demux_t *demux, int fd)
{
    demux_sys_t *sys = demux->p_sys;
    enum v4l2_buf_type buf_type;

    /* Get device capabilites */
    struct v4l2_capability cap;
    if (v4l2_ioctl (fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        msg_Err (demux, "cannot get device capabilities: %m");
        return -1;
    }

    msg_Dbg (demux, "device %s using driver %s (version %u.%u.%u) on %s",
            cap.card, cap.driver, (cap.version >> 16) & 0xFF,
            (cap.version >> 8) & 0xFF, cap.version & 0xFF, cap.bus_info);
    msg_Dbg (demux, "the device has the capabilities: 0x%08X",
             cap.capabilities );
    msg_Dbg (demux, " (%c) Video Capture, (%c) Audio, (%c) Tuner, (%c) Radio",
             (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ? 'X' : ' '),
             (cap.capabilities & V4L2_CAP_AUDIO ? 'X' : ' '),
             (cap.capabilities & V4L2_CAP_TUNER ? 'X' : ' '),
             (cap.capabilities & V4L2_CAP_RADIO ? 'X' : ' '));
    msg_Dbg (demux, " (%c) Read/Write, (%c) Streaming, (%c) Asynchronous",
             (cap.capabilities & V4L2_CAP_READWRITE ? 'X' : ' '),
             (cap.capabilities & V4L2_CAP_STREAMING ? 'X' : ' '),
             (cap.capabilities & V4L2_CAP_ASYNCIO ? 'X' : ' '));

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        msg_Err (demux, "not a video capture device");
        return -1;
    }

    if (cap.capabilities & V4L2_CAP_STREAMING)
        sys->io = IO_METHOD_MMAP;
    else if (cap.capabilities & V4L2_CAP_READWRITE)
        sys->io = IO_METHOD_READ;
    else
    {
        msg_Err (demux, "no supported I/O method");
        return -1;
    }

    if (SetupInput (VLC_OBJECT(demux), fd))
        return -1;

    /* Picture format negotiation */
    const vlc_v4l2_fmt_t *selected = NULL;
    vlc_fourcc_t reqfourcc = var_InheritFourCC (demux, CFG_PREFIX"chroma");
    bool native = false;

    for (struct v4l2_fmtdesc codec = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
         v4l2_ioctl (fd, VIDIOC_ENUM_FMT, &codec) >= 0;
         codec.index++)
    {   /* Enumerate available chromas */
        const vlc_v4l2_fmt_t *dsc = vlc_from_v4l2_fourcc (codec.pixelformat);

        msg_Dbg (demux, " %s %s format %4.4s (%4.4s): %s",
              (codec.flags & V4L2_FMT_FLAG_EMULATED) ? "emulates" : "supports",
              (codec.flags & V4L2_FMT_FLAG_COMPRESSED) ? "compressed" : "raw",
                 (char *)&codec.pixelformat,
                 (dsc != NULL) ? (const char *)&dsc->vlc : "N.A.",
                 codec.description);

        if (dsc == NULL)
            continue; /* ignore VLC-unsupported codec */

        if (dsc->vlc == reqfourcc)
        {
            msg_Dbg (demux, "  matches the requested format");
            selected = dsc;
            break; /* always select the requested format if found */
        }

        if (codec.flags & V4L2_FMT_FLAG_EMULATED)
        {
            if (native)
                continue; /* ignore emulated format if possible */
        }
        else
            native = true;

        if (vlc_v4l2_fmt_rank (dsc) > vlc_v4l2_fmt_rank (selected))
            continue; /* ignore if rank is worse */

        selected = dsc;
    }

    if (selected == NULL)
    {
        msg_Err (demux, "cannot negotiate supported video format");
        return -1;
    }
    msg_Dbg (demux, "selected format %4.4s (%4.4s)",
             (const char *)&selected->v4l2, (const char *)&selected->vlc);

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (v4l2_ioctl (fd, VIDIOC_G_FMT, &fmt) < 0)
    {
        msg_Err (demux, "cannot get device default format: %m");
        return -1;
    }

    fmt.fmt.pix.pixelformat = selected->v4l2;

    uint32_t width = var_InheritInteger (demux, CFG_PREFIX"width");
    if (width != (uint32_t)-1)
        fmt.fmt.pix.width = width; /* override width */
    uint32_t height = var_InheritInteger (demux, CFG_PREFIX"height");
    if (height != (uint32_t)-1)
        fmt.fmt.pix.height = height; /* override height */

    if (v4l2_ioctl (fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        msg_Err (demux, "cannot set format: %m");
        return -1;
    }

    /* Try and find default resolution if not specified */
    float fps = 0.;
    if (width == (uint32_t)-1 || height == (uint32_t)-1)
    {
        fps = var_InheritFloat (demux, CFG_PREFIX"fps");
        if (fps <= 0.)
        {
            fps = GetAbsoluteMaxFrameRate (VLC_OBJECT(demux), fd,
                                           fmt.fmt.pix.pixelformat);
            msg_Dbg (demux, "Found maximum framerate of %f", fps);
        }
        uint32_t i_width, i_height;
        GetMaxDimensions (VLC_OBJECT(demux), fd, fmt.fmt.pix.pixelformat, fps,
                          &i_width, &i_height);
        if (i_width || i_height)
        {
            msg_Dbg (demux, "Found optimal dimensions for framerate %f "
                            "of %ux%u", fps, i_width, i_height);
            fmt.fmt.pix.width = i_width;
            fmt.fmt.pix.height = i_height;
            if (v4l2_ioctl (fd, VIDIOC_S_FMT, &fmt) < 0)
            {
                msg_Err (demux, "Cannot set size to optimal dimensions %ux%u",
                         i_width, i_height);
                return -1;
            }
        }
        else
        {
            msg_Warn (demux, "Could not find optimal width and height, "
                      "falling back to driver default.");
        }
    }

    width = fmt.fmt.pix.width;
    height = fmt.fmt.pix.height;

    if (v4l2_ioctl (fd, VIDIOC_G_FMT, &fmt) < 0) {;}

    /* Print extra info */
    msg_Dbg (demux, "%d bytes maximum for complete image",
             fmt.fmt.pix.sizeimage);
    /* Check interlacing */
    switch (fmt.fmt.pix.field)
    {
        case V4L2_FIELD_NONE:
            msg_Dbg (demux, "Interlacing setting: progressive");
            break;
        case V4L2_FIELD_TOP:
            msg_Dbg (demux, "Interlacing setting: top field only");
            break;
        case V4L2_FIELD_BOTTOM:
            msg_Dbg (demux, "Interlacing setting: bottom field only");
            break;
        case V4L2_FIELD_INTERLACED:
            msg_Dbg (demux, "Interlacing setting: interleaved");
            /*if (NTSC)
                sys->i_block_flags = BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            else*/
                sys->i_block_flags = BLOCK_FLAG_TOP_FIELD_FIRST;
            break;
        case V4L2_FIELD_SEQ_TB:
            msg_Dbg (demux, "Interlacing setting: sequential top bottom (TODO)");
            break;
        case V4L2_FIELD_SEQ_BT:
            msg_Dbg (demux, "Interlacing setting: sequential bottom top (TODO)");
            break;
        case V4L2_FIELD_ALTERNATE:
            msg_Dbg (demux, "Interlacing setting: alternate fields (TODO)");
            height *= 2;
            break;
        case V4L2_FIELD_INTERLACED_TB:
            msg_Dbg (demux, "Interlacing setting: interleaved top bottom");
            sys->i_block_flags = BLOCK_FLAG_TOP_FIELD_FIRST;
            break;
        case V4L2_FIELD_INTERLACED_BT:
            msg_Dbg (demux, "Interlacing setting: interleaved bottom top");
            sys->i_block_flags = BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            break;
        default:
            msg_Warn (demux, "Interlacing setting: unknown type (%d)",
                      fmt.fmt.pix.field);
            break;
    }

    /* Look up final fourcc */
    es_format_t es_fmt;
    es_format_Init (&es_fmt, VIDEO_ES, selected->vlc);
    es_fmt.video.i_rmask = selected->red;
    es_fmt.video.i_gmask = selected->green;
    es_fmt.video.i_bmask = selected->blue;

    /* Init I/O method */
    switch (sys->io)
    {
    case IO_METHOD_READ:
        sys->blocksize = fmt.fmt.pix.sizeimage;
        break;

    case IO_METHOD_MMAP:
        if (InitMmap (VLC_OBJECT(demux), sys, fd))
            return -1;
        for (unsigned int i = 0; i < sys->i_nbuffers; i++)
        {
            struct v4l2_buffer buf = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP,
                .index = i,
            };

            if (v4l2_ioctl (fd, VIDIOC_QBUF, &buf) < 0)
            {
                msg_Err (demux, "cannot queue buffer: %m");
                return -1;
            }
        }

        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (v4l2_ioctl (fd, VIDIOC_STREAMON, &buf_type) < 0)
        {
            msg_Err (demux, "cannot start streaming: %m");
            return -1;
        }
        break;

    default:
        assert (0);
    }

    int ar = 4 * VOUT_ASPECT_FACTOR / 3;
    char *str = var_InheritString (demux, CFG_PREFIX"aspect-ratio");
    if (likely(str != NULL))
    {
        const char *delim = strchr (str, ':');
        if (delim != NULL)
            ar = atoi (str) * VOUT_ASPECT_FACTOR / atoi (delim + 1);
        free (str);
    }

    /* Add */
    es_fmt.video.i_width  = width;
    es_fmt.video.i_height = height;

    /* Get aspect-ratio */
    es_fmt.video.i_sar_num = ar * es_fmt.video.i_height;
    es_fmt.video.i_sar_den = VOUT_ASPECT_FACTOR * es_fmt.video.i_width;

    /* Framerate */
    es_fmt.video.i_frame_rate = lround (fps * 1000000.);
    es_fmt.video.i_frame_rate_base = 1000000;

    msg_Dbg (demux, "added new video es %4.4s %dx%d", (char *)&es_fmt.i_codec,
             es_fmt.video.i_width, es_fmt.video.i_height);
    msg_Dbg (demux, " frame rate: %f", fps);

    sys->p_es = es_out_Add (demux->out, &es_fmt);
    return 0;
}

void DemuxClose( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;
    demux_sys_t *sys = demux->p_sys;
    int fd = sys->i_fd;

    /* Stop video capture */
    switch( sys->io )
    {
        case IO_METHOD_READ:
            /* Nothing to do */
            break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
        {
            /* NOTE: Some buggy drivers hang if buffers are not unmapped before
             * streamoff */
            for( unsigned i = 0; i < sys->i_nbuffers; i++ )
            {
                struct v4l2_buffer buf = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .memory = ( sys->io == IO_METHOD_USERPTR ) ?
                    V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP,
                };
                v4l2_ioctl( fd, VIDIOC_DQBUF, &buf );
            }
            enum v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            v4l2_ioctl( sys->i_fd, VIDIOC_STREAMOFF, &buf_type );
            break;
        }
    }

    /* Free Video Buffers */
    if( sys->p_buffers ) {
        switch( sys->io )
        {
        case IO_METHOD_READ:
            free( sys->p_buffers[0].start );
            break;

        case IO_METHOD_MMAP:
            for( unsigned i = 0; i < sys->i_nbuffers; ++i )
                v4l2_munmap( sys->p_buffers[i].start,
                             sys->p_buffers[i].length );
            break;

        case IO_METHOD_USERPTR:
            for( unsigned i = 0; i < sys->i_nbuffers; ++i )
               free( sys->p_buffers[i].start );
            break;
        }
        free( sys->p_buffers );
    }

    ControlsDeinit( obj, sys->controls );
    v4l2_close( fd );
    free( sys );
}

static int DemuxControl( demux_t *demux, int query, va_list args )
{
    switch( query )
    {
        /* Special for access_demux */
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_PACE:
            *va_arg( args, bool * ) = false;
            return VLC_SUCCESS;

        case DEMUX_GET_PTS_DELAY:
            *va_arg(args,int64_t *) = INT64_C(1000)
                * var_InheritInteger( demux, "live-caching" );
            return VLC_SUCCESS;

        case DEMUX_GET_TIME:
            *va_arg( args, int64_t * ) = mdate();
            return VLC_SUCCESS;

        /* TODO implement others */
        default:
            return VLC_EGENERIC;
    }

    return VLC_EGENERIC;
}

/** Gets a frame in read/write mode */
static block_t *BlockRead( vlc_object_t *obj, int fd, size_t size )
{
    block_t *block = block_Alloc( size );
    if( unlikely(block == NULL) )
        return NULL;

    ssize_t val = v4l2_read( fd, block->p_buffer, size );
    if( val == -1 )
    {
        block_Release( block );
        switch( errno )
        {
            case EAGAIN:
                return NULL;
            case EIO: /* could be ignored per specification */
                /* fall through */
            default:
                msg_Err( obj, "cannot read frame: %m" );
                return NULL;
        }
    }
    block->i_buffer = val;
    return block;
}

static int Demux( demux_t *demux )
{
    demux_sys_t *sys = demux->p_sys;
    struct pollfd ufd;

    ufd.fd = sys->i_fd;
    ufd.events = POLLIN|POLLPRI;
    /* Wait for data */
    /* FIXME: remove timeout */
    while( poll( &ufd, 1, 500 ) == -1 )
        if( errno != EINTR )
        {
            msg_Err( demux, "poll error: %m" );
            return -1;
        }

    if( ufd.revents == 0 )
        return 1;

    block_t *block;

    if( sys->io == IO_METHOD_READ )
        block = BlockRead( VLC_OBJECT(demux), ufd.fd, sys->blocksize );
    else
        block = GrabVideo( VLC_OBJECT(demux), sys );
    if( block == NULL )
        return 1;

    block->i_pts = block->i_dts = mdate();
    block->i_flags |= sys->i_block_flags;
    es_out_Control( demux->out, ES_OUT_SET_PCR, block->i_pts );
    es_out_Send( demux->out, sys->p_es, block );
    return 1;
}

static float GetMaxFPS( vlc_object_t *obj, int fd, uint32_t pixel_format,
                        uint32_t width, uint32_t height )
{
#ifdef VIDIOC_ENUM_FRAMEINTERVALS
    /* This is new in Linux 2.6.19 */
    struct v4l2_frmivalenum fie = {
        .pixel_format = pixel_format,
        .width = width,
        .height = height,
    };

    if( v4l2_ioctl( fd, VIDIOC_ENUM_FRAMEINTERVALS, &fie ) < 0 )
        return -1.;

    switch( fie.type )
    {
        case V4L2_FRMIVAL_TYPE_DISCRETE:
        {
            float max = -1.;
            do
            {
                float fps = (float)fie.discrete.denominator
                          / (float)fie.discrete.numerator;
                if( fps > max )
                    max = fps;
                msg_Dbg( obj, "  discrete frame interval %"PRIu32"/%"PRIu32
                         " supported",
                         fie.discrete.numerator, fie.discrete.denominator );
                fie.index++;
            } while( v4l2_ioctl( fd, VIDIOC_ENUM_FRAMEINTERVALS, &fie ) >= 0 );
            return max;
        }

        case V4L2_FRMIVAL_TYPE_STEPWISE:
        case V4L2_FRMIVAL_TYPE_CONTINUOUS:
            msg_Dbg( obj, "  frame intervals from %"PRIu32"/%"PRIu32
                    "to %"PRIu32"/%"PRIu32" supported",
                    fie.stepwise.min.numerator, fie.stepwise.min.denominator,
                    fie.stepwise.max.numerator, fie.stepwise.max.denominator );
            if( fie.type == V4L2_FRMIVAL_TYPE_STEPWISE )
                msg_Dbg( obj, "  with %"PRIu32"/%"PRIu32" step",
                         fie.stepwise.step.numerator,
                         fie.stepwise.step.denominator );
            return __MAX( (float)fie.stepwise.max.denominator
                        / (float)fie.stepwise.max.numerator,
                          (float)fie.stepwise.min.denominator
                        / (float)fie.stepwise.min.numerator );
    }
#endif
    return -1.;
}

float GetAbsoluteMaxFrameRate( vlc_object_t *obj, int fd,
                               uint32_t pixel_format )
{
#ifdef VIDIOC_ENUM_FRAMESIZES
    /* This is new in Linux 2.6.19 */
    struct v4l2_frmsizeenum fse = {
        .pixel_format = pixel_format
    };

    if( v4l2_ioctl( fd, VIDIOC_ENUM_FRAMESIZES, &fse ) < 0 )
        return -1.;

    float max = -1.;
    switch( fse.type )
    {
      case V4L2_FRMSIZE_TYPE_DISCRETE:
        do
        {
            float fps = GetMaxFPS( obj, fd, pixel_format,
                                   fse.discrete.width, fse.discrete.height );
            if( fps > max )
                max = fps;
            fse.index++;
        } while( v4l2_ioctl( fd, VIDIOC_ENUM_FRAMESIZES, &fse ) >= 0 );
        break;

      case V4L2_FRMSIZE_TYPE_STEPWISE:
      case V4L2_FRMSIZE_TYPE_CONTINUOUS:
        msg_Dbg( obj, " sizes from %"PRIu32"x%"PRIu32" "
                 "to %"PRIu32"x%"PRIu32" supported",
                 fse.stepwise.min_width, fse.stepwise.min_height,
                 fse.stepwise.max_width, fse.stepwise.max_height );
        if( fse.type == V4L2_FRMSIZE_TYPE_STEPWISE )
            msg_Dbg( obj, "  with %"PRIu32"x%"PRIu32" steps",
                     fse.stepwise.step_width, fse.stepwise.step_height );

        for( uint32_t width =  fse.stepwise.min_width;
                      width <= fse.stepwise.max_width;
                      width += fse.stepwise.step_width )
            for( uint32_t height =  fse.stepwise.min_height;
                          height <= fse.stepwise.max_width;
                          height += fse.stepwise.step_height )
            {
                float fps = GetMaxFPS( obj, fd, pixel_format, width, height );
                if( fps > max )
                    max = fps;
            }
        break;
    }
    return max;
#else
    return -1.;
#endif
}

void GetMaxDimensions( vlc_object_t *obj, int fd, uint32_t pixel_format,
                       float fps_min, uint32_t *pwidth, uint32_t *pheight )
{
    *pwidth = 0;
    *pheight = 0;

#ifdef VIDIOC_ENUM_FRAMESIZES
    /* This is new in Linux 2.6.19 */
    struct v4l2_frmsizeenum fse = {
        .pixel_format = pixel_format
    };

    if( v4l2_ioctl( fd, VIDIOC_ENUM_FRAMESIZES, &fse ) < 0 )
        return;

    switch( fse.type )
    {
      case V4L2_FRMSIZE_TYPE_DISCRETE:
        do
        {
            msg_Dbg( obj, " discrete size %"PRIu32"x%"PRIu32" supported",
                     fse.discrete.width, fse.discrete.height );

            float fps = GetMaxFPS( obj, fd, pixel_format,
                                   fse.discrete.width, fse.discrete.height );
            if( fps >= fps_min && fse.discrete.width > *pwidth )
            {
                *pwidth = fse.discrete.width;
                *pheight = fse.discrete.height;
            }
            fse.index++;
        }
        while( v4l2_ioctl( fd, VIDIOC_ENUM_FRAMESIZES, &fse ) >= 0 );
        break;

      case V4L2_FRMSIZE_TYPE_STEPWISE:
      case V4L2_FRMSIZE_TYPE_CONTINUOUS:
        msg_Dbg( obj, " sizes from %"PRIu32"x%"PRIu32" "
                 "to %"PRIu32"x%"PRIu32" supported",
                 fse.stepwise.min_width, fse.stepwise.min_height,
                 fse.stepwise.max_width, fse.stepwise.max_height );
        if( fse.type == V4L2_FRMSIZE_TYPE_STEPWISE )
            msg_Dbg( obj, "  with %"PRIu32"x%"PRIu32" steps",
                     fse.stepwise.step_width, fse.stepwise.step_height );

        for( uint32_t width =  fse.stepwise.min_width;
                      width <= fse.stepwise.max_width;
                      width += fse.stepwise.step_width )
            for( uint32_t height = fse.stepwise.min_height;
                          height <= fse.stepwise.max_width;
                          height += fse.stepwise.step_height )
            {
                float fps = GetMaxFPS( obj, fd, pixel_format, width, height );
                if( fps >= fps_min && width > *pwidth )
                {
                    *pwidth = width;
                    *pheight = height;
                }
            }
        break;
    }
#endif
}
