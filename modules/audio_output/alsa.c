/*****************************************************************************
 * alsa.c : alsa plugin for vlc
 *****************************************************************************
 * Copyright (C) 2000-2010 VLC authors and VideoLAN
 * Copyright (C) 2009-2011 Rémi Denis-Courmont
 *
 * Authors: Henri Fallon <henri@videolan.org> - Original Author
 *          Jeffrey Baker <jwbaker@acm.org> - Port to ALSA 1.0 API
 *          John Paul Lorenti <jpl31@columbia.edu> - Device selection
 *          Arnaud de Bossoreille de Ribou <bozo@via.ecp.fr> - S/PDIF and aout3
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_dialog.h>
#include <vlc_aout.h>
#include <vlc_cpu.h>

#include <alsa/asoundlib.h>
#include <alsa/version.h>

/** Private data for an ALSA PCM playback stream */
struct aout_sys_t
{
    snd_pcm_t *pcm;
    unsigned rate; /**< Sample rate */
    uint8_t chans_table[AOUT_CHAN_MAX]; /**< Channels order table */
    uint8_t chans_to_reorder; /**< Number of channels to reorder */
    uint8_t bits; /**< Bits per sample per channel */
    bool soft_mute;
    float soft_gain;
};

#include "volume.h"

#define A52_FRAME_NB 1536

static int Open (vlc_object_t *);
static void Close (vlc_object_t *);
static int EnumDevices (vlc_object_t *, char const *, char ***, char ***);
static void GetDevices (vlc_object_t *, const char *);

#define AUDIO_DEV_TEXT N_("Audio output device")
#define AUDIO_DEV_LONGTEXT N_("Audio output device (using ALSA syntax).")

#define AUDIO_CHAN_TEXT N_("Audio output channels")
#define AUDIO_CHAN_LONGTEXT N_("Channels available for audio output. " \
    "If the input has more channels than the output, it will be down-mixed. " \
    "This parameter is ignored when digital pass-through is active.")
static const int channels[] = {
    AOUT_CHAN_CENTER, AOUT_CHANS_STEREO, AOUT_CHANS_4_0, AOUT_CHANS_4_1,
    AOUT_CHANS_5_0, AOUT_CHANS_5_1, AOUT_CHANS_7_1,
};
static const char *const channels_text[] = {
    N_("Mono"), N_("Stereo"), N_("Surround 4.0"), N_("Surround 4.1"),
    N_("Surround 5.0"), N_("Surround 5.1"), N_("Surround 7.1"),
};

vlc_module_begin ()
    set_shortname( "ALSA" )
    set_description( N_("ALSA audio output") )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AOUT )
    add_string ("alsa-audio-device", "default",
                AUDIO_DEV_TEXT, AUDIO_DEV_LONGTEXT, false)
        change_string_cb (EnumDevices)
    add_integer ("alsa-audio-channels", AOUT_CHANS_FRONT,
                 AUDIO_CHAN_TEXT, AUDIO_CHAN_LONGTEXT, false)
        change_integer_list (channels, channels_text)
    add_sw_gain ()
    set_capability( "audio output", 150 )
    set_callbacks( Open, Close )
vlc_module_end ()


/** Helper for ALSA -> VLC debugging output */
static void Dump (vlc_object_t *obj, const char *msg,
                  int (*cb)(void *, snd_output_t *), void *p)
{
    snd_output_t *output;
    char *str;

    if (unlikely(snd_output_buffer_open (&output)))
        return;

    int val = cb (p, output);
    if (val)
    {
        msg_Warn (obj, "cannot get info: %s", snd_strerror (val));
        return;
    }

    size_t len = snd_output_buffer_string (output, &str);
    if (len > 0 && str[len - 1])
        len--; /* strip trailing newline */
    msg_Dbg (obj, "%s%.*s", msg, (int)len, str);
    snd_output_close (output);
}
#define Dump(o, m, cb, p) \
        Dump(VLC_OBJECT(o), m, (int (*)(void *, snd_output_t *))(cb), p)

