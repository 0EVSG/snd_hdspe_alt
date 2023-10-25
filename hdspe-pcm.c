/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2012-2021 Ruslan Bukin <br@bsdpad.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * RME HDSPe driver for FreeBSD (pcm-part).
 * Supported cards: AIO, RayDAT.
 */

#include <dev/sound/pcm/sound.h>
#include <hdspe.h>
#include <dev/sound/chip.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <mixer_if.h>

SND_DECLARE_FILE("$FreeBSD$");

struct hdspe_latency {
	uint32_t n;
	uint32_t period;
	float ms;
};

static struct hdspe_latency latency_map[] = {
	{ 7,   32, 0.7 },
	{ 0,   64, 1.5 },
	{ 1,  128,   3 },
	{ 2,  256,   6 },
	{ 3,  512,  12 },
	{ 4, 1024,  23 },
	{ 5, 2048,  46 },
	{ 6, 4096,  93 },

	{ 0,    0,   0 },
};

struct hdspe_rate {
	uint32_t speed;
	uint32_t reg;
};

static struct hdspe_rate rate_map[] = {
	{  32000, (HDSPE_FREQ_32000) },
	{  44100, (HDSPE_FREQ_44100) },
	{  48000, (HDSPE_FREQ_48000) },
	{  64000, (HDSPE_FREQ_32000 | HDSPE_FREQ_DOUBLE) },
	{  88200, (HDSPE_FREQ_44100 | HDSPE_FREQ_DOUBLE) },
	{  96000, (HDSPE_FREQ_48000 | HDSPE_FREQ_DOUBLE) },
	{ 128000, (HDSPE_FREQ_32000 | HDSPE_FREQ_QUAD)   },
	{ 176400, (HDSPE_FREQ_44100 | HDSPE_FREQ_QUAD)   },
	{ 192000, (HDSPE_FREQ_48000 | HDSPE_FREQ_QUAD)   },

	{ 0, 0 },
};

static unsigned int
hdspe_adat_width(uint32_t speed)
{
	if (speed > 96000)
		return 2;
	if (speed > 48000)
		return 4;
	return 8;
}

static unsigned int
hdspe_channel_slot_base(unsigned int adat_width, uint32_t port)
{
	/* AIO ports */
	if (port & HDSPE_CHAN_AIO_LINE)
		return (0);
	if (port & HDSPE_CHAN_AIO_PHONE)
		return (6);
	if (port & HDSPE_CHAN_AIO_AES)
		return (8);
	if (port & HDSPE_CHAN_AIO_SPDIF)
		return (10);
	if (port & HDSPE_CHAN_AIO_ADAT)
		return (12);

	/* RayDAT ports */
	if (port & HDSPE_CHAN_RAY_AES)
		return (0);
	if (port & HDSPE_CHAN_RAY_SPDIF)
		return (2);
	if (port & HDSPE_CHAN_RAY_ADAT1)
		return (4);
	if (port & HDSPE_CHAN_RAY_ADAT2)
		return (4 + adat_width);
	if (port & HDSPE_CHAN_RAY_ADAT3)
		return (4 + 2 * adat_width);
	if (port & HDSPE_CHAN_RAY_ADAT4)
		return (4 + 3 * adat_width);

	return (0);
}

static unsigned int
hdspe_channel_slot_width(unsigned int adat_width, uint32_t port)
{
	unsigned int slots = 0;

	/* AIO ports */
	if (port & HDSPE_CHAN_AIO_LINE)
		return (2);		/* Non-adjacent to next DMA slot. */
	if (port & HDSPE_CHAN_AIO_PHONE)
		slots += 2;
	if (port & HDSPE_CHAN_AIO_AES)
		slots += 2;
	if (port & HDSPE_CHAN_AIO_SPDIF)
		slots += 2;
	if (port & HDSPE_CHAN_AIO_ADAT)
		slots += adat_width;
	if (slots > 0)
		return slots;

	/* RayDAT ports */
	if (port & HDSPE_CHAN_RAY_AES)
		slots += 2;
	if (port & HDSPE_CHAN_RAY_SPDIF)
		slots += 2;
	if (port & HDSPE_CHAN_RAY_ADAT1)
		slots += adat_width;
	if (port & HDSPE_CHAN_RAY_ADAT2)
		slots += adat_width;
	if (port & HDSPE_CHAN_RAY_ADAT3)
		slots += adat_width;
	if (port & HDSPE_CHAN_RAY_ADAT4)
		slots += adat_width;

	return slots;
}


