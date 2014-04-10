/*
	MUS2MIDI: DMX (DOOM) MUS to MIDI Library

	Copyright (C) 2014  Bret Curtis

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Library General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Library General Public License for more details.

	You should have received a copy of the GNU Library General Public
	License along with this library; if not, write to the
	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
	Boston, MA  02110-1301, USA.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mus.h"
#include "wm_error.h"

#define DST_CHUNK 					8192
#define TEMPO						0x001aa309
#define MUSEVENT_KEYOFF				0
#define MUSEVENT_KEYON				1
#define MUSEVENT_PITCHWHEEL			2
#define MUSEVENT_CHANNELMODE		3
#define MUSEVENT_CONTROLLERCHANGE	4
#define MUSEVENT_END				6
#define MIDI_MAXCHANNELS			16
#define MIDIHEADERSIZE				14


uint8_t midimap[] =
{//	MIDI	Number	Description
	0,		//0		// program change
	0,		//1		// bank selection
	0x01,	//2		// Modulation pot (frequency vibrato depth)
	0x07,	//3		// Volume: 0-silent, ~100-normal, 127-loud
	0x0A,	//4		// Pan (balance) pot: 0-left, 64-center (default), 127-right
	0x0B,	//5		// Expression pot
	0x5B,	//6		// Reverb depth
	0x5D,	//7		// Chorus depth
	0x40,	//8		// Sustain pedal
	0x43,	//9		// Soft pedal
	0x78,	//10	// All sounds off
	0x7B,	//11	// All notes off
	0x7E,	//12	// Mono (use numchannels + 1)
	0x7F,	//13	// Poly
	0x79,	//14	// reset all controllers
};

typedef struct MUSheader {
	char		ID[4];          // identifier "MUS" 0x1A
	uint16_t    scoreLen;
	uint16_t    scoreStart;
	uint16_t    channels;		// count of primary channels
	uint16_t    sec_channels;	// count of secondary channels
	uint16_t    instrCnt;
	uint16_t    dummy;
	/*  variable-length part starts here
	uint16_t	instruments[];
	*/
} MUSheader ;

typedef struct MidiHeaderChunk {
	char name[4];
	int32_t	length;
	int16_t	format;		// make 0
	int16_t	ntracks;	// make 1
	int16_t	division;	// 0xe250??
} MidiHeaderChunk;

typedef struct MidiTrackChunk {
	char name[4];
	int32_t	length;
} MidiTrackChunk;

struct mus_ctx {
	uint8_t *src, *src_ptr;
	uint32_t srcsize;
	uint32_t datastart;
	uint8_t *dst, *dst_ptr;
	uint32_t dstsize, dstrem;
};

static void resize_dst(struct mus_ctx *ctx) {
	uint32_t pos = ctx->dst_ptr - ctx->dst;
	ctx->dst = realloc(ctx->dst, ctx->dstsize + DST_CHUNK);
	ctx->dstsize += DST_CHUNK;
	ctx->dstrem += DST_CHUNK;
	ctx->dst_ptr = ctx->dst + pos;
}

static void write1(struct mus_ctx *ctx, uint32_t val)
{
	if (ctx->dstrem < 1)
		resize_dst(ctx);
	*ctx->dst_ptr++ = val & 0xff;
	ctx->dstrem--;
}

static void write2(struct mus_ctx *ctx, uint32_t val)
{
	if (ctx->dstrem < 2)
		resize_dst(ctx);
	*ctx->dst_ptr++ = (val>>8) & 0xff;
	*ctx->dst_ptr++ = val & 0xff;
	ctx->dstrem -= 2;
}

static void write4(struct mus_ctx *ctx, uint32_t val)
{
	if (ctx->dstrem < 4)
		resize_dst(ctx);
	*ctx->dst_ptr++ = (val>>24)&0xff;
	*ctx->dst_ptr++ = (val>>16)&0xff;
	*ctx->dst_ptr++ = (val>>8) & 0xff;
	*ctx->dst_ptr++ = val & 0xff;
	ctx->dstrem -= 4;
}

static void seekdst(struct mus_ctx *ctx, uint32_t pos) {
	ctx->dst_ptr = ctx->dst + pos;
	while (ctx->dstsize < pos)
		resize_dst(ctx);
	ctx->dstrem = ctx->dstsize - pos;
}