static void DumpDevice (vlc_object_t *obj, snd_pcm_t *pcm)
{
    snd_pcm_info_t *info;

    Dump (obj, " ", snd_pcm_dump, pcm);
    snd_pcm_info_alloca (&info);
    if (snd_pcm_info (pcm, info) == 0)
    {
        msg_Dbg (obj, " device name   : %s", snd_pcm_info_get_name (info));
        msg_Dbg (obj, " device ID     : %s", snd_pcm_info_get_id (info));
        msg_Dbg (obj, " subdevice name: %s",
                snd_pcm_info_get_subdevice_name (info));
    }
}

static void DumpDeviceStatus (vlc_object_t *obj, snd_pcm_t *pcm)
{
    snd_pcm_status_t *status;

    snd_pcm_status_alloca (&status);
    snd_pcm_status (pcm, status);
    Dump (obj, "current status:\n", snd_pcm_status_dump, status);
}
#define DumpDeviceStatus(o, p) DumpDeviceStatus(VLC_OBJECT(o), p)

static int DeviceChanged (vlc_object_t *obj, const char *varname,
                          vlc_value_t prev, vlc_value_t cur, void *data)
{
    aout_ChannelsRestart (obj, varname, prev, cur, data);

    if (!var_Type (obj, "alsa-audio-device"))
        var_Create (obj, "alsa-audio-device", VLC_VAR_STRING);
    var_SetString (obj, "alsa-audio-device", cur.psz_string);
    return VLC_SUCCESS;
}

static unsigned SetupChannelsUnknown (vlc_object_t *obj,
                                      uint16_t *restrict mask)
{
    uint16_t map = var_InheritInteger (obj, "alsa-audio-channels");
    uint16_t chans = *mask & map;

    if (unlikely(chans == 0)) /* WTH? */
        chans = AOUT_CHANS_STEREO;

    if (popcount (chans) < popcount (*mask))
        msg_Dbg (obj, "downmixing from %u to %u channels",
                 popcount (*mask), popcount (chans));
    else
        msg_Dbg (obj, "keeping %u channels", popcount (chans));
    *mask = chans;
    return 0;
}

#if (SND_LIB_VERSION >= 0x01001B)
static const uint16_t vlc_chans[] = {
    [SND_CHMAP_MONO] = AOUT_CHAN_CENTER,
    [SND_CHMAP_FL]   = AOUT_CHAN_LEFT,
    [SND_CHMAP_FR]   = AOUT_CHAN_RIGHT,
    [SND_CHMAP_RL]   = AOUT_CHAN_REARLEFT,
    [SND_CHMAP_RR]   = AOUT_CHAN_REARRIGHT,
    [SND_CHMAP_FC]   = AOUT_CHAN_CENTER,
    [SND_CHMAP_LFE]  = AOUT_CHAN_LFE,
    [SND_CHMAP_SL]   = AOUT_CHAN_MIDDLELEFT,
    [SND_CHMAP_SR]   = AOUT_CHAN_MIDDLERIGHT,
    [SND_CHMAP_RC]   = AOUT_CHAN_REARCENTER,
};

static int Map2Mask (vlc_object_t *obj, const snd_pcm_chmap_t *restrict map)
{
    uint16_t mask = 0;

    for (unsigned i = 0; i < map->channels; i++)
    {
        const unsigned pos = map->pos[i];
        uint_fast16_t vlc_chan = 0;

        if (pos < sizeof (vlc_chans) / sizeof (vlc_chans[0]))
            vlc_chan = vlc_chans[pos];
        if (vlc_chan == 0)
        {
            msg_Dbg (obj, " %s channel %u position %u", "unsupported", i, pos);
            return -1;
        }
        if (mask & vlc_chan)
        {
            msg_Dbg (obj, " %s channel %u position %u", "duplicate", i, pos);
            return -1;
        }
        mask |= vlc_chan;
    }
    return mask;
}

/**
 * Compares a fixed ALSA channels map with the VLC channels order.
 */
static unsigned SetupChannelsFixed(const snd_pcm_chmap_t *restrict map,
                                uint16_t *restrict mask, uint8_t *restrict tab)
{
    uint32_t chans_out[AOUT_CHAN_MAX];

    for (unsigned i = 0; i < map->channels; i++)
    {
        uint_fast16_t vlc_chan = vlc_chans[map->pos[i]];

        chans_out[i] = vlc_chan;
        *mask |= vlc_chan;
    }

    return aout_CheckChannelReorder(NULL, chans_out, *mask, tab);
}

