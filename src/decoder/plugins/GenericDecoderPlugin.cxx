/*
 * The Music Player Daemon (MPD)
 * (C) 2003-2004 by Warren Dukes <shank@mercury.chem.pitt.edu>
 * (C) 2006-2007 by Jan-Benedict Glaw <jbglaw@lug-owl.de>
 * (C) 2014      by mitsuman <kmkz.mtm@gmail.com>
 * This project's homepage is: http://www.musicpd.org/
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * This source file implements a generic input module for audio files.
 * It calls external decoders that are expected to produce raw PCM data.
 */

#include "config.h"
#include "GenericDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "tag/TagHandler.hxx"
#include "fs/Path.hxx"
#include "util/Alloc.hxx"
#include "util/FormatString.hxx"
#include "util/UriUtil.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "config/ConfigGlobal.hxx"
#include "Log.hxx"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static constexpr Domain generic_domain("generic");

#define CONF_GENERIC_DECODER_SUFFIX		"suffix"
#define CONF_GENERIC_DECODER_MIME_TYPE		"mime_type"
#define CONF_GENERIC_DECODER_PCM_FORMAT		"pcm_format"
#define CONF_GENERIC_DECODER_PROGRAM		"program"

bool generic_scan_file(Path path_fs, const struct tag_handler *handler, void *handler_ctx);
void generic_decode_file(Decoder &output, Path path_fs);

/*
 * The "decode_program" will be called with two arguments for playing
 * streams (which are fed in via stdin):
 *
 * <decode_program> "streamrawdecode" "mime type or file suffix"
 */
static struct generic_decoder {
	char *suffix;
	char *mime_type;
	char *decode_program;
	AudioFormat audio_format;
	struct generic_decoder *next;
} *generic_decoder_list = NULL;

/*
 * Internal representation of a program which we can read from.
 */
struct generic_program {
#define RD	0
#define WR	1
	int child_stdin[2];
	int child_stdout[2];
	pid_t pid;
};

static char **generic_suffixes = NULL;
static char **generic_mime_types = NULL;
static int generic_num_suffixes = 0;
static int generic_num_mime_types = 0;
//struct DecoderPlugin generic_decoder_plugin;
//InputPlugin genericPlugin;

#define DEBUG(...)
#define LOG(...) FormatDebug(generic_domain, __VA_ARGS__)
#define WARNING(...) FormatWarning(generic_domain, __VA_ARGS__)
#define ERROR(...) FormatError(generic_domain, __VA_ARGS__)

static int
add_decoder (const char *suffix, const char *mime_type,
	     AudioFormat *audio_format, const char *program)
{
	struct generic_decoder *new_decoder;

	/* Check input.  */
	if ((! suffix && ! mime_type) || ! audio_format || ! program)
		return -1;

	LOG ("%s: \"%s\" with %d channels, %lu frequency and %d bits per "
	     "sample for %s/%s\n", __FUNCTION__, program,
	     audio_format->channels, (unsigned long) audio_format->sample_rate,
	     audio_format->GetSampleSize() * 8, suffix? suffix: "-", mime_type? mime_type: "-");

	/* Allocate new slot.  */
	new_decoder = (generic_decoder*) xalloc (sizeof (*new_decoder));
	memset (new_decoder, 0x00, sizeof (*new_decoder));

	/* Set values.  */
	if (suffix)
		new_decoder->suffix = xstrdup (suffix);
	if (mime_type)
		new_decoder->mime_type = xstrdup (mime_type);
	new_decoder->decode_program = xstrdup (program);
	new_decoder->audio_format = *audio_format;

	/* Queue in.  */
	new_decoder->next = generic_decoder_list;
	generic_decoder_list = new_decoder;

	/* Extend suffix/mime_type list.  */
	if (suffix) {
		generic_suffixes = (char**) xrealloc (generic_suffixes,
					     sizeof (void *)
					     * (generic_num_suffixes + 2));
		generic_suffixes[generic_num_suffixes++] = xstrdup (suffix);
		generic_suffixes[generic_num_suffixes] = NULL;
	}
	if (mime_type) {
		generic_mime_types = (char**) xrealloc (generic_mime_types,
					       sizeof (void *)
					       * (generic_num_mime_types + 2));
		generic_mime_types[generic_num_mime_types++] = xstrdup (mime_type);
		generic_mime_types[generic_num_mime_types] = NULL;
	}

	return 0;
}

