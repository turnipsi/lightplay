/*
 * Copyright (c) 2019 Juha Erkkilä <juhaerk@icloud.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <math.h>
#include <poll.h>
#include <sndio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#ifdef HAVE_MERGESORT_IN_LIBBSD
#include <bsd/stdlib.h>
#endif

#define MIDI_NOTE_OFF			0x80
#define MIDI_NOTE_ON			0x90
#define MIDI_PROGRAM_CHANGE		0xc0
#define MIDI_CHANNEL_KEY_PRESSURE	0xd0

#define MIDI_SYSEX_EVENT_F0		0xf0
#define MIDI_SYSEX_EVENT_F7		0xf7
#define MIDI_META_EVENT			0xff

#define MIDI_META_SET_TEMPO		0x51

#define DEFAULT_MIDIEVENTS_SIZE 1024
#define MAX_ACTIVE_NOTES 128

enum midievent_type { MIDIEVENT_CHANNEL_VOICE, MIDIEVENT_TEMPO_CHANGE };

struct mididevice {
	struct mio_hdl		*dev;
	struct pollfd		*pfd;
	nfds_t			 nfds;
};

struct midievent {
	enum midievent_type	type;
	int			at_ticks;
	union {
		uint8_t		raw_midievent[3];
		int		tempo_in_microseconds_pqn;
	} u;
};

struct midievent_buffer {
	struct midievent	*events;
	size_t			 allocated_size;
	size_t			 event_count;
};

void	add_notes_waiting(int *, uint8_t *);
int	compare_midievent_positions(const void *, const void *);
void	close_mididevice(const struct mididevice *);
void	debugmsg(int, const char *, ...);
int	do_sequencing(FILE *, const struct mididevice *);
int	get_next_variable_length_quantity(FILE *, uint32_t *);
int	get_next_midi_event(FILE *, uint32_t *, struct midievent *, int *,
    uint8_t *);
int	notes_to_wait_for(int *);
int	open_mididevice(struct mididevice *);
int	parse_meta_event(FILE *, uint32_t *, struct midievent *, int *, int);
int	parse_next_track(FILE *, struct midievent_buffer *);
int	parse_smf_header(FILE *, uint16_t *, uint16_t *);
int	parse_standard_midi_file(FILE *, struct midievent_buffer *,
    uint16_t *);
int	playback_midievents(const struct mididevice *,
    struct midievent_buffer *, uint16_t);
int	turn_on_next_lights(const struct mididevice *,
    struct midievent_buffer *, size_t *, int *);
void	usage(void);
int	wait_for_event(const struct mididevice *, int, int *);
int	wait_for_notes(const struct mididevice *, int *, uint8_t *, size_t *);

/* XXX const could be used where applicable */

static int	debug_level;
static int	dry_run;