/**
 * Negotiate channels mapping.
 */
static unsigned SetupChannels (vlc_object_t *obj, snd_pcm_t *pcm,
                                uint16_t *restrict mask, uint8_t *restrict tab)
{
    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps (pcm);
    if (tab == NULL)
    {   /* Fallback to manual configuration */
        msg_Dbg(obj, "channels map not provided");
        return SetupChannelsUnknown (obj, mask);
    }

    /* Find most appropriate available channels map */
    unsigned best_offset;
    unsigned best_score = 0;

    for (snd_pcm_chmap_query_t *const *p = maps; *p != NULL; p++)
    {
        snd_pcm_chmap_query_t *map = *p;

        switch (map->type)
        {
            case SND_CHMAP_TYPE_FIXED:
            case SND_CHMAP_TYPE_PAIRED:
            case SND_CHMAP_TYPE_VAR:
                break;
            default:
                msg_Err (obj, "unknown channels map type %u", map->type);
                continue;
        }

        int chans = Map2Mask (obj, &map->map);
        if (chans == -1)
            continue;

        unsigned score = popcount (chans & *mask);
        if (score > best_score)
        {
            best_offset = p - maps;
            best_score = score;
        }
    }

    if (best_score == 0)
    {
        msg_Err (obj, "cannot find supported channels map");
        snd_pcm_free_chmaps (maps);
        return SetupChannelsUnknown (obj, mask);
    }

    const snd_pcm_chmap_t *map = &maps[best_offset]->map;
    msg_Dbg (obj, "using channels map %u, type %u, %u channel(s)", best_offset,
             maps[best_offset]->type, best_score);

    /* Setup channels map */
    unsigned to_reorder = SetupChannelsFixed(map, mask, tab);

    /* TODO: avoid reordering for PAIRED and VAR types */
    //snd_pcm_set_chmap (pcm, ...)

    snd_pcm_free_chmaps (maps);
    return to_reorder;
}
#else /* (SND_LIB_VERSION < 0x01001B) */
# define SetupChannels(obj, pcm, mask, tab) \
         SetupChannelsUnknown(obj, mask)
#endif

static int TimeGet (audio_output_t *aout, mtime_t *);
static void Play (audio_output_t *, block_t *);
static void Pause (audio_output_t *, bool, mtime_t);
static void PauseDummy (audio_output_t *, bool, mtime_t);
static void Flush (audio_output_t *, bool);