static struct generic_decoder *
generic_decoder_find (const char *suffix, const char *mime_type)
{
	struct generic_decoder *decoder;

	for (decoder = generic_decoder_list; decoder; decoder = decoder->next) {
		if (suffix
		    && decoder->suffix
		    && strcasecmp (decoder->suffix, suffix) == 0)
			return decoder;
		if (mime_type
		    && decoder->mime_type
		    && strcasecmp (decoder->mime_type, mime_type) == 0)
			return decoder;
	}

	return NULL;
}

/*
 *
 * Execution helper
 *
 */
static int
generic_start_program (struct generic_program *program,
		       const char *file,
		       char const *const *arg)
{
	int ret;
	int i;

	if (! program || ! file || ! arg || ! arg[0]) {
		ERROR ("%s: empty program/file/arg/arg[0]\n", __FUNCTION__);
		return -1;
	}

	memset (program, 0x00, sizeof (*program));

	ret = pipe (program->child_stdout);
	if (ret) {
		ERROR ("%s: 1st pipe() failed.\n", __FUNCTION__);
		return errno;
	}

	ret = pipe (program->child_stdin);
	if (ret) {
		ERROR ("%s: 2nd pipe() failed.\n", __FUNCTION__);
		close (program->child_stdout[RD]);
		close (program->child_stdout[WR]);
		return ret;
	}

	program->pid = fork ();
	if (program->pid < 0) {
		/*
		 * fork() failed.
		 */
		ERROR ("%s: fork() failed\n", __FUNCTION__);
		close (program->child_stdout[RD]);
		close (program->child_stdout[WR]);
		close (program->child_stdin[RD]);
		close (program->child_stdin[WR]);
		return (int) program->pid;
	} else if (program->pid > 0) {
		/*
		 * Parent.
		 */
		close (program->child_stdout[WR]);
		close (program->child_stdin[RD]);
		return 0;
	} else {
		struct sigaction sa;

		/*
		 * Child
		 */
		close (program->child_stdout[RD]);
		close (program->child_stdin[WR]);
		for (i = 0; i < getdtablesize (); i++) {
			if (program->child_stdout[WR] != i
			    && program->child_stdin[RD] != i)
				close (i);
		}

		/*
		 * Clear signal handling.
		 */
		sa.sa_flags = 0;
		sigemptyset (&sa.sa_mask);

		sa.sa_handler = SIG_DFL;
		while (sigaction (SIGCHLD, &sa, NULL) && errno == EINTR)
			/* loop */;

		sa.sa_handler = SIG_DFL;
		while (sigaction (SIGPIPE, &sa, NULL) && errno == EINTR)
			/* loop */;

		/*
		 * Fix stdin/stdout to be properly connected to the parend.
		 */
		ret = dup2 (program->child_stdin[RD], STDIN_FILENO);
		if (ret != STDIN_FILENO) {
			/*
			 * We're the child, already had all our file
			 * descriptors closed and cannot complain. Just
			 * die.
			 */
			_exit (EXIT_FAILURE);
		}

		ret = dup2 (program->child_stdout[WR], STDOUT_FILENO);
		if (ret != STDOUT_FILENO) {
			/*
			 * We're the child, already had all our file
			 * descriptors closed and cannot complain. Just
			 * die.
			 */
			_exit (EXIT_FAILURE);
		}

		/*
		 * Run Forrest, run!
		 */
		execvp (file, (char*const*)arg);

		/* Havoc.  */
		_exit (EXIT_FAILURE);
	} /* Child */
}

static int
start_file_decoder (const struct generic_decoder *decoder,
					struct generic_program *program,
					const char *file)
{
	char const *arg[4];
	int ret;

	if (! decoder || ! program) {
		ERROR ("%s: empty decoder/program\n", __FUNCTION__);
		return -1;
	}

	arg[0] = decoder->decode_program;
	arg[1] = "filerawdecode";
	arg[2] = file;
	arg[3] = NULL;

	ret = generic_start_program (program, decoder->decode_program, arg);

	return ret;
}

static ssize_t
generic_read_data (const struct generic_program *program, char *buf,
		   const size_t len)
{
	ssize_t ret_len = -1;

	if (! program || ! buf || len == 0) {
		ERROR ("%s: null progam/buf/len\n", __FUNCTION__);
		return -1;
	}

	ret_len = read (program->child_stdout[RD], buf, len);
	if (ret_len < 0 && errno == EAGAIN)
		ret_len = 0;

	return ret_len;
}