static unsigned int
hdspe_channel_count(uint32_t adat_width, uint32_t port)
{
	unsigned int count = 0;

	/* AIO ports. */
	if (port & HDSPE_CHAN_AIO_LINE)
		count += 2;
	if (port & HDSPE_CHAN_AIO_PHONE)
		count += 2;	/* Phones have no recording channel. */
	if (port & HDSPE_CHAN_AIO_AES)
		count += 2;
	if (port & HDSPE_CHAN_AIO_SPDIF)
		count += 2;
	if (port & HDSPE_CHAN_AIO_ADAT)
		count += adat_width;
	if (count > 0)
		return count;	/* Do not mix with RayDAT ports. */

	/* RayDAT ports. */
	if (port & HDSPE_CHAN_RAY_AES)
		count += 2;
	if (port & HDSPE_CHAN_RAY_SPDIF)
		count += 2;
	if (port & HDSPE_CHAN_RAY_ADAT1)
		count += adat_width;
	if (port & HDSPE_CHAN_RAY_ADAT2)
		count += adat_width;
	if (port & HDSPE_CHAN_RAY_ADAT3)
		count += adat_width;
	if (port & HDSPE_CHAN_RAY_ADAT4)
		count += adat_width;

	return count;
}

static int
hdspe_hw_mixer(struct sc_chinfo *ch, unsigned int dst,
    unsigned int src, unsigned short data)
{
	struct sc_pcminfo *scp;
	struct sc_info *sc;
	int offs;

	scp = ch->parent;
	sc = scp->sc;

	offs = 0;
	if (ch->dir == PCMDIR_PLAY)
		offs = 64;

	hdspe_write_4(sc, HDSPE_MIXER_BASE +
	    ((offs + src + 128 * dst) * sizeof(uint32_t)),
	    data & 0xFFFF);

	return (0);
};

static int
hdspechan_setgain(struct sc_chinfo *ch)
{
	struct sc_info *sc;
	unsigned int slot, end_slot;

	sc = ch->parent->sc;

	slot = hdspe_channel_slot_base(hdspe_adat_width(sc->speed), ch->ports);
	end_slot = slot +
	    hdspe_channel_slot_width(hdspe_adat_width(sc->speed), ch->ports);

	/* Treat first slot as left channel. */
	if (slot < end_slot) {
		hdspe_hw_mixer(ch, slot, slot, ch->lvol * HDSPE_MAX_GAIN / 100);
		slot++;
	}
	/* Right channel, subsequent slots all get the same volume. */
	for (; slot < end_slot; slot++) {
		hdspe_hw_mixer(ch, slot, slot, ch->rvol * HDSPE_MAX_GAIN / 100);
	}

	return (0);
}

static int
hdspemixer_init(struct snd_mixer *m)
{
	struct sc_pcminfo *scp;
	struct sc_info *sc;
	int mask;

	scp = mix_getdevinfo(m);
	sc = scp->sc;
	if (sc == NULL)
		return (-1);

	mask = SOUND_MASK_PCM;

	if (scp->hc->play)
		mask |= SOUND_MASK_VOLUME;

	if (scp->hc->rec)
		mask |= SOUND_MASK_RECLEV;

	snd_mtxlock(sc->lock);
	pcm_setflags(scp->dev, pcm_getflags(scp->dev) | SD_F_SOFTPCMVOL);
	mix_setdevs(m, mask);
	snd_mtxunlock(sc->lock);

	return (0);
}

static int
hdspemixer_set(struct snd_mixer *m, unsigned dev,
    unsigned left, unsigned right)
{
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	int i;

	scp = mix_getdevinfo(m);

#if 0
	device_printf(scp->dev, "hdspemixer_set() %d %d\n",
	    left, right);
#endif

	for (i = 0; i < scp->chnum; i++) {
		ch = &scp->chan[i];
		if ((dev == SOUND_MIXER_VOLUME && ch->dir == PCMDIR_PLAY) ||
		    (dev == SOUND_MIXER_RECLEV && ch->dir == PCMDIR_REC)) {
			ch->lvol = left;
			ch->rvol = right;
			if (ch->run)
				hdspechan_setgain(ch);
		}
	}

	return (0);
}