/** Initializes an ALSA playback stream */
static int Start (audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    aout_sys_t *sys = aout->sys;

    /* Get device name */
    char *device = var_InheritString (aout, "alsa-audio-device");
    if (unlikely(device == NULL))
        return VLC_ENOMEM;

    snd_pcm_format_t pcm_format; /* ALSA sample format */
    vlc_fourcc_t fourcc = fmt->i_format;
    bool spdif = false;

    switch (fourcc)
    {
        case VLC_CODEC_F64B:
            pcm_format = SND_PCM_FORMAT_FLOAT64_BE;
            break;
        case VLC_CODEC_F64L:
            pcm_format = SND_PCM_FORMAT_FLOAT64_LE;
            break;
        case VLC_CODEC_F32B:
            pcm_format = SND_PCM_FORMAT_FLOAT_BE;
            break;
        case VLC_CODEC_F32L:
            pcm_format = SND_PCM_FORMAT_FLOAT_LE;
            break;
        case VLC_CODEC_S32B:
            pcm_format = SND_PCM_FORMAT_S32_BE;
            break;
        case VLC_CODEC_S32L:
            pcm_format = SND_PCM_FORMAT_S32_LE;
            break;
        case VLC_CODEC_S24B:
            pcm_format = SND_PCM_FORMAT_S24_3BE;
            break;
        case VLC_CODEC_S24L:
            pcm_format = SND_PCM_FORMAT_S24_3LE;
            break;
        case VLC_CODEC_U24B:
            pcm_format = SND_PCM_FORMAT_U24_3BE;
            break;
        case VLC_CODEC_U24L:
            pcm_format = SND_PCM_FORMAT_U24_3LE;
            break;
        case VLC_CODEC_S16B:
            pcm_format = SND_PCM_FORMAT_S16_BE;
            break;
        case VLC_CODEC_S16L:
            pcm_format = SND_PCM_FORMAT_S16_LE;
            break;
        case VLC_CODEC_U16B:
            pcm_format = SND_PCM_FORMAT_U16_BE;
            break;
        case VLC_CODEC_U16L:
            pcm_format = SND_PCM_FORMAT_U16_LE;
            break;
        case VLC_CODEC_S8:
            pcm_format = SND_PCM_FORMAT_S8;
            break;
        case VLC_CODEC_U8:
            pcm_format = SND_PCM_FORMAT_U8;
            break;
        default:
            if (AOUT_FMT_SPDIF(fmt))
                spdif = var_InheritBool (aout, "spdif");
            if (spdif)
            {
                fourcc = VLC_CODEC_SPDIFL;
                pcm_format = SND_PCM_FORMAT_S16;
            }
            else
            if (HAVE_FPU)
            {
                fourcc = VLC_CODEC_FL32;
                pcm_format = SND_PCM_FORMAT_FLOAT;
            }
            else
            {
                fourcc = VLC_CODEC_S16N;
                pcm_format = SND_PCM_FORMAT_S16;
            }
    }

    /* Choose the IEC device for S/PDIF output:
       if the device is overridden by the user then it will be the one.
       Otherwise we compute the default device based on the output format. */
    if (spdif && !strcmp (device, "default"))
    {
        unsigned aes3;

        switch (fmt->i_rate)
        {
#define FS(freq) \
            case freq: aes3 = IEC958_AES3_CON_FS_ ## freq; break;
            FS( 44100) /* def. */ FS( 48000) FS( 32000)
            FS( 22050)            FS( 24000)
            FS( 88200) FS(768000) FS( 96000)
            FS(176400)            FS(192000)
#undef FS
            default:
                aes3 = IEC958_AES3_CON_FS_NOTID;
                break;
        }

        free (device);
        if (asprintf (&device,
                      "iec958:AES0=0x%x,AES1=0x%x,AES2=0x%x,AES3=0x%x",
                      IEC958_AES0_CON_EMPHASIS_NONE | IEC958_AES0_NONAUDIO,
                      IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
                      0, aes3) == -1)
            return VLC_ENOMEM;
    }

    /* Open the device */
    snd_pcm_t *pcm;
    /* VLC always has a resampler. No need for ALSA's. */
    const int mode = SND_PCM_NO_AUTO_RESAMPLE;

    int val = snd_pcm_open (&pcm, device, SND_PCM_STREAM_PLAYBACK, mode);
    if (val != 0)
    {
        msg_Err (aout, "cannot open ALSA device \"%s\": %s", device,
                 snd_strerror (val));
        dialog_Fatal (aout, _("Audio output failed"),
                      _("The audio device \"%s\" could not be used:\n%s."),
                      device, snd_strerror (val));
        free (device);
        return VLC_EGENERIC;
    }
    sys->pcm = pcm;

    /* Print some potentially useful debug */
    msg_Dbg (aout, "using ALSA device: %s", device);
    DumpDevice (VLC_OBJECT(aout), pcm);

    /* Get Initial hardware parameters */
    snd_pcm_hw_params_t *hw;
    unsigned param;

    snd_pcm_hw_params_alloca (&hw);
    snd_pcm_hw_params_any (pcm, hw);
    Dump (aout, "initial hardware setup:\n", snd_pcm_hw_params_dump, hw);

    val = snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);
    if (val)
    {
        msg_Err (aout, "cannot disable resampling: %s", snd_strerror (val));
        goto error;
    }

    val = snd_pcm_hw_params_set_access (pcm, hw,
                                        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (val)
    {
        msg_Err (aout, "cannot set access mode: %s", snd_strerror (val));
        goto error;
    }

    /* Set sample format */
    if (snd_pcm_hw_params_test_format (pcm, hw, pcm_format) == 0)
        ;
    else
    if (snd_pcm_hw_params_test_format (pcm, hw, SND_PCM_FORMAT_FLOAT) == 0)
    {
        fourcc = VLC_CODEC_FL32;
        pcm_format = SND_PCM_FORMAT_FLOAT;
    }
    else
    if (snd_pcm_hw_params_test_format (pcm, hw, SND_PCM_FORMAT_S32) == 0)
    {
        fourcc = VLC_CODEC_S32N;
        pcm_format = SND_PCM_FORMAT_S32;
    }
    else
    if (snd_pcm_hw_params_test_format (pcm, hw, SND_PCM_FORMAT_S16) == 0)
    {
        fourcc = VLC_CODEC_S16N;
        pcm_format = SND_PCM_FORMAT_S16;
    }
    else
    {
        msg_Err (aout, "no supported sample format");
        goto error;
    }

    val = snd_pcm_hw_params_set_format (pcm, hw, pcm_format);
    if (val)
    {
        msg_Err (aout, "cannot set sample format: %s", snd_strerror (val));
        goto error;
    }

    /* Set channels count */
    unsigned channels;
    if (!spdif)
    {
        sys->chans_to_reorder = SetupChannels (VLC_OBJECT(aout), pcm,
                                  &fmt->i_physical_channels, sys->chans_table);
        channels = popcount (fmt->i_physical_channels);
    }
    else
        channels = 2;
    fmt->i_original_channels = fmt->i_physical_channels;

    /* By default, ALSA plug will pad missing channels with zeroes, which is
     * usually fine. However, it will also discard extraneous channels, which
     * is not acceptable. Thus the user must configure the physically
     * available channels, and VLC will downmix if needed. */
    val = snd_pcm_hw_params_set_channels (pcm, hw, channels);
    if (val)
    {
        msg_Err (aout, "cannot set %u channels: %s", channels,
                 snd_strerror (val));
        goto error;
    }

    /* Set sample rate */
    unsigned rate = fmt->i_rate;
    val = snd_pcm_hw_params_set_rate_near (pcm, hw, &rate, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set sample rate: %s", snd_strerror (val));
        goto error;
    }
    if (fmt->i_rate != rate)
        msg_Dbg (aout, "resampling from %d Hz to %d Hz", fmt->i_rate, rate);

    /* Set buffer size */
    param = AOUT_MAX_ADVANCE_TIME;
    val = snd_pcm_hw_params_set_buffer_time_near (pcm, hw, &param, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set buffer duration: %s", snd_strerror (val));
        goto error;
    }
#if 0
    val = snd_pcm_hw_params_get_buffer_time (hw, &param, NULL);
    if (val)
    {
        msg_Warn (aout, "cannot get buffer time: %s", snd_strerror(val));
        param = AOUT_MIN_PREPARE_TIME;
    }
    else
        param /= 2;
#else /* work-around for period-long latency outputs (e.g. PulseAudio): */
    param = AOUT_MIN_PREPARE_TIME;
#endif
    val = snd_pcm_hw_params_set_period_time_near (pcm, hw, &param, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set period: %s", snd_strerror (val));
        goto error;
    }

    /* Commit hardware parameters */
    val = snd_pcm_hw_params (pcm, hw);
    if (val < 0)
    {
        msg_Err (aout, "cannot commit hardware parameters: %s",
                 snd_strerror (val));
        goto error;
    }
    Dump (aout, "final HW setup:\n", snd_pcm_hw_params_dump, hw);

    /* Get Initial software parameters */
    snd_pcm_sw_params_t *sw;

    snd_pcm_sw_params_alloca (&sw);
    snd_pcm_sw_params_current (pcm, sw);
    Dump (aout, "initial software parameters:\n", snd_pcm_sw_params_dump, sw);

    /* START REVISIT */
    //snd_pcm_sw_params_set_avail_min( pcm, sw, i_period_size );
    // FIXME: useful?
    val = snd_pcm_sw_params_set_start_threshold (pcm, sw, 1);
    if( val < 0 )
    {
        msg_Err( aout, "unable to set start threshold (%s)",
                 snd_strerror( val ) );
        goto error;
    }
    /* END REVISIT */

    /* Commit software parameters. */
    val = snd_pcm_sw_params (pcm, sw);
    if (val)
    {
        msg_Err (aout, "cannot commit software parameters: %s",
                 snd_strerror (val));
        goto error;
    }
    Dump (aout, "final software parameters:\n", snd_pcm_sw_params_dump, sw);

    val = snd_pcm_prepare (pcm);
    if (val)
    {
        msg_Err (aout, "cannot prepare device: %s", snd_strerror (val));
        goto error;
    }

    /* Setup audio_output_t */
    fmt->i_format = fourcc;
    fmt->i_rate = rate;
    sys->rate = rate;
    if (spdif)
    {
        fmt->i_bytes_per_frame = AOUT_SPDIF_SIZE;
        fmt->i_frame_length = A52_FRAME_NB;
    }
    else
    {
        aout_FormatPrepare (fmt);
        sys->bits = fmt->i_bitspersample;
    }

    aout->time_get = TimeGet;
    aout->play = Play;
    if (snd_pcm_hw_params_can_pause (hw))
        aout->pause = Pause;
    else
    {
        aout->pause = PauseDummy;
        msg_Warn (aout, "device cannot be paused");
    }
    aout->flush = Flush;

    /* Setup audio-device choices */
    {
        vlc_value_t text;

        var_Create (aout, "audio-device", VLC_VAR_STRING | VLC_VAR_HASCHOICE);
        text.psz_string = _("Audio Device");
        var_Change (aout, "audio-device", VLC_VAR_SETTEXT, &text, NULL);

        GetDevices (VLC_OBJECT(aout), device);
    }
    var_AddCallback (aout, "audio-device", DeviceChanged, NULL);

    free (device);
    aout_SoftVolumeStart (aout);
    return 0;

error:
    snd_pcm_close (pcm);
    free (device);
    return VLC_EGENERIC;
}