int
main(int argc, char *argv[])
{
	FILE *midifile;
	const char *midifile_path;
	struct mididevice mididev;
	int ch, ret;

	debug_level = 0;
	dry_run = 0;

	while ((ch = getopt(argc, argv, "dn")) != -1) {
		switch (ch) {
		case 'd':
			debug_level++;
			break;
		case 'n':
			dry_run = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	debugmsg(1, "starting up lightplay\n");

	midifile_path = argv[0];

	debugmsg(1, "opening midi file %s\n", midifile_path);
	if ((midifile = fopen(midifile_path, "r")) == NULL)
		err(1, "could not open midi file \"%s\"", midifile_path);

	debugmsg(1, "opening midi device\n");
	if (open_mididevice(&mididev) == -1)
		errx(1, "could not open midi device");

#ifdef HAVE_PLEDGE
	debugmsg(3, "calling pledge\n");
	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");
#endif

	ret = do_sequencing(midifile, &mididev);

	close_mididevice(&mididev);

	if (fclose(midifile) == EOF)
		warn("could not close midi file");

	debugmsg(1, "lightplay exiting with status code %d\n", ret);

	return ret;
}

void
debugmsg(int msg_level, const char *fmt, ...)
{
	va_list va;

	if (debug_level < msg_level)
		return;

	(void) printf("lightplay debug[%d] :: ", msg_level);

	va_start(va, fmt);
	(void) vprintf(fmt, va);
	va_end(va);
}

void
usage(void)
{
	(void) fprintf(stderr, "Usage: lightplay [-d] midifile\n");
	exit(1);
}

int
open_mididevice(struct mididevice *mididev)
{
	struct mio_hdl *mio;
	struct pollfd *pfd;
	nfds_t nfds;

	if (dry_run)
		return 0;

	if ((mio = mio_open(MIO_PORTANY, MIO_IN|MIO_OUT, 0)) == NULL) {
		warnx("mio_open() error\n");
		return -1;
	}

	nfds = mio_nfds(mio);
	if ((pfd = calloc(nfds, sizeof(struct pollfd))) == NULL) {
		warn("calloc");
		mio_close(mio);
		return -1;
	}

	if (mio_pollfd(mio, pfd, POLLIN) != 1) {
		warnx("unexpected mio_pollfd() return value");
		mio_close(mio);
		return -1;
	}

	mididev->dev = mio;
	mididev->pfd = pfd;
	mididev->nfds = nfds;

	return 0;
}

void
close_mididevice(const struct mididevice *mididev)
{
	if (dry_run)
		return 0;

	mio_close(mididev->dev);
	free(mididev->pfd);
}

int
do_sequencing(FILE *midifile, const struct mididevice *mididev)
{
	struct midievent_buffer me_buffer;
	int ret;
	uint16_t ticks_pqn;

	debugmsg(3, "allocating midi event buffer\n");

	me_buffer.events = calloc(DEFAULT_MIDIEVENTS_SIZE,
	    sizeof(struct midievent));
	if (me_buffer.events == NULL) {
		warn("calloc midibuffer");
		return -1;
	}
	me_buffer.allocated_size = DEFAULT_MIDIEVENTS_SIZE;
	me_buffer.event_count = 0;

	ret = parse_standard_midi_file(midifile, &me_buffer, &ticks_pqn);
	if (ret == -1)
		goto out;

	debugmsg(3, "sorting midi events for playback\n");

	/* sort playback events by position, must be stable sort */
	ret = mergesort(me_buffer.events, me_buffer.event_count,
	   sizeof(struct midievent), compare_midievent_positions);
	if (ret == -1) {
		warn("error in sorting midi event positions");
		goto out;
	}

	ret = playback_midievents(mididev, &me_buffer, ticks_pqn);

out:
	free(me_buffer.events);

	debugmsg(2, "playback done\n");

	return ret;
}

int
compare_midievent_positions(const void *_a, const void *_b)
{
	const struct midievent *a, *b;

	a = _a;
	b = _b;

	return (a->at_ticks < b->at_ticks) ? -1 :
	       (a->at_ticks > b->at_ticks) ?  1 : 0;
}

int
parse_standard_midi_file(FILE *midifile, struct midievent_buffer *me_buffer,
    uint16_t *ticks_pqn)
{
	uint16_t track_count;
	int i, ret;

	debugmsg(2, "starting to parse standard midi file\n");

	if ((ret = parse_smf_header(midifile, &track_count, ticks_pqn)) == -1)
		return ret;

	for (i = 0; i < track_count; i++) {
		ret = parse_next_track(midifile, me_buffer);
		if (ret == -1)
			return ret;
	}

	debugmsg(2, "midi file parse finished\n");

	return ret;
}

int
parse_smf_header(FILE *midifile, uint16_t *track_count, uint16_t *ticks_pqn)
{
	char mthd[5];
	uint32_t hdr_length;
	uint16_t format, _track_count, _ticks_pqn;

	debugmsg(3, "parsing midi file header\n");

	if (fscanf(midifile, "%4s", mthd) != 1) {
		warnx("could not read header, not a standard midi file?");
		return -1;
	}
	if (strcmp(mthd, "MThd") != 0) {
		warnx("midi file header not found, not a standard midi file?");
		return -1;
	}

	if (fread(&hdr_length, sizeof(hdr_length), 1, midifile) != 1) {
		warnx("could not header length");
		return -1;
	}
	hdr_length = ntohl(hdr_length);
	if (hdr_length < 6) {
		warnx("midi header length too short");
		return -1;
	}

	if (fread(&format, sizeof(format), 1, midifile) != 1) {
		warnx("could not read midi file format");
		return -1;
	}
	format = ntohs(format);
	if (format != 1) {
		warnx("only standard midi file format 1 is supported");
		return -1;
	}

	if (fread(&_track_count, sizeof(_track_count), 1, midifile) != 1) {
		warnx("could not read midi track count");
		return -1;
	}
	_track_count = ntohs(_track_count);

	if (fread(&_ticks_pqn, sizeof(_ticks_pqn), 1, midifile) != 1) {
		warnx("could not read tick per quarter note");
		return -1;
	}
	_ticks_pqn = ntohs(_ticks_pqn);
	if (_ticks_pqn & 0x8000) {
		/* XXX might be nice to support this as well */
		warnx("SMPTE-style delta-time units not supported\n");
		return -1;
	}
	if (_ticks_pqn == 0) {
		warnx("ticks per quarter note is zero");
		return -1;
	}

	if (fseek(midifile, hdr_length - 6, SEEK_CUR) == -1) {
		warnx("could not seek over header chunk");
		return -1;
	}

	*track_count = _track_count;
	*ticks_pqn = _ticks_pqn;

	debugmsg(3, "parsed header, expecting %d tracks\n", *track_count);
	debugmsg(3, "ticks per quarter note is %d\n", *ticks_pqn);

	return 0;
}

int
parse_next_track(FILE *midifile, struct midievent_buffer *me_buffer)
{
	char mtrk[5];
	struct midievent midievent;
	struct midievent *new_events;
	uint32_t track_bytes, current_byte;
	int at_ticks, delta_time, track_found, ret;
	uint8_t prev_event_type;

	debugmsg(3, "parsing next midi track\n");

	prev_event_type = 0x00;

	track_found = 0;
	while (!track_found) {
		if (fscanf(midifile, "%4s", mtrk) != 1) {
			warnx("could not read next chunk");
			return -1;
		}
		if (strcmp(mtrk, "MTrk") == 0)
			track_found = 1;
		if (fread(&track_bytes, sizeof(track_bytes), 1, midifile)
		    != 1) {
			warnx("could not read number of bytes in midi chunk");
			return -1;
		}
		track_bytes = ntohl(track_bytes);
		debugmsg(4, "track contains %d bytes\n", track_bytes);

		if (!track_found) {
			debugmsg(4, "skipping non-track chunk\n");
			if (fseek(midifile, track_bytes, SEEK_CUR) == -1) {
				warnx("could not seek over header chunk");
				return -1;
			}
		}
	}

	at_ticks = 0;
	current_byte = 0;

	while (current_byte < track_bytes) {
		ret = get_next_midi_event(midifile, &current_byte, &midievent,
		    &at_ticks, &prev_event_type);
		if (ret == -1)
			return -1;
		if (ret == 0)
			continue;
		assert(ret == 1);

		if (me_buffer->event_count >= me_buffer->allocated_size) {
			debugmsg(4, "reallocating midi event buffer\n");
			if (me_buffer->allocated_size
			    >= SIZE_MAX / sizeof(struct midievent) / 2) {
				warnx("maximum allocated size reached");
				return -1;
			}
			me_buffer->allocated_size *= 2;

			new_events = realloc(me_buffer->events,
			    me_buffer->allocated_size
			    * sizeof(struct midievent));
			if (new_events == NULL) {
				warn("realloc");
				return -1;
			}
			me_buffer->events = new_events;
		}

		me_buffer->events[me_buffer->event_count] = midievent;
		me_buffer->event_count++;
	}

	debugmsg(2, "track parse finished\n");

	return 0;
}

int
get_next_variable_length_quantity(FILE *midifile, uint32_t *current_byte)
{
	uint32_t value;
	uint8_t vlq_byte;
	ssize_t i;

	value = 0;
	for (i = 0; i < 4; i++) {
		if (fread(&vlq_byte, sizeof(uint8_t), 1, midifile) != 1) {
			warnx("could not read next variable length quantity,"
			    " short file?");
			return -1;
		}
		(*current_byte)++;
		value = (value << 7) | (vlq_byte & 0x7f);
		if ((vlq_byte & 0x80) == 0)
			break;
	}

	return value;
}

/*
 * Returns the number of midi events returned, 0 or 1.  Is 0 if next midi
 * event is not an interesting event, and 1 if it is.  Returns -1 in case
 * of error.
 */

int
get_next_midi_event(FILE *midifile, uint32_t *current_byte,
    struct midievent *midievent, int *at_ticks, uint8_t *prev_event_type)
{
	uint8_t raw_midievent[3];
	int delta_time, event_length;
	size_t skip_bytes;

	delta_time = get_next_variable_length_quantity(midifile, current_byte);
	if (delta_time < 0)
		return -1;

	debugmsg(5, "got delta time %d\n", delta_time);

	if (fread(&raw_midievent[0], sizeof(uint8_t), 1, midifile) != 1) {
		warnx("could not read next midi event type, short file?");
		return -1;
	}
	(*current_byte)++;

	if (prev_event_type && !(raw_midievent[0] & 0x80)) {
		debugmsg(4, "using previous event type\n");
		raw_midievent[0] = *prev_event_type;
		if (fseek(midifile, -1, SEEK_CUR) == -1) {
			warnx("could not rewind back one byte in midi file");
			return -1;
		}
		(*current_byte)--;
	}
	*prev_event_type = raw_midievent[0];

	if (raw_midievent[0] == MIDI_META_EVENT)
		return parse_meta_event(midifile, current_byte, midievent,
		    at_ticks, delta_time);

	skip_bytes = 2;

	if (raw_midievent[0] == MIDI_SYSEX_EVENT_F0
	    || raw_midievent[0] == MIDI_SYSEX_EVENT_F7) {
		event_length = get_next_variable_length_quantity(midifile,
		    current_byte);
		if (event_length < 0)
			return -1;
		skip_bytes = event_length;
	} else if ((raw_midievent[0] & 0xf0) == MIDI_PROGRAM_CHANGE
	    || (raw_midievent[0] & 0xf0) == MIDI_CHANNEL_KEY_PRESSURE) {
		skip_bytes = 1;
	}

	if ((raw_midievent[0] & 0xf0) != MIDI_NOTE_OFF
	    && (raw_midievent[0] & 0xf0) != MIDI_NOTE_ON
	    && skip_bytes > 0) {
		debugmsg(4, "skipping midi event %02x\n", raw_midievent[0]);
		if (fseek(midifile, skip_bytes, SEEK_CUR) == -1) {
			warnx("could not skip an uninteresting midi event");
			return -1;
		}
		*current_byte += skip_bytes;
		return 0;
	}

	/* now raw_midievent is either noteoff or noteon event */

	if (fread(&raw_midievent[1], sizeof(uint8_t), 2, midifile) != 2) {
		warnx("could not read next midi event, short file?");
		return -1;
	}
	*current_byte += 2;

	/* XXX should check possibility of int overflow */
	*at_ticks += delta_time;

	midievent->type = MIDIEVENT_CHANNEL_VOICE;
	midievent->at_ticks = *at_ticks;
	midievent->u.raw_midievent[0] = raw_midievent[0];
	midievent->u.raw_midievent[1] = raw_midievent[1];
	midievent->u.raw_midievent[2] = raw_midievent[2];

	debugmsg(3, "parsed midi event %02x %02x %02x at ticks %d\n",
	    raw_midievent[0], raw_midievent[1], raw_midievent[2], *at_ticks);

	return 1;
}

int
parse_meta_event(FILE *midifile, uint32_t *current_byte,
    struct midievent *midievent, int *at_ticks, int delta_time)
{
	uint8_t new_tempo_spec[3];
	int event_length, new_tempo;
	uint8_t event_type;

	if (fread(&event_type, sizeof(uint8_t), 1, midifile) != 1) {
		warnx("could not read next meta event type, short file?");
		return -1;
	}
	(*current_byte)++;

	event_length = get_next_variable_length_quantity(midifile,
	    current_byte);
	if (event_length < 0)
		return -1;

	if (event_type != MIDI_META_SET_TEMPO) {
		debugmsg(4, "skipping midi meta event %02x\n", event_type);
		if (fseek(midifile, event_length, SEEK_CUR) == -1) {
			warnx("could not skip a meta event");
			return -1;
		}
		*current_byte += event_length;
		return 0;
	}

	if (event_length != sizeof(new_tempo_spec)) {
		warnx("set tempo meta event not of expected size");
		return -1;
	}

	if (fread(new_tempo_spec, sizeof(new_tempo_spec), 1, midifile) != 1) {
		warnx("could not read set tempo event value");
		return -1;
	}
	*current_byte += sizeof(new_tempo_spec);

	*at_ticks += delta_time;
	new_tempo = (new_tempo_spec[0] << 16) | (new_tempo_spec[1] << 8)
	    | (new_tempo_spec[2] << 0);

	debugmsg(4, "new set tempo event with tempo %d at ticks %d\n",
	    new_tempo, *at_ticks);

	midievent->type = MIDIEVENT_TEMPO_CHANGE;
	midievent->at_ticks = *at_ticks;
	midievent->u.tempo_in_microseconds_pqn = new_tempo;

	return 1;
}

int
playback_midievents(const struct mididevice *mididev,
    struct midievent_buffer *me_buffer, uint16_t ticks_pqn)
{
	struct midievent me;
	int notes_waiting[MAX_ACTIVE_NOTES];
	size_t i, lighted_keys_index, next_lighted_keys_index;
	int at_ticks_difference, current_at_ticks, next_event_at_ticks;
	int tempo_microseconds_pqn, wait_microseconds;
	int r;

	debugmsg(2, "starting playback\n");

	for (i = 0; i < MAX_ACTIVE_NOTES; i++)
		notes_waiting[i] = 0;

	current_at_ticks = 0;
	lighted_keys_index = 0;
	next_lighted_keys_index = 0;
	tempo_microseconds_pqn = 500000;

	for (i = 0; i < me_buffer->event_count; i++) {
		debugmsg(5, "checking next midi event\n");

		me = me_buffer->events[i];
		next_event_at_ticks = me.at_ticks;

		debugmsg(3, "1. lighted_keys_index=%d i=%d\n", lighted_keys_index,
		    i);
		if (!notes_to_wait_for(notes_waiting)) {
			lighted_keys_index = next_lighted_keys_index;
			/* this increments next_lighted_keys_index */
			turn_on_next_lights(mididev, me_buffer,
			    &next_lighted_keys_index, notes_waiting);
		}

		debugmsg(3, "2. lighted_keys_index=%d i=%d\n", lighted_keys_index,
		    i);
		if (lighted_keys_index <= i) {
			debugmsg(3, "lighted_keys ARE WAITED\n");
			wait_microseconds = -1;
		} else {
			at_ticks_difference
			    = next_event_at_ticks - current_at_ticks;
			wait_microseconds = at_ticks_difference
			    * (tempo_microseconds_pqn / ticks_pqn);
		}

		if (wait_for_event(mididev, wait_microseconds, notes_waiting)
		    == -1) {
			return -1;
		}

		if (me.type == MIDIEVENT_TEMPO_CHANGE) {
			debugmsg(3, "tempo change to %d microseconds pqn\n",
			    tempo_microseconds_pqn);
			tempo_microseconds_pqn
			    = me.u.tempo_in_microseconds_pqn;
		} else if (me.u.raw_midievent[0] != MIDI_NOTE_ON
		    && me.u.raw_midievent[1] != MIDI_NOTE_OFF) {
			/* play events if those are not on channel one */
			debugmsg(3, "playing midi event %02x %02x %02x\n",
			    me.u.raw_midievent[0], me.u.raw_midievent[1],
			    me.u.raw_midievent[2]);
			if (!dry_run) {
				r = mio_write(mididev->dev, me.u.raw_midievent,
				    sizeof(me.u.raw_midievent));
				if (r < sizeof(me.u.raw_midievent)) {
					warnx("mio_write returned an error");
					return -1;
				}
			}
		}

		current_at_ticks = next_event_at_ticks;
	}

	return 0;
}

int
notes_to_wait_for(int *notes_waiting)
{
	int i;

	for (i = 0; i < MAX_ACTIVE_NOTES; i++)
		if (notes_waiting[i])
			return 1;

	return 0;
}

int
turn_on_next_lights(const struct mididevice *mididev,
    struct midievent_buffer *me_buffer, size_t *lighted_keys_index,
    int *notes_waiting)
{
	struct midievent me;
	int next_event_at_ticks, r;
	uint8_t raw_midievent[3];
	uint8_t note;

	debugmsg(3, "turning on lights\n");

	if (*lighted_keys_index >= me_buffer->event_count)
		return 0;

	me = me_buffer->events[ *lighted_keys_index ];
	next_event_at_ticks = me.at_ticks;

	do {
		/* XXX this is wrong, we should test that next_event_at_ticks
		 * XXX is not too big */

		raw_midievent[0] = me.u.raw_midievent[0];
		raw_midievent[1] = me.u.raw_midievent[1];
		raw_midievent[2] = me.u.raw_midievent[2];

		/*
		 * Exact match means the noteon/noteoff events occur on
		 * channel 1.  Manipulate the note velocity to something that
		 * is (hopefully) not going to be heard, because we only want
		 * to show the lights and not the sound.  Note that at least
		 * with Yamaha EZ-220 velocity 0 does not trigger the keyboard
		 * lights, but 1 is enough.
		 */
		if (raw_midievent[0] == MIDI_NOTE_ON) {
			note = raw_midievent[1] & 0x7f;
			debugmsg(3, "turning on light on note %d\n", note);
			raw_midievent[2] = 1;
			if (!dry_run) {
				r = mio_write(mididev->dev, raw_midievent,
				    sizeof(raw_midievent));
				if (r < sizeof(raw_midievent)) {
					warnx("mio_write returned an error");
					return -1;
				}
			}
			notes_waiting[note] = 1;
		}

		if (++(*lighted_keys_index) >= me_buffer->event_count) {
			debugmsg(3, "lighted keys index is in the end\n");
			break;
		}

		me = me_buffer->events[ *lighted_keys_index ];

	} while (me.at_ticks <= next_event_at_ticks);

	debugmsg(3, "done turning on lights\n");

	return 0;
}

int
wait_for_event(const struct mididevice *mididev, int wait_microseconds,
    int *notes_waiting)
{
	size_t i, bytes_to_read;
	int timeout, r, ret;
	struct timespec current_time, nextev_time;
	uint8_t raw_midievent[3];

	if (dry_run)
		return 0;

	bytes_to_read = 3;

	ret = 0;

	if (wait_microseconds >= 0) {
		if (clock_gettime(CLOCK_MONOTONIC, &current_time) == -1) {
			warn("clock_gettime()");
			return -1;
		}

		nextev_time = current_time;
		nextev_time.tv_nsec += 1000 * wait_microseconds;
		while (nextev_time.tv_nsec >= 1000000000) {
			nextev_time.tv_sec += 1;
			nextev_time.tv_nsec -= 1000000000;
		}
	}

	for (;;) {
		if (wait_microseconds >= 0) {
			if (clock_gettime(CLOCK_MONOTONIC, &current_time)
			    == -1) {
				warn("clock_gettime()");
				return -1;
			}
			timeout =
			   1000 * (nextev_time.tv_sec - current_time.tv_sec)
			     + (nextev_time.tv_nsec - current_time.tv_nsec)
				 / 1000000;
			if (timeout < 0)
				timeout = 0;
			debugmsg(3, "waiting user with timeout %d\n", timeout);
		} else {
			debugmsg(3, "waiting user\n");
			timeout = -1;
		}

		if ((r = poll(mididev->pfd, mididev->nfds, timeout)) == -1) {
			warn("poll");
			ret = -1;
			break;
		}

		if (r == 0) {
			/* timeout hits, playback should continue */
			debugmsg(3, "timeout reached, playback continues\n");
			break;
		}

		r = wait_for_notes(mididev, notes_waiting, raw_midievent,
		    &bytes_to_read);
		if (r == -1) {
			ret = -1;
			break;
		}
		if (r == 0)
			break;
	}

	return ret;
}

/*  1 == must wait for more
 *  0 == nothing to wait for
 * -1 == error occurred */

int
wait_for_notes(const struct mididevice *mididev, int *notes_waiting,
   uint8_t *raw_midievent, size_t *bytes_to_read)
{
	size_t i, read_bytes;
	int must_wait, r;
	uint8_t note;

	assert(dry_run == 0);

	if (!notes_to_wait_for(notes_waiting))
		return 0;

	debugmsg(3, "waiting for notes\n");

	read_bytes = mio_read(mididev->dev, &raw_midievent[3-(*bytes_to_read)],
			      *bytes_to_read);
	if (read_bytes == 0) {
		warnx("mio_read error");
		return -1;
	}
	*bytes_to_read -= read_bytes;

	if (*bytes_to_read > 0)
		return 1;

	if ((raw_midievent[0] & 0xf0) != MIDI_NOTE_ON
	    && (raw_midievent[0] & 0xf0) != MIDI_NOTE_OFF) {
		/* XXX We skip events we are not interested in.
		 * XXX But we should probably also understand
		 * XXX something about the possible inputs we
		 * XXX might be receiving. */
		debugmsg(4, "skipping input event %02x\n", raw_midievent[0]);
		raw_midievent[0] = raw_midievent[1];
		raw_midievent[1] = raw_midievent[2];
		*bytes_to_read = 1;
		return 1;
	}

	/* Exact match means the noteon/noteoff events occur
	 * on channel 1. */
	if (raw_midievent[0] == MIDI_NOTE_ON) {
		note = raw_midievent[1] & 0x7f;
		debugmsg(3, "turning note %d off\n", note);
		raw_midievent[0] = MIDI_NOTE_OFF;
		r = mio_write(mididev->dev, raw_midievent, 3);
		if (r < 3) {
			warnx("mio_write returned an error");
			return -1;
		}
		notes_waiting[note] = 0;
	}

	*bytes_to_read = 3;

	if (!notes_to_wait_for(notes_waiting))
		return 0;

	return 1;
}