static kobj_method_t hdspemixer_methods[] = {
	KOBJMETHOD(mixer_init,      hdspemixer_init),
	KOBJMETHOD(mixer_set,       hdspemixer_set),
	KOBJMETHOD_END
};
MIXER_DECLARE(hdspemixer);

static void
hdspechan_enable(struct sc_chinfo *ch, int value)
{
	struct sc_pcminfo *scp;
	struct sc_info *sc;
	int reg;
	unsigned int slot, end_slot;

	scp = ch->parent;
	sc = scp->sc;

	if (ch->dir == PCMDIR_PLAY)
		reg = HDSPE_OUT_ENABLE_BASE;
	else
		reg = HDSPE_IN_ENABLE_BASE;

	ch->run = value;

	slot = hdspe_channel_slot_base(hdspe_adat_width(sc->speed), ch->ports);
	end_slot = slot +
	    hdspe_channel_slot_width(hdspe_adat_width(sc->speed), ch->ports);

	for (; slot < end_slot; slot++) {
		hdspe_write_1(sc, reg + (4 * slot), value);
	}
}

static int
hdspe_running(struct sc_info *sc)
{
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	device_t *devlist;
	int devcount;
	int i, j;
	int err;

	if ((err = device_get_children(sc->dev, &devlist, &devcount)) != 0)
		goto bad;

	for (i = 0; i < devcount; i++) {
		scp = device_get_ivars(devlist[i]);
		for (j = 0; j < scp->chnum; j++) {
			ch = &scp->chan[j];
			if (ch->run)
				goto bad;
		}
	}

	free(devlist, M_TEMP);

	return (0);
bad:

#if 0
	device_printf(sc->dev, "hdspe is running\n");
#endif

	free(devlist, M_TEMP);

	return (1);
}

static void
hdspe_start_audio(struct sc_info *sc)
{

	sc->ctrl_register |= (HDSPE_AUDIO_INT_ENABLE | HDSPE_ENABLE);
	hdspe_write_4(sc, HDSPE_CONTROL_REG, sc->ctrl_register);
}

static void
hdspe_stop_audio(struct sc_info *sc)
{

	if (hdspe_running(sc) == 1)
		return;

	sc->ctrl_register &= ~(HDSPE_AUDIO_INT_ENABLE | HDSPE_ENABLE);
	hdspe_write_4(sc, HDSPE_CONTROL_REG, sc->ctrl_register);
}

static void
buffer_mux_write(uint32_t *dma, uint32_t *pcm, unsigned int pos,
    unsigned int samples, unsigned int slots, unsigned int channels)
{
	int slot;

	for (; samples > 0; samples--) {
		for (slot = 0; slot < slots; slot++) {
			dma[slot * HDSPE_CHANBUF_SAMPLES + pos] =
			    pcm[pos * channels + slot];
		}
		pos = (pos + 1) % HDSPE_CHANBUF_SAMPLES;
	}
}

static void
buffer_mux_port(uint32_t *dma, uint32_t *pcm, uint32_t port, uint32_t ports,
    unsigned int pos, unsigned int samples, unsigned int adat_width)
{
	unsigned int slot, slot_width;
	unsigned int channels, chan_pos = 0;

	slot = hdspe_channel_slot_base(adat_width, port);
	slot_width = hdspe_channel_slot_width(adat_width, port);
	channels = hdspe_channel_count(adat_width, ports);

	dma += slot * HDSPE_CHANBUF_SAMPLES;

	pcm += chan_pos;

	buffer_mux_write(dma, pcm, pos, samples, slot_width, channels);
}

static void
buffer_demux_read(uint32_t *dma, uint32_t *pcm, unsigned int pos,
    unsigned int samples, unsigned int slots, unsigned int channels)
{
	int slot;

	for (; samples > 0; samples--) {
		for (slot = 0; slot < slots; slot++) {
			pcm[pos * channels + slot] =
			    dma[slot * HDSPE_CHANBUF_SAMPLES + pos];
		}
		pos = (pos + 1) % HDSPE_CHANBUF_SAMPLES;
	}
}