static int TimeGet (audio_output_t *aout, mtime_t *restrict delay)
{
    aout_sys_t *sys = aout->sys;
    snd_pcm_sframes_t frames;

    int val = snd_pcm_delay (sys->pcm, &frames);
    if (val)
    {
        msg_Err (aout, "cannot estimate delay: %s", snd_strerror (val));
        return -1;
    }
    *delay = frames * CLOCK_FREQ / sys->rate;
    return 0;
}

/**
 * Queues one audio buffer to the hardware.
 */
static void Play (audio_output_t *aout, block_t *block)
{
    aout_sys_t *sys = aout->sys;

    if (sys->chans_to_reorder != 0)
        aout_ChannelReorder(block->p_buffer, block->i_buffer,
                           sys->chans_to_reorder, sys->chans_table, sys->bits);

    snd_pcm_t *pcm = sys->pcm;

    /* TODO: better overflow handling */
    /* TODO: no period wake ups */

    while (block->i_nb_samples > 0)
    {
        snd_pcm_sframes_t frames;

        frames = snd_pcm_writei (pcm, block->p_buffer, block->i_nb_samples);
        if (frames >= 0)
        {
            size_t bytes = snd_pcm_frames_to_bytes (pcm, frames);
            block->i_nb_samples -= frames;
            block->p_buffer += bytes;
            block->i_buffer -= bytes;
            // pts, length
        }
        else  
        {
            int val = snd_pcm_recover (pcm, frames, 1);
            if (val)
            {
                msg_Err (aout, "cannot recover playback stream: %s",
                         snd_strerror (val));
                DumpDeviceStatus (aout, pcm);
                break;
            }
            msg_Warn (aout, "cannot write samples: %s", snd_strerror (frames));
        }
    }
    block_Release (block);
}

