/*
 * Copyright (C) 2003-2015 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "MikmodDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "tag/TagHandler.hxx"
#include "system/FatalError.hxx"
#include "fs/Path.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <mikmod.h>

#include <assert.h>

static constexpr Domain mikmod_domain("mikmod");

/* this is largely copied from alsaplayer */

static constexpr size_t MIKMOD_FRAME_SIZE = 4096;

static BOOL
mikmod_mpd_init(void)
{
	return VC_Init();
}

static void
mikmod_mpd_exit(void)
{
	VC_Exit();
}

static void
mikmod_mpd_update(void)
{
}

static BOOL
mikmod_mpd_is_present(void)
{
	return true;
}

static char drv_name[] = PACKAGE_NAME;
static char drv_version[] = VERSION;

#if (LIBMIKMOD_VERSION > 0x030106)
static char drv_alias[] = PACKAGE;
#endif

static MDRIVER drv_mpd = {
	nullptr,
	drv_name,
	drv_version,
	0,
	255,
#if (LIBMIKMOD_VERSION > 0x030106)
	drv_alias,
#if (LIBMIKMOD_VERSION >= 0x030200)
	nullptr,  /* CmdLineHelp */
#endif
	nullptr,  /* CommandLine */
#endif
	mikmod_mpd_is_present,
	VC_SampleLoad,
	VC_SampleUnload,
	VC_SampleSpace,
	VC_SampleLength,
	mikmod_mpd_init,
	mikmod_mpd_exit,
	nullptr,
	VC_SetNumVoices,
	VC_PlayStart,
	VC_PlayStop,
	mikmod_mpd_update,
	nullptr,
	VC_VoiceSetVolume,
	VC_VoiceGetVolume,
	VC_VoiceSetFrequency,
	VC_VoiceGetFrequency,
	VC_VoiceSetPanning,
	VC_VoiceGetPanning,
	VC_VoicePlay,
	VC_VoiceStop,
	VC_VoiceStopped,
	VC_VoiceGetPosition,
	VC_VoiceRealVolume
};

static bool mikmod_loop;
static unsigned mikmod_sample_rate;

static bool
mikmod_decoder_init(const ConfigBlock &block)
{
	static char params[] = "";

	mikmod_loop = block.GetBlockValue("loop", false);
	mikmod_sample_rate = block.GetBlockValue("sample_rate", 44100u);
	if (!audio_valid_sample_rate(mikmod_sample_rate))
		FormatFatalError("Invalid sample rate in line %d: %u",
				 block.line, mikmod_sample_rate);

	md_device = 0;
	md_reverb = 0;

	MikMod_RegisterDriver(&drv_mpd);
	MikMod_RegisterAllLoaders();

	md_pansep = 64;
	md_mixfreq = mikmod_sample_rate;
	md_mode = (DMODE_SOFT_MUSIC | DMODE_INTERP | DMODE_STEREO |
		   DMODE_16BITS);

	if (MikMod_Init(params)) {
		FormatError(mikmod_domain,
			    "Could not init MikMod: %s",
			    MikMod_strerror(MikMod_errno));
		return false;
	}

	return true;
}

static void
mikmod_decoder_finish(void)
{
	MikMod_Exit();
}

static void
mikmod_decoder_file_decode(Decoder &decoder, Path path_fs)
{
	/* deconstify the path because libmikmod wants a non-const
	   string pointer */
	char *const path2 = const_cast<char *>(path_fs.c_str());

	MODULE *handle;
	int ret;
	SBYTE buffer[MIKMOD_FRAME_SIZE];

	handle = Player_Load(path2, 128, 0);

	if (handle == nullptr) {
		FormatError(mikmod_domain,
			    "failed to open mod: %s", path_fs.c_str());
		return;
	}

	handle->loop = mikmod_loop;

	const AudioFormat audio_format(mikmod_sample_rate, SampleFormat::S16, 2);
	assert(audio_format.IsValid());

	decoder_initialized(decoder, audio_format, false,
			    SignedSongTime::Negative());

	Player_Start(handle);

	DecoderCommand cmd = DecoderCommand::NONE;
	while (cmd == DecoderCommand::NONE && Player_Active()) {
		ret = VC_WriteBytes(buffer, sizeof(buffer));
		cmd = decoder_data(decoder, nullptr, buffer, ret, 0);
	}

	Player_Stop();
	Player_Free(handle);
}

static bool
mikmod_decoder_scan_file(Path path_fs,
			 const struct tag_handler *handler, void *handler_ctx)
{
	/* deconstify the path because libmikmod wants a non-const
	   string pointer */
	char *const path2 = const_cast<char *>(path_fs.c_str());

	MODULE *handle = Player_Load(path2, 128, 0);

	if (handle == nullptr) {
		FormatDebug(mikmod_domain,
			    "Failed to open file: %s", path_fs.c_str());
		return false;
	}

	Player_Free(handle);

	char *title = Player_LoadTitle(path2);
	if (title != nullptr) {
		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_TITLE, title);
#if (LIBMIKMOD_VERSION >= 0x030200)
		MikMod_free(title);
#else
		free(title);
#endif
	}

	return true;
}

static const char *const mikmod_decoder_suffixes[] = {
	"amf",
	"dsm",
	"far",
	"gdm",
	"imf",
	"it",
	"med",
	"mod",
	"mtm",
	"s3m",
	"stm",
	"stx",
	"ult",
	"uni",
	"xm",
	nullptr
};

const struct DecoderPlugin mikmod_decoder_plugin = {
	"mikmod",
	mikmod_decoder_init,
	mikmod_decoder_finish,
	nullptr,
	mikmod_decoder_file_decode,
	mikmod_decoder_scan_file,
	nullptr,
	nullptr,
	mikmod_decoder_suffixes,
	nullptr,
};