static void
buffer_demux_port(uint32_t *dma, uint32_t *pcm, uint32_t port, uint32_t ports,
    unsigned int pos, unsigned int samples, unsigned int adat_width)
{
	unsigned int slot, slot_width;
	unsigned int channels, chan_pos = 0;

	slot = hdspe_channel_slot_base(adat_width, port);
	slot_width = hdspe_channel_slot_width(adat_width, port);
	channels = hdspe_channel_count(adat_width, ports);

	dma += slot * HDSPE_CHANBUF_SAMPLES;

	pcm += chan_pos;

	buffer_demux_read(dma, pcm, pos, samples, slot_width, channels);
}


/* Multiplex / demultiplex: 2.0 <-> 2 x 1.0. */
static void
buffer_copy(struct sc_chinfo *ch)
{
	struct sc_pcminfo *scp;
	struct sc_info *sc;
	unsigned int src, dst;
	unsigned int n;
	unsigned int adat_width;

	scp = ch->parent;
	sc = scp->sc;

	n = AFMT_CHANNEL(ch->format); /* n channels */

	/* We only have to copy the minimum ADAT width from speed and format. */
	adat_width = hdspe_adat_width(sc->speed);
	if (adat_width > 2 && n == hdspe_channel_count(2, ch->ports))
		adat_width = 2;
	else if (adat_width > 4 && n == hdspe_channel_count(4, ch->ports))
		adat_width = 4;

	if (ch->dir == PCMDIR_PLAY) {
		src = sndbuf_getreadyptr(ch->buffer);
	} else {
		src = sndbuf_getfreeptr(ch->buffer);
	}

	src /= 4; /* Bytes per sample. */
	dst = src / n; /* Destination buffer n-times smaller. */

	if (ch->dir == PCMDIR_PLAY) {
		buffer_mux_port(sc->pbuf, ch->data, ch->ports, ch->ports, dst,
		    sc->period * 2, adat_width);
	} else {
		buffer_demux_port(sc->rbuf, ch->data, ch->ports, ch->ports, dst,
		    sc->period * 2, adat_width);
	}
}

static int
clean(struct sc_chinfo *ch)
{
	struct sc_pcminfo *scp;
	struct sc_info *sc;
	uint32_t *buf;
	unsigned int slot, end_slot;

	scp = ch->parent;
	sc = scp->sc;
	buf = sc->rbuf;

	if (ch->dir == PCMDIR_PLAY) {
		buf = sc->pbuf;
	}

	slot = hdspe_channel_slot_base(hdspe_adat_width(sc->speed), ch->ports);
	end_slot = slot +
	    hdspe_channel_slot_width(hdspe_adat_width(sc->speed), ch->ports);

	for (; slot < end_slot; slot++) {
		bzero(buf + HDSPE_CHANBUF_SAMPLES * slot, HDSPE_CHANBUF_SIZE);
	}

	return (0);
}

/* Channel interface. */
static void *
hdspechan_init(kobj_t obj, void *devinfo, struct snd_dbuf *b,
    struct pcm_channel *c, int dir)
{
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	struct sc_info *sc;
	int num;

	scp = devinfo;
	sc = scp->sc;

	snd_mtxlock(sc->lock);
	num = scp->chnum;

	ch = &scp->chan[num];
	/* device_printf(scp->dev, "Slot base %d width %d vs left slot %d.",
	    hdspe_channel_slot_base(8, scp->hc->ports),
	    hdspe_channel_slot_width(8, scp->hc->ports), scp->hc->left); */
	ch->ports = scp->hc->ports;
	ch->run = 0;
	ch->lvol = 0;
	ch->rvol = 0;

	ch->cap_fmts[0] =
	    SND_FORMAT(AFMT_S32_LE, hdspe_channel_count(2, ch->ports), 0);
	ch->cap_fmts[1] =
	    SND_FORMAT(AFMT_S32_LE, hdspe_channel_count(4, ch->ports), 0);
	ch->cap_fmts[2] =
	    SND_FORMAT(AFMT_S32_LE, hdspe_channel_count(8, ch->ports), 0);
	ch->cap_fmts[3] = 0;
	ch->caps = malloc(sizeof(struct pcmchan_caps), M_HDSPE, M_NOWAIT);
	*(ch->caps) = (struct pcmchan_caps) {32000, 192000, ch->cap_fmts, 0};