static int
generic_finish_program (struct generic_program *program)
{
	pid_t wait_ret;
	int ret = 0;
	int status;

	/* Either it's already dead, or we shoot it.  */
	kill (program->pid, SIGKILL);

	/* Reap child's status now.  */
	wait_ret = waitpid (program->pid, &status, 0);
	if (wait_ret != program->pid) {
		WARNING ("%s: We lost our child #%d\n", __FUNCTION__,
			 (int) program->pid);
		ret = -1;
	}

	close (program->child_stdout[RD]);
	close (program->child_stdin[WR]);

	return ret;
}

static char *
generic_read_all_data (struct generic_program *program)
{
	char *temp = NULL;
	char minibuf[100];
	size_t read_sum = 0;
	ssize_t read_num;

	/* Read the tag output generated by the tagreader program.  */
	do {
		read_num = generic_read_data (program, minibuf,
					      sizeof (minibuf));
		if (read_num > 0) {
			temp = (char*) xrealloc (temp, read_sum + read_num + 1);
			memcpy (temp + read_sum, minibuf, read_num);
			read_sum += read_num;
			temp[read_sum] = '\0';
		}
	} while (read_num > 0);

	return temp;
}

static char *
generic_get_one_tag (struct generic_decoder *tagread, const char *tagname,
		     const char *filename)
{
	struct generic_program program;
	const char *arg[5];
	int ret;
	char *result;

	if (!tagread)
		return NULL;

	arg[0] = tagread->decode_program;
	arg[1] = "gettag";
	arg[2] = tagname;
	arg[3] = filename;
	arg[4] = NULL;

	ret = generic_start_program (&program, tagread->decode_program, arg);
	if (ret)
		return NULL;

	result = generic_read_all_data (&program);

	generic_finish_program (&program);

	return result;
}

/*
 *
 * Functions for the inputPlugin.
 *
 */
static void
generic_fini (void)
{
	static struct generic_decoder *decoder, *next_decoder;
	int i;

	/* Free decoder list.  */
	for (decoder = generic_decoder_list; decoder; decoder = next_decoder) {
		next_decoder = decoder->next;

		if (decoder->suffix)
			free (decoder->suffix);
		if (decoder->mime_type)
			free (decoder->mime_type);
		free (decoder->decode_program);
		free (decoder);
	}
	generic_decoder_list = NULL;

	/* Free suffix list.  */
	if (generic_suffixes) {
		for (i = 0; generic_suffixes[i]; i++)
			free (generic_suffixes[i]);
		free (generic_suffixes);
		generic_suffixes = NULL;
	}
	generic_decoder_plugin.suffixes = NULL;

	/* Free mime_type list.  */
	if (generic_mime_types) {
		for (i = 0; generic_mime_types[i]; i++)
			free (generic_mime_types[i]);
		free (generic_mime_types);
		generic_mime_types = NULL;
	}
	generic_decoder_plugin.mime_types = NULL;

	return;
}

static bool
generic_init (const config_param &)
{
	AudioFormat audio_format(44100, SampleFormat::S16, 2);
	int num_decoders = 0;

	const config_param *param_block = 0;

	/* Process generic decoders.  */
	do {
		param_block = config_get_next_param(CONF_GENERIC_DECODER, param_block);
		if (param_block) {
			const block_param *param_suffix  = param_block->GetBlockParam (CONF_GENERIC_DECODER_SUFFIX);
			const block_param *param_mime    = param_block->GetBlockParam (CONF_GENERIC_DECODER_MIME_TYPE);
			const block_param *param_pcm_fmt = param_block->GetBlockParam (CONF_GENERIC_DECODER_PCM_FORMAT);
			const block_param *param_program = param_block->GetBlockParam (CONF_GENERIC_DECODER_PROGRAM);

			if (
			    ! param_program
				|| ! param_pcm_fmt
			    || (! param_suffix && ! param_mime)
			    //|| parseAudioConfig (&audio_format, param_pcm_fmt->value)  // TODO(mitsuman) support audio pcm format like "44100:16:2"
			    || add_decoder (param_suffix ? param_suffix->value.c_str(): NULL,
								param_mime ? param_mime->value.c_str(): NULL,
				&audio_format,
				param_program->value.c_str())) {
				ERROR ("%s: suffix, mime_type, pcm_format or "
				       "program missing in line %d\n",
				       __FUNCTION__, param_block->line);
				generic_fini ();
				return false;
			}

			num_decoders++;
		}
	} while (param_block);

	if (num_decoders == 0)
		return false;

	generic_decoder_plugin.suffixes = generic_suffixes;
	generic_decoder_plugin.mime_types = generic_mime_types;

	return true;
}

