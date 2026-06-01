#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALLEGRO_HAVE_STDINT_H
#include <allegro.h>
#ifndef DIGI_SBPRO2
#define DIGI_SBPRO2 DIGI_SBPRO
#endif

#include "libavutil/common.h"
#include "mpbswap.h"
#include "subopt-helper.h"
#include "libaf/af_format.h"
#include "audio_out.h"
#include "audio_out_internal.h"
#include "mp_msg.h"
#include "help_mp.h"


static ao_info_t info = 
{
	"Allegro audio output",
	"allegro",
	"Michael Kostylev <mik@niipt.ru>",
	"experimental version, try `-autosync 30` to prevent a/v sync errors"
};

LIBAO_EXTERN(allegro)

static AUDIOSTREAM *stream;
static void *mem_chunk;
static int bits;
static unsigned long total_samples;
static struct timeval play_start;

// to set/get/query special features/parameters
static int control(int cmd,void *arg)
{
	int format, volume;
	ao_control_vol_t* ao_vol;

	switch(cmd)
	{
	case AOCONTROL_QUERY_FORMAT:
		format = (int)arg;
		switch(format)
		{
			case AF_FORMAT_S8:
			case AF_FORMAT_U8:
			case AF_FORMAT_S16_BE:
	                case AF_FORMAT_U16_BE:
	                case AF_FORMAT_S16_LE:
			case AF_FORMAT_U16_LE:
				return CONTROL_OK;
			default:
				return CONTROL_FALSE;
		}
	case AOCONTROL_GET_VOLUME:
		ao_vol = (ao_control_vol_t*)arg;
		volume = voice_get_volume(stream->voice);
		ao_vol->left = ao_vol->right = volume / 2.55;
		return CONTROL_OK;
	case AOCONTROL_SET_VOLUME:
		ao_vol = (ao_control_vol_t*)arg;
		volume = ao_vol->left * 255 / 100;
		voice_set_volume(stream->voice, volume);
		return CONTROL_OK;
	}
	return CONTROL_UNKNOWN;
}

// open & setup audio device
// return: 1=success 0=fail
static int init(int rate,int channels,int format,int flags)
{
	unsigned int outburst = 16384;
	int volume = 255, voices = 1, card_id = DIGI_AUTODETECT;
	opt_t subopts[] = {
	  {"buffer",       OPT_ARG_INT, &outburst, (opt_test_f)int_non_neg},
	  {"volume",       OPT_ARG_INT, &volume, (opt_test_f)int_non_neg},
	  {"voices",       OPT_ARG_INT, &voices, (opt_test_f)int_non_neg},
	  {"card_id",      OPT_ARG_INT, &card_id, NULL},
	  {NULL}
	};

	if (subopt_parse(ao_subdevice, subopts) != 0)
	{
		mp_msg(MSGT_VO, MSGL_FATAL,
			"\n-ao allegro command line help:\n"
			"Example: mplayer -ao allegro:buffer=32768\n"
			"\nOptions:\n"
			"  buffer=<4096..65535>\n"
			"    Buffer size, must be not too small (16384 bytes by default).\n"
			"  volume=<0..255>\n"
			"    Initial volume level (255 by default).\n"
			"  card_id=<id>\n"
			"    Id can be one of the following:\n"
			"    %d (SB1.0), %d (SB1.5), %d (SB2.0),\n"
			"    %d (SBPro), %d (SBPro2), %d (SB16)\n"
			"    %d (ESS Audiodrive), %d (Ensoniq Soundscape),\n"
			"    %d (WSS)\n"
			"\n", 
			DIGI_SB10, DIGI_SB15, DIGI_SB20, DIGI_SBPRO, DIGI_SBPRO2,
			DIGI_SB16, DIGI_AUDIODRIVE, DIGI_SOUNDSCAPE, DIGI_WINSOUNDSYS);
		return 0;
	}
	
	if (outburst < 4096) outburst = 4096;
	mp_msg(MSGT_AO,MSGL_DBG2, "ao_allegro: buffer size=%u\n", outburst);

	switch(format)
	{
		case AF_FORMAT_S16_BE:
		case AF_FORMAT_U16_BE:
		case AF_FORMAT_S16_LE:
		case AF_FORMAT_U16_LE:
			bits = 16;
			ao_data.bps = channels * rate * 2;
			break;
		case AF_FORMAT_S8:
		case AF_FORMAT_U8:
			bits = 8;
			ao_data.bps = channels * rate;
			break;
		default:
		{
			mp_msg(MSGT_AO,MSGL_ERR, "ao_allegro: unsupported format %s\n", af_fmt2str_short(format));
			return 0;
		}
	}
	if(channels > 2)
	{
		mp_msg(MSGT_AO,MSGL_ERR, "ao_allegro: unable to play %d channel sound\n", channels);
		return 0;
	}
	if(allegro_init() != 0)
	{
		mp_msg(MSGT_AO,MSGL_ERR, "ao_allegro: error initialising library\n");
		return 0;
	}

	reserve_voices(voices, 0);

	if(install_sound(card_id, MIDI_NONE, NULL) != 0)
	{
		mp_msg(MSGT_AO,MSGL_ERR, "ao_allegro: error initialising sound system\n");
		mp_msg(MSGT_AO,MSGL_ERR, "ao_allegro: %s\n", allegro_error);
		return 0;
	}

	set_volume(255, -1);
	mp_msg(MSGT_AO, MSGL_INFO, "ao_allegro: sound system initialized. Driver: %s (%s)\n", digi_driver->name, digi_driver->desc);

	ao_data.samplerate = rate;
	ao_data.channels = channels;
	ao_data.format = format;
	ao_data.outburst = outburst;
	ao_data.buffersize = outburst;

	mp_msg(MSGT_AO,MSGL_DBG2, "ao_allegro: stream - buffer=%d, bits=%d, stereo=%d, rate=%d\n",
		ao_data.outburst / (ao_data.channels * bits / 8), bits, channels - 1, rate);

	stream = play_audio_stream(ao_data.outburst / (ao_data.channels * bits / 8), bits, \
		ao_data.channels - 1, ao_data.samplerate, volume, 127);

	if (stream == NULL)
	{
		mp_msg(MSGT_AO,MSGL_ERR, "ao_allegro: error creating audio stream!\n");
		return 0;
	}
	return 1;
}