/**
 * Pauses/resumes the audio playback.
 */
static void Pause (audio_output_t *aout, bool pause, mtime_t date)
{
    snd_pcm_t *pcm = aout->sys->pcm;

    int val = snd_pcm_pause (pcm, pause);
    if (unlikely(val))
        PauseDummy (aout, pause, date);
}

static void PauseDummy (audio_output_t *aout, bool pause, mtime_t date)
{
    snd_pcm_t *pcm = aout->sys->pcm;

    /* Stupid device cannot pause. Discard samples. */
    if (pause)
        snd_pcm_drop (pcm);
    else
        snd_pcm_prepare (pcm);
    (void) date;
}

/**
 * Flushes/drains the audio playback buffer.
 */
static void Flush (audio_output_t *aout, bool wait)
{
    snd_pcm_t *pcm = aout->sys->pcm;

    if (wait)
        snd_pcm_drain (pcm);
    else
        snd_pcm_drop (pcm);
    snd_pcm_prepare (pcm);
}


/**
 * Releases the audio output.
 */
static void Stop (audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    snd_pcm_t *pcm = sys->pcm;

    var_DelCallback (aout, "audio-device", DeviceChanged, NULL);
    var_Destroy (aout, "audio-device");

    snd_pcm_drop (pcm);
    snd_pcm_close (pcm);
}