static void skipdst(struct mus_ctx *ctx, int32_t pos) {
	size_t newpos;
	ctx->dst_ptr += pos;
	newpos = ctx->dst_ptr - ctx->dst;
	while (ctx->dstsize < newpos)
		resize_dst(ctx);
	ctx->dstrem = ctx->dstsize - newpos;
}

static uint32_t getdstpos(struct mus_ctx *ctx) {
	return (ctx->dst_ptr - ctx->dst);
}

uint8_t *mus_getmididata(struct mus_ctx *ctx){
	return ctx->dst;
}

uint32_t mus_getmidisize(struct mus_ctx *ctx){
	return ctx->dstsize - ctx->dstrem;
}

void mus_free(struct mus_ctx *ctx){
	if (!ctx) return;
	free(ctx->dst);
	free(ctx);
}

/* writes a variable length integer to a buffer, and returns bytes written */
int WriteVarLen( long value, uint8_t* out )
{
	long buffer, count = 0;

	buffer = value & 0x7f;
	while ((value >>= 7) > 0) {
		buffer <<= 8;
		buffer += 0x80;
		buffer += (value & 0x7f);
	}

	while (1) {
		++count;
		*out = (uint8_t)buffer;
		++out;
		if (buffer & 0x80)
			buffer >>= 8;
		else
			break;
	}
	return count;
}


struct mus_ctx *mus2midi(uint8_t *data, uint32_t size){
	struct mus_ctx *ctx;
	ctx = calloc(1, sizeof(struct mus_ctx));
	ctx->src = ctx->src_ptr = data;
	ctx->srcsize = size;

	ctx->dst = calloc(DST_CHUNK, sizeof(uint8_t));
	ctx->dst_ptr = ctx->dst;
	ctx->dstsize = DST_CHUNK;
	ctx->dstrem = DST_CHUNK;

	MUSheader header;
	unsigned char* cur = data,* end;

	uint32_t track_size_pos, begin_track_pos, current_pos;

	// Delta time for midi event
	int delta_time = 0;
	int temp;
	int channel_volume[MIDI_MAXCHANNELS] = {0};
	int channelMap[MIDI_MAXCHANNELS], currentChannel = 0;

	/* read the MUS header and set our location */
	memcpy(&header, ctx->src_ptr, sizeof(header));
	ctx->src_ptr += sizeof(header);

	// TODO: data is stored in little-endian, do we need to convert?

	// we only support 15 channels
	if (header.channels > MIDI_MAXCHANNELS - 1) return NULL;

	// Map channel 15 to 9(percussions)
	for (temp = 0; temp < MIDI_MAXCHANNELS; ++temp) {
		channelMap[temp] = -1;
		channel_volume[temp] = 0x40;
	}
	channelMap[15] = 9;

	/* Header is 14 bytes long and add the rest as well */
	write1(ctx, 'M');
	write1(ctx, 'T');
	write1(ctx, 'h');
	write1(ctx, 'd');
	write4(ctx, 6);			// length of header
	write2(ctx, 0);			// MIDI type (always 0)
	write2(ctx, 1);			// MUS files only have 1 track
	write2(ctx, 0x0059);	// devision

	/* Write out track header and track length position for later */
	begin_track_pos = getdstpos(ctx);
	write1(ctx, 'M');
	write1(ctx, 'T');
	write1(ctx, 'r');
	write1(ctx, 'k');
	track_size_pos = getdstpos(ctx);
	skipdst(ctx, 4);

	/* write tempo: microseconds per quarter note */
	write1(ctx, 0x00);		// delta time
	write1(ctx, 0xff);		// sys command
	write2(ctx, 0x5103); 	// command - set tempo
	write1(ctx, TEMPO & 0x000000ff);
	write1(ctx, (TEMPO & 0x0000ff00) >> 8);
	write1(ctx, (TEMPO & 0x00ff0000) >> 16);

	/* Percussions channel starts out at full volume */
	write1(ctx, 0x00);
	write1(ctx, 0xB9);
	write1(ctx, 0x07);
	write1(ctx, 127);