	/* Allocate maximum buffer size. */
	ch->size = HDSPE_CHANBUF_SIZE * hdspe_channel_count(8, ch->ports);
	ch->data = malloc(ch->size, M_HDSPE, M_NOWAIT);

	ch->buffer = b;
	ch->channel = c;
	ch->parent = scp;

	ch->dir = dir;

	snd_mtxunlock(sc->lock);

	if (sndbuf_setup(ch->buffer, ch->data, ch->size) != 0) {
		device_printf(scp->dev, "Can't setup sndbuf.\n");
		return (NULL);
	}

	return (ch);
}

static int
hdspechan_trigger(kobj_t obj, void *data, int go)
{
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	struct sc_info *sc;

	ch = data;
	scp = ch->parent;
	sc = scp->sc;

	snd_mtxlock(sc->lock);
	switch (go) {
	case PCMTRIG_START:
#if 0
		device_printf(scp->dev, "hdspechan_trigger(): start\n");
#endif
		hdspechan_enable(ch, 1);
		hdspechan_setgain(ch);
		hdspe_start_audio(sc);
		break;

	case PCMTRIG_STOP:
	case PCMTRIG_ABORT:
#if 0
		device_printf(scp->dev, "hdspechan_trigger(): stop or abort\n");
#endif
		clean(ch);
		hdspechan_enable(ch, 0);
		hdspe_stop_audio(sc);
		break;

	case PCMTRIG_EMLDMAWR:
	case PCMTRIG_EMLDMARD:
		if(ch->run)
			buffer_copy(ch);
		break;
	}

	snd_mtxunlock(sc->lock);

	return (0);
}

static uint32_t
hdspechan_getptr(kobj_t obj, void *data)
{
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	struct sc_info *sc;
	uint32_t ret, pos;

	ch = data;
	scp = ch->parent;
	sc = scp->sc;

	snd_mtxlock(sc->lock);
	ret = hdspe_read_2(sc, HDSPE_STATUS_REG);
	snd_mtxunlock(sc->lock);

	pos = ret & HDSPE_BUF_POSITION_MASK;
	pos *= AFMT_CHANNEL(ch->format); /* Hardbuf with multiple channels. */

	return (pos);
}

static int
hdspechan_free(kobj_t obj, void *data)
{
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	struct sc_info *sc;

	ch = data;
	scp = ch->parent;
	sc = scp->sc;

#if 0
	device_printf(scp->dev, "hdspechan_free()\n");
#endif

	snd_mtxlock(sc->lock);
	if (ch->data != NULL) {
		free(ch->data, M_HDSPE);
		ch->data = NULL;
	}
	if (ch->caps != NULL) {
		free(ch->caps, M_HDSPE);
		ch->caps = NULL;
	}
	snd_mtxunlock(sc->lock);

	return (0);
}

static int
hdspechan_setformat(kobj_t obj, void *data, uint32_t format)
{
	struct sc_chinfo *ch;

	ch = data;

#if 0
	struct sc_pcminfo *scp = ch->parent;
	device_printf(scp->dev, "hdspechan_setformat(%d)\n", format);
#endif

	ch->format = format;

	return (0);
}