/**
 * Enumerates ALSA output devices.
 */
static int EnumDevices(vlc_object_t *obj, char const *varname,
                       char ***restrict vp, char ***restrict tp)
{
    unsigned n = 0;

    char **names = xmalloc(sizeof (*names));
    char **descs = xmalloc(sizeof (*names));
    names[0] = strdup ("default");
    descs[0] = strdup (N_("Default"));
    n++;
    if (unlikely(names[0] == NULL || descs[0] == NULL))
        abort();

    void **hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return n;

    for (size_t i = 0; hints[i] != NULL; i++)
    {
        void *hint = hints[i];

        char *name = snd_device_name_get_hint(hint, "NAME");
        if (unlikely(name == NULL))
            continue;

        char *desc = snd_device_name_get_hint(hint, "DESC");
        if (desc == NULL)
        {
            free (name);
            continue;
        }

        names = xrealloc (names, (n + 1) * sizeof (*names));
        descs = xrealloc (descs, (n + 1) * sizeof (*descs));
        names[n] = name;
        descs[n] = desc;
        n++;
    }
    snd_device_name_free_hint(hints);

    (void) obj; (void) varname;
    *vp = names;
    *tp = descs;
    return n;
}


static void GetDevices(vlc_object_t *obj, const char *prefs_dev)
{
    void **hints;

    msg_Dbg(obj, "Available ALSA PCM devices:");
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return;

    vlc_value_t val, text;
    bool hinted_default = false;
    bool hinted_prefs = !strcmp (prefs_dev, "default");

    for (size_t i = 0; hints[i] != NULL; i++)
    {
        void *hint = hints[i];

        char *name = snd_device_name_get_hint(hint, "NAME");
        if (unlikely(name == NULL))
            continue;

        char *desc = snd_device_name_get_hint(hint, "DESC");
        if (desc != NULL)
            for (char *lf = strchr(desc, '\n'); lf; lf = strchr(lf, '\n'))
                 *lf = ' ';
        msg_Dbg(obj, "%s (%s)", (desc != NULL) ? desc : name, name);

        if (!strcmp (name, "default"))
            hinted_default = true;
        if (!strcmp (name, prefs_dev))
            hinted_prefs = true;

        val.psz_string = name;
        text.psz_string = desc;
        var_Change(obj, "audio-device", VLC_VAR_ADDCHOICE, &val, &text);
        free(desc);
        free(name);
    }

    snd_device_name_free_hint(hints);

    if (!hinted_default)
    {
        val.psz_string = (char *)"default";
        text.psz_string = (char *)N_("Default");
        var_Change(obj, "audio-device", VLC_VAR_ADDCHOICE, &val, &text);
    }

    val.psz_string = (char *)prefs_dev;
    if (!hinted_prefs)
    {
        text.psz_string = (char *)N_("VLC preferences");
        var_Change(obj, "audio-device", VLC_VAR_ADDCHOICE, &val, &text);
    }
    var_Change(obj, "audio-device", VLC_VAR_SETVALUE, &val, NULL);
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = malloc (sizeof (*sys));

    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout_SoftVolumeInit (aout);
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = aout->sys;

    free(sys);
}