bool generic_scan_file(Path path_fs,
					   const struct tag_handler *handler,
					   void *handler_ctx) {
	struct generic_decoder *tagread = generic_decoder_find (uri_get_suffix (path_fs.c_str()), 0);
        if (!tagread)
                return false;

	struct tag_type_list {
		const char *name;
		TagType number;
	} tag_type_list[] = {
		{ .name = "artist",	.number = TAG_ARTIST,	},
		//{ .name = "album",	.number = TAG_ALBUM,	},
		{ .name = "title",	.number = TAG_TITLE,	},
		//{ .name = "track",	.number = TAG_TRACK,	},
		//{ .name = "name",	.number = TAG_NAME,	},
		//{ .name = "genre",	.number = TAG_GENRE,	},
		{ .name = "date",	.number = TAG_DATE,	},
		{ .name = "composer",	.number = TAG_COMPOSER,	},
		//{ .name = "performer",	.number = TAG_PERFORMER,	},
		{ .name = "comment",	.number = TAG_COMMENT,	},
		//{ .name = "disc",	.number = TAG_DISC,	},
	};

	DEBUG("scan:%s %x", path_fs.c_str(), tagread);

	for (unsigned int i = 0; i < sizeof(tag_type_list) / sizeof(*tag_type_list); i++) {
		char *text = generic_get_one_tag (tagread, tag_type_list[i].name, path_fs.c_str());
		if (!text) {
			continue;
		}
		DEBUG("scan %s:%s", tag_type_list[i].name, text);
		tag_handler_invoke_tag(handler, handler_ctx,
							   tag_type_list[i].number, text);
		free(text);
	}

	char *text = generic_get_one_tag (tagread, "time", path_fs.c_str());
	if (text) {
		tag_handler_invoke_duration(handler, handler_ctx, atoi(text));
		free (text);
	} else {
		tag_handler_invoke_duration(handler, handler_ctx, 0);
	}

	return true;
}

void generic_decode_file(Decoder &output, Path path_fs) {
	struct generic_decoder *decoder;
	struct generic_program program;

	/* Do we have a decoder capable of streaming and handling the suffix?  */
	decoder = generic_decoder_find (uri_get_suffix (path_fs.c_str()),
									NULL);

	/* No decoder? No decoding...  */
	if (! decoder) {
		ERROR ("%s: Did not find a decoder?!\n", __FUNCTION__);
		return;
	}

	Error error;
	AudioFormat audio_format;
	const float song_len = -1;
	decoder_initialized(output, decoder->audio_format, true, song_len);

	int ret;
	//ret = start_stream_decoder (decoder, &program);
	ret = start_file_decoder(decoder, &program, path_fs.c_str());
	if (ret) {
		ERROR ("%s: Failed to start decoder for %s/%s\n",
		       __FUNCTION__,
		       decoder->suffix? decoder->suffix: "-",
		       decoder->mime_type? decoder->mime_type: "-");
		return;
	}
	DEBUG("audio decode starting");

	/* play */
	DecoderCommand cmd;
	do {
		char pcm_data[4410 * 2 * 2]; // 10ms cd quality


		/* Try to read data from decoder.  */
		int pcm_data_len = generic_read_data (&program, pcm_data,
						  sizeof (pcm_data));
		if (pcm_data_len <= 0) {
			DEBUG("audio decode len 0");
			break;
		}

		cmd = decoder_data(output, nullptr, pcm_data, pcm_data_len, 0);

		if (cmd == DecoderCommand::SEEK) {
			//float where = decoder_seek_where(output);
			decoder_command_finished(output);
		}
	} while (cmd != DecoderCommand::STOP);
	DEBUG("audio decode complete");

	generic_finish_program (&program);
}

struct DecoderPlugin generic_decoder_plugin = {
	"generic",
	generic_init,
	generic_fini,
	nullptr, //generic_decode_stream,
	generic_decode_file,
	generic_scan_file,
	nullptr, //generic_scan_stream,
	nullptr, /* container scan */
	nullptr, /* .suffixes is dynamically assigned in generic_init().  */
	nullptr, /* .mime_types is dynamically assigned in generic_init().  */
};