static uint32_t
hdspechan_setspeed(kobj_t obj, void *data, uint32_t speed)
{
	struct sc_pcminfo *scp;
	struct hdspe_rate *hr;
	struct sc_chinfo *ch;
	struct sc_info *sc;
	long long period;
	int threshold;
	int i;

	ch = data;
	scp = ch->parent;
	sc = scp->sc;
	hr = NULL;

#if 0
	device_printf(scp->dev, "hdspechan_setspeed(%d)\n", speed);
#endif

	if (hdspe_running(sc) == 1)
		goto end;

	/* First look for equal frequency. */
	for (i = 0; rate_map[i].speed != 0; i++) {
		if (rate_map[i].speed == speed)
			hr = &rate_map[i];
	}

	/* If no match, just find nearest. */
	if (hr == NULL) {
		for (i = 0; rate_map[i].speed != 0; i++) {
			hr = &rate_map[i];
			threshold = hr->speed + ((rate_map[i + 1].speed != 0) ?
			    ((rate_map[i + 1].speed - hr->speed) >> 1) : 0);
			if (speed < threshold)
				break;
		}
	}

	switch (sc->type) {
	case HDSPE_RAYDAT:
	case HDSPE_AIO:
		period = HDSPE_FREQ_AIO;
		break;
	default:
		/* Unsupported card. */
		goto end;
	}

	/* Write frequency on the device. */
	sc->ctrl_register &= ~HDSPE_FREQ_MASK;
	sc->ctrl_register |= hr->reg;
	hdspe_write_4(sc, HDSPE_CONTROL_REG, sc->ctrl_register);

	speed = hr->speed;
	if (speed > 96000)
		speed /= 4;
	else if (speed > 48000)
		speed /= 2;

	/* Set DDS value. */
	period /= speed;
	hdspe_write_4(sc, HDSPE_FREQ_REG, period);

	sc->speed = hr->speed;
end:

	return (sc->speed);
}

static uint32_t
hdspechan_setblocksize(kobj_t obj, void *data, uint32_t blocksize)
{
	struct hdspe_latency *hl;
	struct sc_pcminfo *scp;
	struct sc_chinfo *ch;
	struct sc_info *sc;
	int threshold;
	int i;

	ch = data;
	scp = ch->parent;
	sc = scp->sc;
	hl = NULL;

#if 0
	device_printf(scp->dev, "hdspechan_setblocksize(%d)\n", blocksize);
#endif

	if (hdspe_running(sc) == 1)
		goto end;

	if (blocksize > HDSPE_LAT_BYTES_MAX)
		blocksize = HDSPE_LAT_BYTES_MAX;
	else if (blocksize < HDSPE_LAT_BYTES_MIN)
		blocksize = HDSPE_LAT_BYTES_MIN;

	blocksize /= 4 /* samples */;

	/* First look for equal latency. */
	for (i = 0; latency_map[i].period != 0; i++) {
		if (latency_map[i].period == blocksize) {
			hl = &latency_map[i];
		}
	}

	/* If no match, just find nearest. */
	if (hl == NULL) {
		for (i = 0; latency_map[i].period != 0; i++) {
			hl = &latency_map[i];
			threshold = hl->period + ((latency_map[i + 1].period != 0) ?
			    ((latency_map[i + 1].period - hl->period) >> 1) : 0);
			if (blocksize < threshold)
				break;
		}
	}

	snd_mtxlock(sc->lock);
	sc->ctrl_register &= ~HDSPE_LAT_MASK;
	sc->ctrl_register |= hdspe_encode_latency(hl->n);
	hdspe_write_4(sc, HDSPE_CONTROL_REG, sc->ctrl_register);
	sc->period = hl->period;
	snd_mtxunlock(sc->lock);

#if 0
	device_printf(scp->dev, "New period=%d\n", sc->period);
#endif

	sndbuf_resize(ch->buffer,
	    (HDSPE_CHANBUF_SIZE * AFMT_CHANNEL(ch->format)) / (sc->period * 4),
	    (sc->period * 4));
end:

	return (sndbuf_getblksz(ch->buffer));
}

static uint32_t hdspe_bkp_fmt[] = {
	SND_FORMAT(AFMT_S32_LE, 2, 0),
	0
};

static struct pcmchan_caps hdspe_bkp_caps = {32000, 192000, hdspe_bkp_fmt, 0};

static struct pcmchan_caps *
hdspechan_getcaps(kobj_t obj, void *data)
{
	struct sc_chinfo *ch;

	ch = data;

#if 0
	struct sc_pcminfo *scl = ch->parent;
	device_printf(scp->dev, "hdspechan_getcaps()\n");
#endif

	if (ch->caps != NULL)
		return (ch->caps);

	return (&hdspe_bkp_caps);
}

