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
#include <err.h>
#include <sndio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int	do_sequencing(FILE *, struct mio_hdl *);
int	parse_smf_header(FILE *, uint16_t *, uint16_t *);
int	parse_standard_midi_file(FILE *);

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
	int ret;

	ret = parse_standard_midi_file(midifile);

	return ret;
}

int
parse_standard_midi_file(FILE *midifile)
{
	uint16_t track_count, ticks_pqn;
	int r;

	if ((r = parse_smf_header(midifile, &track_count, &ticks_pqn)) != 0)
		return r;

	return 0;
}

int
parse_smf_header(FILE *midifile, uint16_t *track_count,
    uint16_t *ticks_pqn)
{
	char mthd[5];
	uint32_t six;
	uint16_t format, _track_count, _ticks_pqn;

	if (fscanf(midifile, "%4s", mthd) != 1) {
		warnx("could not read header, not a standard midi file?");
		return 1;
	}
	if (strcmp(mthd, "MThd") != 0) {
		warnx("track header not found, not a standard midi file?");
		return 1;
	}

	if (fread(&six, sizeof(six), 1, midifile) != 1) {
		warnx("could not read number six from header");
		return 1;
	}
	six = ntohl(six);
	if (six != 6) {
		warnx("invalid midi track header");
		return 1;
	}

	if (fread(&format, sizeof(format), 1, midifile) != 1) {
		warnx("could not read midi file format");
		return 1;
	}
	format = ntohs(format);
	if (format != 1) {
		warnx("only standard midi file format 1 is supported");
		return 1;
	}

	if (fread(&_track_count, sizeof(_track_count), 1, midifile) != 1) {
		warnx("could not read midi track count");
		return 1;
	}
	_track_count = ntohs(_track_count);

	if (fread(&_ticks_pqn, sizeof(_ticks_pqn), 1, midifile) != 1) {
		warnx("could not read tick per quarter note");
		return 1;
	}
	_ticks_pqn = ntohs(_ticks_pqn);

	*track_count = _track_count;
	*ticks_pqn = _ticks_pqn;

	return 0;
}