	/* get current position in source, and end of position */
	cur = data + header.scoreStart;
	end = cur + header.scoreLen;

	// main loop
	while(cur < end){
		//printf("LOOP DEBUG: %d\r\n",iterator++);
		uint8_t channel;
		uint8_t event;
		uint8_t temp_buffer[32];	// temp buffer for current iterator
		uint8_t *out_local = temp_buffer;
		uint8_t status, bit1, bit2, bitc = 2;

		/* read in current bit */
		event		= *cur++;
		channel		= (event & 15);		// current channel

		/* write variable length delta time */
		out_local += WriteVarLen(delta_time, out_local);

		/* set all channels to 127 (max) volume */
		if (channelMap[channel] < 0) {
			*out_local++ = 0xB0 + currentChannel;
			*out_local++ = 0x07;
			*out_local++ = 127;
			*out_local++ = 0x00;
			channelMap[channel] = currentChannel++;
			if (currentChannel == 9)
				++currentChannel;
		}
		status = channelMap[channel];

		/* handle events */
		switch ((event & 122) >> 4){
			default:
				// we shouldn't be here...
				break;
			case MUSEVENT_KEYOFF:
				status |=  0x80;
				bit1 = *cur++;
				bit2 = 0x40;
				break;
			case MUSEVENT_KEYON:
				status |= 0x90;
				bit1 = *cur & 127;
				if (*cur++ & 128)	// volume bit?
					channel_volume[channelMap[channel]] = *cur++;
				bit2 = channel_volume[channelMap[channel]];
				break;
			case MUSEVENT_PITCHWHEEL:
				status |= 0xE0;
				bit1 = (*cur & 1) >> 6;
				bit2 = (*cur++ >> 1) & 127;
				break;
			case MUSEVENT_CHANNELMODE:
				status |= 0xB0;
				if (! (*cur < sizeof(midimap) / sizeof(midimap[0])))
					printf("Not good.\r\n");
				bit1 = midimap[*cur++];
				bit2 = (*cur++ == 12) ? header.channels + 1 : 0x00;
				break;
			case MUSEVENT_CONTROLLERCHANGE:
				if (*cur == 0) {
					cur++;
					status |= 0xC0;
					bit1 = *cur++;
					bitc = 1;
				} else {
					status |= 0xB0;
					if (! (*cur < sizeof(midimap) / sizeof(midimap[0])))
						printf("Not good.\r\n");
					bit1 = midimap[*cur++];
					bit2 = *cur++;
				}
				break;
			case 5:	// Unknown
				// we shouldn't be here
				break;
			case MUSEVENT_END:	// End
				status = 0xff;
				bit1 = 0x2f;
				bit2 = 0x00;
				if (! (cur == end))
					printf("Not good.'r'n");
				break;
			case 7:	// Unknown
				// we shouldn't be here
				break;
		}

		/* write it out */
		*out_local++ = status;
		*out_local++ = bit1;
		if (bitc == 2)
			*out_local++ = bit2;

		/* write out our temp buffer */
		if (out_local != temp_buffer)
		{
			if (ctx->dstrem < sizeof(temp_buffer)*32)
				resize_dst(ctx);

			memcpy(ctx->dst_ptr, temp_buffer, out_local - temp_buffer);
			ctx->dst_ptr += out_local - temp_buffer;
			ctx->dstsize += out_local - temp_buffer;
			ctx->dstrem -= out_local - temp_buffer;

		}

		if (event & 128) {
			delta_time = 0;
			do {
				delta_time = delta_time * 128 + (*cur & 127);
			} while ((*cur++ & 128));
		} else {
			delta_time = 0;
		}


	}

	/* write out track length */
	current_pos = getdstpos(ctx);
	seekdst(ctx, track_size_pos);
	write4(ctx, current_pos - begin_track_pos - sizeof(MidiTrackChunk)); // when track begins
	seekdst(ctx, current_pos);

	/* correct our midi size */
	ctx->dstsize = ctx->dst_ptr - ctx->dst;
	//ctx->dstrem = 0;

	FILE* file = fopen("/tmp/test.mid", "wb");
	fwrite(ctx->dst, ctx->dstsize, 1, file);
	fclose(file);


	return ctx;
}
