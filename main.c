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
#include <sndio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIDI_NOTE_OFF			0x80
#define MIDI_NOTE_ON			0x90
#define MIDI_PROGRAM_CHANGE		0xc0
#define MIDI_CHANNEL_KEY_PRESSURE	0xd0

#define MIDI_SYSEX_EVENT_F0		0xf0
#define MIDI_SYSEX_EVENT_F7		0xf7
#define MIDI_META_EVENT			0xff

#define MIDI_META_SET_TEMPO		0x51

#define DEFAULT_MIDIEVENTS_SIZE 1024

enum midievent_type { MIDIEVENT_CHANNEL_VOICE, MIDIEVENT_TEMPO_CHANGE };

struct midievent {
	enum midievent_type	type;
	int			at_ticks;
	union {
		uint8_t		midievent[3];
		int		tempo_in_microseconds_pqn;
	} u;
};

struct midievent_buffer {
	struct midievent	*events;
	size_t			 allocated_size;
	size_t			 event_count;
};

int	compare_midievent_positions(const void *, const void *);
int	do_sequencing(FILE *, struct mio_hdl *);
int	get_next_variable_length_quantity(FILE *, uint32_t *);
int	get_next_midi_event(FILE *, uint32_t *, struct midievent *, int *);
int	parse_meta_event(FILE *, uint32_t *, struct midievent *, int *, int);
int	parse_smf_header(FILE *, uint16_t *, uint16_t *);
int	parse_standard_midi_file(FILE *, struct midievent_buffer *,
    uint16_t *);
int	parse_next_track(FILE *, struct midievent_buffer *);
int	playback_midievents(struct mio_hdl *, struct midievent_buffer *,
    uint16_t);

int
main(int argc, char *argv[])
{
	FILE *midifile;
	const char *midifile_path;
	struct mio_hdl *mididev;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "Usage: lightplay midifile\n");
		exit(1);
	}
	midifile_path = argv[1];

	if ((midifile = fopen(midifile_path, "r")) == NULL)
		err(1, "could not open midi file \"%s\"", midifile_path);

	if ((mididev = mio_open(MIO_PORTANY, MIO_IN|MIO_OUT, 0)) == NULL)
		errx(1, "could not open midi device");

	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");

	ret = do_sequencing(midifile, mididev);

	mio_close(mididev);

	if (fclose(midifile) == EOF)
		warn("could not close midi file");

	return ret;
}

int
do_sequencing(FILE *midifile, struct mio_hdl *mididev)
{
	struct midievent_buffer me_buffer;
	int ret;
	uint16_t ticks_pqn;

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

	/* sort playback events by position, must be stable sort */
	/* XXX error handling? */
	ret = mergesort(me_buffer.events, me_buffer.event_count,
	   sizeof(struct midievent), compare_midievent_positions);
	if (ret == -1) {
		warn("error in sorting midi event positions");
		goto out;
	}

	ret = playback_midievents(mididev, &me_buffer, ticks_pqn);

out:
	free(me_buffer.events);

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

	if ((ret = parse_smf_header(midifile, &track_count, ticks_pqn)) == -1)
		return ret;

	for (i = 0; i < track_count; i++) {
		ret = parse_next_track(midifile, me_buffer);
		if (ret == -1)
			return ret;
	}

	return ret;
}

int
parse_smf_header(FILE *midifile, uint16_t *track_count, uint16_t *ticks_pqn)
{
	char mthd[5];
	uint32_t hdr_length;
	uint16_t format, _track_count, _ticks_pqn;

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
		if (!track_found) {
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
		    &at_ticks);
		if (ret == -1)
			return -1;
		if (ret == 0)
			continue;
		assert(ret == 1);

		if (me_buffer->event_count >= me_buffer->allocated_size) {
			if (me_buffer->allocated_size >= SIZE_MAX/2) {
				warnx("maximum allocated size reached");
				return -1;
			}
			me_buffer->allocated_size *= 2;

			new_events = reallocarray(me_buffer->events,
			    me_buffer->allocated_size,
			    sizeof(struct midievent));
			if (new_events == NULL) {
				warn("reallocarray");
				return -1;
			}
			me_buffer->events = new_events;
		}

		me_buffer->events[me_buffer->event_count] = midievent;
		me_buffer->event_count++;
	}

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
    struct midievent *midievent, int *at_ticks)
{
	uint8_t raw_midievent[3];
	int delta_time, event_length;
	size_t skip_bytes;

	delta_time = get_next_variable_length_quantity(midifile, current_byte);
	if (delta_time < 0)
		return -1;

	if (fread(&raw_midievent[0], sizeof(uint8_t), 1, midifile) != 1) {
		warnx("could not read next midi event type, short file?");
		return -1;
	}
	(*current_byte)++;

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
	midievent->u.midievent[0] = raw_midievent[0];
	midievent->u.midievent[1] = raw_midievent[1];
	midievent->u.midievent[2] = raw_midievent[2];

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
	(*current_byte) += sizeof(new_tempo_spec);

	*at_ticks += delta_time;
	new_tempo = (new_tempo_spec[0] << 16) | (new_tempo_spec[1] << 8)
	    | (new_tempo_spec[2] << 0);

	midievent->type = MIDIEVENT_TEMPO_CHANGE;
	midievent->at_ticks = *at_ticks;
	midievent->u.tempo_in_microseconds_pqn = new_tempo;

	return 1;
}

int
playback_midievents(struct mio_hdl *mididev,
    struct midievent_buffer *me_buffer, uint16_t ticks_pqn)
{
	struct midievent me;
	struct timespec timeout, remainder;
	int current_at_ticks, next_event_at_ticks, at_ticks_difference;
	int tempo_microseconds_pqn, wait_microseconds;
 	size_t i, r;

	current_at_ticks = 0;
	tempo_microseconds_pqn = 500000;

	for (i = 0; i < me_buffer->event_count; i++) {
		me = me_buffer->events[i];
		next_event_at_ticks = me.at_ticks;
		at_ticks_difference = next_event_at_ticks - current_at_ticks;

		if (at_ticks_difference > 0) {
			wait_microseconds =
			    at_ticks_difference * (tempo_microseconds_pqn / ticks_pqn);
			timeout.tv_sec = wait_microseconds / 1000000;
			timeout.tv_nsec = 1000
			    * (wait_microseconds - 1000000 * timeout.tv_sec);
			if (nanosleep(&timeout, &remainder) == -1) {
				warn("nanosleep");
				/* XXX perhaps should also do something about
				 * XXX the error */
			}
		}

		if (me.type == MIDIEVENT_TEMPO_CHANGE) {
			tempo_microseconds_pqn
			    = me.u.tempo_in_microseconds_pqn;
		} else {
			r = mio_write(mididev, me.u.midievent,
			    sizeof(me.u.midievent));
			if (r < sizeof(me.u.midievent)) {
				warnx("mio_write returned an error");
				return -1;
			}
		}

		current_at_ticks = next_event_at_ticks;
	}

	return 0;
}