static kobj_method_t hdspechan_methods[] = {
	KOBJMETHOD(channel_init,         hdspechan_init),
	KOBJMETHOD(channel_free,         hdspechan_free),
	KOBJMETHOD(channel_setformat,    hdspechan_setformat),
	KOBJMETHOD(channel_setspeed,     hdspechan_setspeed),
	KOBJMETHOD(channel_setblocksize, hdspechan_setblocksize),
	KOBJMETHOD(channel_trigger,      hdspechan_trigger),
	KOBJMETHOD(channel_getptr,       hdspechan_getptr),
	KOBJMETHOD(channel_getcaps,      hdspechan_getcaps),
	KOBJMETHOD_END
};
CHANNEL_DECLARE(hdspechan);

static int
hdspe_pcm_probe(device_t dev)
{

#if 0
	device_printf(dev,"hdspe_pcm_probe()\n");
#endif

	return (0);
}

static uint32_t
hdspe_pcm_intr(struct sc_pcminfo *scp)
{
	struct sc_chinfo *ch;
	struct sc_info *sc;
	int i;

	sc = scp->sc;

	for (i = 0; i < scp->chnum; i++) {
		ch = &scp->chan[i];
		snd_mtxunlock(sc->lock);
		chn_intr(ch->channel);
		snd_mtxlock(sc->lock);
	}

	return (0);
}

static int
hdspe_pcm_attach(device_t dev)
{
	char status[SND_STATUSLEN];
	struct sc_pcminfo *scp;
	char desc[64];
	int i, err;

	scp = device_get_ivars(dev);
	scp->ih = &hdspe_pcm_intr;

	bzero(desc, sizeof(desc));
	if (scp->hc->ports & HDSPE_CHAN_AIO_ALL)
		snprintf(desc, sizeof(desc), "HDSPe AIO [%s]",
		    scp->hc->descr);
	else if (scp->hc->ports & HDSPE_CHAN_RAY_ALL)
		snprintf(desc, sizeof(desc), "HDSPe RayDAT [%s]",
		    scp->hc->descr);
	else
		snprintf(desc, sizeof(desc), "HDSPe ? [%s]", scp->hc->descr);
	device_set_desc_copy(dev, desc);

	/*
	 * We don't register interrupt handler with snd_setup_intr
	 * in pcm device. Mark pcm device as MPSAFE manually.
	 */
	pcm_setflags(dev, pcm_getflags(dev) | SD_F_MPSAFE);

	err = pcm_register(dev, scp, scp->hc->play, scp->hc->rec);
	if (err) {
		device_printf(dev, "Can't register pcm.\n");
		return (ENXIO);
	}

	scp->chnum = 0;
	for (i = 0; i < scp->hc->play; i++) {
		pcm_addchan(dev, PCMDIR_PLAY, &hdspechan_class, scp);
		scp->chnum++;
	}

	for (i = 0; i < scp->hc->rec; i++) {
		pcm_addchan(dev, PCMDIR_REC, &hdspechan_class, scp);
		scp->chnum++;
	}

	snprintf(status, SND_STATUSLEN, "at io 0x%jx irq %jd %s",
	    rman_get_start(scp->sc->cs),
	    rman_get_start(scp->sc->irq),
	    PCM_KLDSTRING(snd_hdspe));
	pcm_setstatus(dev, status);

	mixer_init(dev, &hdspemixer_class, scp);

	return (0);
}

static int
hdspe_pcm_detach(device_t dev)
{
	int err;

	err = pcm_unregister(dev);
	if (err) {
		device_printf(dev, "Can't unregister device.\n");
		return (err);
	}

	return (0);
}

static device_method_t hdspe_pcm_methods[] = {
	DEVMETHOD(device_probe,     hdspe_pcm_probe),
	DEVMETHOD(device_attach,    hdspe_pcm_attach),
	DEVMETHOD(device_detach,    hdspe_pcm_detach),
	{ 0, 0 }
};

static driver_t hdspe_pcm_driver = {
	"pcm",
	hdspe_pcm_methods,
	PCM_SOFTC_SIZE,
};

DRIVER_MODULE(snd_hdspe_pcm, hdspe, hdspe_pcm_driver, 0, 0);
MODULE_DEPEND(snd_hdspe, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
MODULE_VERSION(snd_hdspe, 1);