// close audio device
static void uninit(int immed)
{
	stop_audio_stream(stream);
}

// stop playing and empty buffers (for seeking/pause)
static void reset(void)
{
//	inaccurate :(
	int volume = voice_get_volume(stream->voice);
	stop_audio_stream(stream);
	stream = play_audio_stream(ao_data.outburst / (ao_data.channels * bits / 8), bits, \
		ao_data.channels - 1, ao_data.samplerate, volume, 127);
}

// stop playing, keep buffers (for pause)
static void audio_pause(void)
{
	voice_stop(stream->voice);
}

// resume playing, after audio_pause()
static void audio_resume(void)
{
	voice_start(stream->voice);
}

// return: how many bytes can be played without blocking
static int get_space(void)
{
	mem_chunk = get_audio_stream_buffer(stream);
	if (mem_chunk != NULL)
	{
		return ao_data.outburst;
	}
	return 0;
}

// puts to destination (stream) buffer unsigned (le) data of 'len' bytes
// 'len' should be even for 16-bit formats
void mkunsigned (void *dest, void *src, int len)
{
	int i;

	switch(ao_data.format)
	{
		case AF_FORMAT_S16_LE:
			for (i = 0; i < len/2; i++)
				((uint16_t *)dest)[i] = ((uint16_t *)src)[i] ^ 0x8000;
			break;
		case AF_FORMAT_S16_BE:
			for (i = 0; i < len; i += 2)
			{
				((uint8_t *)dest)[i+1] = ((uint8_t *)src)[i] ^ 0x80;
				((uint8_t *)dest)[i] = ((uint8_t *)src)[i+1];
			}
			break;
		case AF_FORMAT_U16_BE:
			swab(src, dest, len);
			break;
		case AF_FORMAT_S8:
			for (i = 0; i < len; i++)
				((uint8_t *)dest)[i] = ((uint8_t *)src)[i] ^ 0x80;
			break;
		default:
			memcpy (dest, src, len);
	}
}

// fills buffer of 'len' bytes with silence
void mksilence (void *data, int len)
{
	int i;

	if (bits == 8)
		for (i = 0; i < len; i++)
			((uint8_t *)data)[i] = 0x80;
	else
		for (i = 0; i < len/2; i++)
			((uint16_t *)data)[i] = 0x8000;
}

// plays 'len' bytes of 'data'
// it should round it down to outburst*n
// return: number of bytes played
static int play(void* data,int len,int flags)
{
	if (len <= 0)
		return 0;
	if (len > ao_data.outburst || !(flags & AOPLAY_FINAL_CHUNK))
		len = ao_data.outburst;
	if (mem_chunk != NULL) 
	{
		mkunsigned (mem_chunk, data, len);
		if (flags & AOPLAY_FINAL_CHUNK)
			mksilence((int8_t *)(mem_chunk) + len, ao_data.outburst - len);
		free_audio_stream_buffer(stream);
		if (!play_start.tv_sec)
			gettimeofday(&play_start, NULL);
		total_samples += len / (ao_data.channels * bits / 8);
		return len;
	}
	return 0;
}

// return: delay in seconds between first and last sample in buffer
static float get_delay(void)
{
	struct timeval now;
	double buffered_samples_time;
	double play_time;

	if (!play_start.tv_sec)
		return 0;
	buffered_samples_time = (float)total_samples / ao_data.samplerate;
	gettimeofday (&now, NULL);
	play_time  =  now.tv_sec  - play_start.tv_sec;
	play_time += (now.tv_usec - play_start.tv_usec) / 1000000.0;

	if (play_time > buffered_samples_time)
	{
//		mp_msg(MSGT_AO,MSGL_WARN, "\nao_allegro: underflow\n");
		play_start.tv_sec = 0;
		total_samples = 0;
		return 0;
	}
    
	return buffered_samples_time - play_time;
        
}
