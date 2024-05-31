/*
 * SPDIF output driver using the I2C interface and software encoding
 * Copyright (C) 2015, 2023 Stephan "Kiffie" <kiffie.vanhaash@gmail.com>
 * 2024 Matti Metsälä
 *
 * Based on
 *      ALSA SoC I2S Audio Layer for Broadcom BCM2708 SoC
 *
 *      Copyright (c) Florian Meier <florian.meier@koalo.de>
 *
 *	Raspberry Pi PCM I2S ALSA Driver
 *	Copyright (c) by Phil Poole 2013
 *
 *	ALSA SoC I2S (McBSP) Audio Layer for TI DAVINCI processor
 *      Vladimir Barinov, <vbarinov@embeddedalley.com>
 *	Copyright (C) 2007 MontaVista Software, Inc., <source@mvista.com>
 *
 *	OMAP ALSA SoC DAI driver using McBSP port
 *	Copyright (C) 2008 Nokia Corporation
 *	Contact: Jarkko Nikula <jarkko.nikula@bitmer.com>
 *		 Peter Ujfalusi <peter.ujfalusi@ti.com>
 *
 *	Freescale SSI ALSA SoC Digital Audio Interface (DAI) driver
 *	Author: Timur Tabi <timur@freescale.com>
 *	Copyright 2007-2010 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "spdif-encoder.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>

/* Clock registers */
#define BCM2708_CLK_PCMCTL_REG  0x00
#define BCM2708_CLK_PCMDIV_REG  0x04

/* Clock register settings */
#define BCM2708_CLK_PASSWD		(0x5a000000)
#define BCM2708_CLK_PASSWD_MASK	(0xff000000)
#define BCM2708_CLK_MASH(v)		((v) << 9)
#define BCM2708_CLK_FLIP		BIT(8)
#define BCM2708_CLK_BUSY		BIT(7)
#define BCM2708_CLK_KILL		BIT(5)
#define BCM2708_CLK_ENAB		BIT(4)
#define BCM2708_CLK_SRC(v)		(v)

#define BCM2708_CLK_SHIFT		(12)
#define BCM2708_CLK_DIVI(v)		((v) << BCM2708_CLK_SHIFT)
#define BCM2708_CLK_DIVF(v)		(v)
#define BCM2708_CLK_DIVF_MASK		(0xFFF)

enum {
	BCM2708_CLK_MASH_0 = 0,
	BCM2708_CLK_MASH_1,
	BCM2708_CLK_MASH_2,
	BCM2708_CLK_MASH_3,
};

enum {
	BCM2708_CLK_SRC_GND = 0,
	BCM2708_CLK_SRC_OSC,
	BCM2708_CLK_SRC_DBG0,
	BCM2708_CLK_SRC_DBG1,
	BCM2708_CLK_SRC_PLLA,
	BCM2708_CLK_SRC_PLLC,
	BCM2708_CLK_SRC_PLLD,
	BCM2708_CLK_SRC_HDMI,
};

/* Most clocks are not useable (freq = 0) */
static const unsigned int bcm2708_clk_freq[BCM2708_CLK_SRC_HDMI+1] = {
	[BCM2708_CLK_SRC_GND]		= 0,
	[BCM2708_CLK_SRC_OSC]		= 19200000,
	[BCM2708_CLK_SRC_DBG0]		= 0,
	[BCM2708_CLK_SRC_DBG1]		= 0,
	[BCM2708_CLK_SRC_PLLA]		= 0,
	[BCM2708_CLK_SRC_PLLC]		= 0,
	[BCM2708_CLK_SRC_PLLD]		= 500000000,
	[BCM2708_CLK_SRC_HDMI]		= 0,
};

/* I2S registers */
#define BCM2708_I2S_CS_A_REG		0x00
#define BCM2708_I2S_FIFO_A_REG		0x04
#define BCM2708_I2S_MODE_A_REG		0x08
#define BCM2708_I2S_RXC_A_REG		0x0c
#define BCM2708_I2S_TXC_A_REG		0x10
#define BCM2708_I2S_DREQ_A_REG		0x14
#define BCM2708_I2S_INTEN_A_REG		0x18
#define BCM2708_I2S_INTSTC_A_REG	0x1c
#define BCM2708_I2S_GRAY_REG		0x20

/* I2S register settings */
#define BCM2708_I2S_STBY		BIT(25)
#define BCM2708_I2S_SYNC		BIT(24)
#define BCM2708_I2S_RXSEX		BIT(23)
#define BCM2708_I2S_RXF			BIT(22)
#define BCM2708_I2S_TXE			BIT(21)
#define BCM2708_I2S_RXD			BIT(20)
#define BCM2708_I2S_TXD			BIT(19)
#define BCM2708_I2S_RXR			BIT(18)
#define BCM2708_I2S_TXW			BIT(17)
#define BCM2708_I2S_CS_RXERR		BIT(16)
#define BCM2708_I2S_CS_TXERR		BIT(15)
#define BCM2708_I2S_RXSYNC		BIT(14)
#define BCM2708_I2S_TXSYNC		BIT(13)
#define BCM2708_I2S_DMAEN		BIT(9)
#define BCM2708_I2S_RXTHR(v)		((v) << 7)
#define BCM2708_I2S_TXTHR(v)		((v) << 5)
#define BCM2708_I2S_RXCLR		BIT(4)
#define BCM2708_I2S_TXCLR		BIT(3)
#define BCM2708_I2S_TXON		BIT(2)
#define BCM2708_I2S_RXON		BIT(1)
#define BCM2708_I2S_EN			(1)

#define BCM2708_I2S_CLKDIS		BIT(28)
#define BCM2708_I2S_PDMN		BIT(27)
#define BCM2708_I2S_PDME		BIT(26)
#define BCM2708_I2S_FRXP		BIT(25)
#define BCM2708_I2S_FTXP		BIT(24)
#define BCM2708_I2S_CLKM		BIT(23)
#define BCM2708_I2S_CLKI		BIT(22)
#define BCM2708_I2S_FSM			BIT(21)
#define BCM2708_I2S_FSI			BIT(20)
#define BCM2708_I2S_FLEN(v)		((v) << 10)
#define BCM2708_I2S_FSLEN(v)		(v)

#define BCM2708_I2S_CHWEX		BIT(15)
#define BCM2708_I2S_CHEN		BIT(14)
#define BCM2708_I2S_CHPOS(v)		((v) << 4)
#define BCM2708_I2S_CHWID(v)		(v)
#define BCM2708_I2S_CH1(v)		((v) << 16)
#define BCM2708_I2S_CH2(v)		(v)

#define BCM2708_I2S_TX_PANIC(v)		((v) << 24)
#define BCM2708_I2S_RX_PANIC(v)		((v) << 16)
#define BCM2708_I2S_TX(v)		((v) << 8)
#define BCM2708_I2S_RX(v)		(v)

#define BCM2708_I2S_INT_RXERR		BIT(3)
#define BCM2708_I2S_INT_TXERR		BIT(2)
#define BCM2708_I2S_INT_RXR		BIT(1)
#define BCM2708_I2S_INT_TXW		BIT(0)

/* logging and debugging */
#define dprintk(mask, fmt, arg...) do { \
if (debug & mask) \
	printk(KERN_ERR "bcm2708-i2s-spdif: " fmt, ## arg); \
} while (0)

#define DBG_INIT 0x1
#define DBG_IRQ  0x2
#define DBG_ALSA 0x4

// static unsigned int debug=DBG_INIT|DBG_IRQ|DBG_ALSA;
static unsigned int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "debug mask (0: no debug messages)");

/* General device struct */

#define SPDIF_BUFSIZE_FRAMES	(2*192)	/* buffer size in SPDIF frames */
#define SPDIF_BUFSIZE			(SPDIF_BUFSIZE_FRAMES*SPDIF_FRAMESIZE)
#define PCM_PERIODES			8
/* PCM period size must be divisible by 192*4 (S16_LE), 192*6 (S24_3LE) and 192*8 (S24_LE) */
#define PCM_BUFSIZE				(PCM_PERIODES*192*24)	/* PCM buffer size */

typedef void (*spdif_encode_func)(struct spdif_encoder *, void *, const void *);

struct bcm2708_i2s_dev {
	spinlock_t lock;

	struct device *dev;
	unsigned int fmt;
	//unsigned int bclk_ratio;

	struct regmap *i2s_regmap;
	struct clk *clk;

	struct dma_chan *i2s_dma;
	dma_cookie_t i2s_dma_cookie;

	uint8_t *spdif_buffer; /* size in bytes: SPDIF_BUFSIZE */
	dma_addr_t spdif_buffer_handle; /* bus address of spdif_buffer */

	snd_pcm_uframes_t pcm_pointer;

	struct spdif_encoder spdif;

	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *ss; /* current substream or NULL */

	int period_frames;
	spdif_encode_func encode_frame;
	atomic_t silence;
};

static void bcm_2708_i2s_init_clock(struct bcm2708_i2s_dev *dev,
				    unsigned bclk_rate)
{
	if (clk_set_rate(dev->clk, bclk_rate) != 0)
		dev_err(dev->dev, "cannot set clock rate to %u\n", bclk_rate);
	if (clk_prepare_enable(dev->clk) != 0)
		dev_err(dev->dev, "cannot enable clock\n");
}

/*
 * ALSA related functions
 */

static struct snd_device_ops bcm2708_i2s_alsa_device_ops = { NULL };
static struct snd_pcm_hardware bcm2708_i2s_pcm_hw = {
        .info             = SNDRV_PCM_INFO_MMAP |
                            SNDRV_PCM_INFO_INTERLEAVED |
                            SNDRV_PCM_INFO_BLOCK_TRANSFER,
        .formats          = SNDRV_PCM_FMTBIT_S16_LE |
                            SNDRV_PCM_FMTBIT_S20_LE | SNDRV_PCM_FMTBIT_S20_3LE |
                            SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE |
                            SNDRV_PCM_FMTBIT_S32_LE,
        // .subformats       = SNDRV_PCM_SUBFMTBIT_STD |
        //                     SNDRV_PCM_SUBFMTBIT_MSBITS_MAX |
        //                     SNDRV_PCM_SUBFMTBIT_MSBITS_20 |
        //                     SNDRV_PCM_SUBFMTBIT_MSBITS_24,
        .rates            = SNDRV_PCM_RATE_44100  | SNDRV_PCM_RATE_48000 |
                            SNDRV_PCM_RATE_88200  | SNDRV_PCM_RATE_96000 |
                            SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
        .rate_min         = 44100,
        .rate_max         = 192000,
        .channels_min     = 2,
        .channels_max     = 2,
        .buffer_bytes_max = PCM_BUFSIZE,
        .period_bytes_min = PCM_BUFSIZE/PCM_PERIODES,
        .period_bytes_max = PCM_BUFSIZE/PCM_PERIODES,
        .periods_min      = PCM_PERIODES,
        .periods_max      = PCM_PERIODES,
};

static int bcm2708_i2s_dmaengine_prepare_and_submit(struct bcm2708_i2s_dev *dev);

static int bcm2708_pcm_open(struct snd_pcm_substream *ss)
{
	struct bcm2708_i2s_dev *dev = ss->pcm->private_data;
	ss->private_data = dev;
	dprintk(DBG_ALSA, "dev=%p\n", dev);
	ss->runtime->hw = bcm2708_i2s_pcm_hw;
	dprintk(DBG_ALSA, "pcm_open\n");
	dev->ss = ss;
	return 0;
}

static int bcm2708_pcm_close(struct snd_pcm_substream *ss)
{
	struct bcm2708_i2s_dev *dev = ss->pcm->private_data;
	ss->private_data = NULL;
	dev->ss = NULL;
	return 0;
}

static int bcm2708_hw_params(struct snd_pcm_substream *ss,
			     struct snd_pcm_hw_params *hw_params)
{
	struct bcm2708_i2s_dev *dev = ss->pcm->private_data;
	uint32_t sample_mask = SPDIF_SAMPLE_MASK;
	int buffer_bytes, res;
	dprintk(DBG_ALSA, "hw_params start ss=%p, hw_params=%p\n", ss, hw_params);
	dprintk(DBG_ALSA, "msbits: %u\n", hw_params->msbits);
	if (hw_params->msbits < 24) {
		sample_mask <<= (24 - hw_params->msbits);
		sample_mask &= SPDIF_SAMPLE_MASK;
	}
	dev_info(dev->dev, "Sample mask: 0x%08x\n", sample_mask);
	spdif_encoder_set_sample_mask(&dev->spdif, sample_mask);
	buffer_bytes = params_buffer_bytes(hw_params);
	dprintk(DBG_ALSA, "buffer size in frames: %d\n", params_buffer_size(hw_params) );
	dprintk(DBG_ALSA, "buffer size in bytes: %d\n", buffer_bytes);
	res = snd_pcm_lib_malloc_pages(ss, buffer_bytes);
	dprintk(DBG_ALSA, "snd_pcm_lib_malloc_pages: res=%d\n", res);
	dprintk(DBG_INIT, "alsa buf. base/size = %p/%d, int. buf base = %p\n",
		ss->runtime->dma_buffer_p->area,
		ss->runtime->dma_buffer_p->bytes,
		dev->spdif_buffer);
	dprintk(DBG_ALSA, "hw_params end\n");
	return res;
}

#define CASE_RATE(n) \
	case n: \
		ch_stat[3] = SPDIF_CS3_##n; \
		break;

static int bcm2708_pcm_prepare(struct snd_pcm_substream *ss)
{
	int silence;
	uint8_t ch_stat[] = { SPDIF_CS0_NOT_COPYRIGHT,
			      SPDIF_CS1_DDCONV | SPDIF_CS1_ORIGINAL,
			      0,
			      0,
			      SPDIF_CS4_WORDLEN_UNSPEC };
	struct bcm2708_i2s_dev *dev = snd_pcm_substream_chip(ss);
	dprintk(DBG_ALSA, "pcm_prepare start ss=%p\n", ss);
	dprintk(DBG_ALSA, "buffer size in frames: %ld\n", ss->runtime->buffer_size);
	dprintk(DBG_ALSA, "period size in frames: %ld\n", ss->runtime->period_size);
	dprintk(DBG_ALSA, "number of periods    : %d\n",  ss->runtime->periods);
	dprintk(DBG_ALSA, "rate                 : %d\n",  ss->runtime->rate);
	dprintk(DBG_ALSA, "format               : %d\n",  ss->runtime->format);
	dprintk(DBG_ALSA, "frame-bits           : %u\n",  ss->runtime->frame_bits);
	dprintk(DBG_ALSA, "sample-bits          : %u\n",  ss->runtime->sample_bits);
	switch (ss->runtime->rate) {
	CASE_RATE(44100)
	CASE_RATE(48000)
	CASE_RATE(88200)
	CASE_RATE(96000)
	CASE_RATE(176400)
	CASE_RATE(192000)
	default:
		dev_err(dev->dev, "prepare: invalid sampling rate: %u\n", ss->runtime->rate);
		return -EINVAL;
	}
	switch (ss->runtime->format) {
		case SNDRV_PCM_FORMAT_S16_LE:
			dev->encode_frame = spdif_encode_frame_s16le;
			break;
		case SNDRV_PCM_FORMAT_S20_LE:
		case SNDRV_PCM_FORMAT_S24_LE:
			dev->encode_frame = spdif_encode_frame_s24le;
			break;
		case SNDRV_PCM_FORMAT_S20_3LE:
		case SNDRV_PCM_FORMAT_S24_3LE:
			dev->encode_frame = spdif_encode_frame_s24le_packed;
			break;
		case SNDRV_PCM_FORMAT_S32_LE:
			dev->encode_frame = spdif_encode_frame_s32le;
			break;
		default:
			dev_err(dev->dev, "%s: invalid format: %u\n", __func__, ss->runtime->format);
			return -EINVAL;
	}
	switch (ss->runtime->sample_bits) {
		case 16:
			ch_stat[4] = SPDIF_CS4_WORDLEN_20_16;
			break;
		case 20:
			ch_stat[4] = SPDIF_CS4_WORDLEN_24_20;
			break;
		case 24:
		case 32:
			ch_stat[4] = SPDIF_CS4_MAX_WORDLEN_24 | SPDIF_CS4_WORDLEN_24_20;
			break;
	}
	spdif_encoder_set_channel_status(&dev->spdif, ch_stat, sizeof(ch_stat));
	bcm_2708_i2s_init_clock(dev, 128 * ss->runtime->rate);
	silence = atomic_cmpxchg(&dev->silence, 0, 1);
	if (silence != 0) {
		dprintk(DBG_ALSA, "silence-count          : %d\n", silence);
	} else {
		dev_info(dev->dev, "Prepare %u-bit %u Hz\n", ss->runtime->sample_bits, ss->runtime->rate);
	}
	bcm2708_i2s_dmaengine_prepare_and_submit(dev);
	return 0;
}

static int bcm2708_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct bcm2708_i2s_dev *dev = snd_pcm_substream_chip(ss);
	int ret = 0;
	int silence;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		dprintk(DBG_ALSA, "SNDRV_PCM_TRIGGER_START\n");
		dev->pcm_pointer = 0;
		dev->period_frames = 0;
		silence = atomic_xchg(&dev->silence, 0);
		if (silence > 1) {
			dev_info(dev->dev, "Start: %d frames silenced\n", (silence+1) * SPDIF_BUFSIZE_FRAMES/2);
		} else {
			dev_info(dev->dev, "Start\n");
		}
		bcm2708_i2s_dmaengine_prepare_and_submit(dev);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		dprintk(DBG_ALSA, "SNDRV_PCM_TRIGGER_STOP\n");
		dev_info(dev->dev, "Stop\n");
		dmaengine_terminate_all(dev->i2s_dma);
		dev->i2s_dma_cookie = 0;
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static snd_pcm_uframes_t bcm2708_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct bcm2708_i2s_dev *dev= ss->pcm->private_data;
	return dev->pcm_pointer;
}

static struct snd_pcm_ops bcm2708_i2s_pcm_ops = {
        .open      = bcm2708_pcm_open,
        .close     = bcm2708_pcm_close,
        .ioctl     = snd_pcm_lib_ioctl,
        .hw_params = bcm2708_hw_params,
        .hw_free   = snd_pcm_lib_free_pages,
        .prepare   = bcm2708_pcm_prepare,
        .trigger   = bcm2708_pcm_trigger,
        .pointer   = bcm2708_pcm_pointer,
};

/*
 * I2S interface
 */

static void bcm2708_i2s_dma_complete(void *arg)
{
	struct bcm2708_i2s_dev *dev = arg;
	struct dma_tx_state state;
	int offset;
	uint8_t *dst;
	int i;

	if (!dev->encode_frame) {
		return;
	}
	dmaengine_tx_status(dev->i2s_dma, dev->i2s_dma_cookie, &state);

	/* index of part of double buffer to fill */
	offset = state.residue <= SPDIF_BUFSIZE/2 ? 0 : SPDIF_BUFSIZE/2;
	dst = dev->spdif_buffer + offset;

	if (atomic_inc_not_zero(&dev->silence)) {
		const uint32_t zero[2] = {0, 0};
		for (i = 0; i < SPDIF_BUFSIZE_FRAMES/2; i++) {
			dev->encode_frame(&dev->spdif, dst, zero);
			dst += SPDIF_FRAMESIZE;
		}
	} else if (dev->ss) {
		ssize_t frame_bytes = frames_to_bytes(dev->ss->runtime, 1);
		uint8_t *src = dev->ss->dma_buffer.area;
		src += frames_to_bytes(dev->ss->runtime, dev->pcm_pointer);
		for (i =0 ; i < SPDIF_BUFSIZE_FRAMES/2; i++) {
			dev->encode_frame(&dev->spdif, dst, src);
			src += frame_bytes;
			dst += SPDIF_FRAMESIZE;
		}
		dev->pcm_pointer += SPDIF_BUFSIZE_FRAMES/2;
		if( dev->pcm_pointer >= dev->ss->runtime->buffer_size ){
			dev->pcm_pointer -= dev->ss->runtime->buffer_size;
		}

		dev->period_frames += SPDIF_BUFSIZE_FRAMES/2;
		if (dev->period_frames >= dev->ss->runtime->period_size) {
			dev->period_frames -= dev->ss->runtime->period_size;
			snd_pcm_period_elapsed(dev->ss);
		}
	}
}

static int bcm2708_i2s_dmaengine_prepare_and_submit(struct bcm2708_i2s_dev *dev)
{
	struct dma_async_tx_descriptor *desc;

	if (dev->i2s_dma_cookie > 0) {
		return 0;
	} else if (dev->encode_frame) {
		// Fill with silence
		const uint32_t zero[2] = {0, 0};
		uint8_t* dst = dev->spdif_buffer;
		for (int i = 0; i < SPDIF_BUFSIZE_FRAMES; i++) {
			dev->encode_frame(&dev->spdif, dst, zero);
			dst += SPDIF_FRAMESIZE;
		}
	}

	desc = dmaengine_prep_dma_cyclic(dev->i2s_dma,
			dev->spdif_buffer_handle,
			SPDIF_BUFSIZE,
			SPDIF_BUFSIZE/2,
			DMA_MEM_TO_DEV,
			DMA_CTRL_ACK|DMA_PREP_INTERRUPT);

	if (!desc)
		return -ENOMEM;

	desc->callback = bcm2708_i2s_dma_complete;
	desc->callback_param = dev;
	dev->i2s_dma_cookie = dmaengine_submit(desc);
	dma_async_issue_pending(dev->i2s_dma);
	return 0;
}

static bool bcm2708_i2s_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case BCM2708_I2S_CS_A_REG:
	case BCM2708_I2S_FIFO_A_REG:
	case BCM2708_I2S_INTSTC_A_REG:
	case BCM2708_I2S_GRAY_REG:
		return true;
	default:
		return false;
	};
}

static bool bcm2708_i2s_precious_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case BCM2708_I2S_FIFO_A_REG:
		return true;
	default:
		return false;
	};
}

static const struct regmap_config bcm2708_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = BCM2708_I2S_GRAY_REG,
	.precious_reg = bcm2708_i2s_precious_reg,
	.volatile_reg = bcm2708_i2s_volatile_reg,
	.cache_type = REGCACHE_RBTREE,
};

static int bcm2708_i2s_probe(struct platform_device *pdev)
{
	struct bcm2708_i2s_dev *dev;
	int ret;
	void __iomem *base;
	unsigned syncval, csreg, txcreg;
	int timeout;
	dma_cap_mask_t mask;
	struct dma_slave_config slave_config;
	const __be32 *addr;
	dma_addr_t dma_base;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev),
			   GFP_KERNEL);
	if (dev == NULL)
		return -ENOMEM;

	/* Store the pdev */
	dev->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, dev);

	/* get the clock */
	dev->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(dev->clk)) {
		ret =  dev_err_probe(&pdev->dev, PTR_ERR(dev->clk),
				     "could not get clk\n");
		goto out_devm_kzalloc;
	}
	/* Request ioarea */
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto out_devm_kzalloc;
	}
	dev->i2s_regmap = devm_regmap_init_mmio(&pdev->dev, base,
				&bcm2708_regmap_config);
	if (IS_ERR(dev->i2s_regmap)) {
		ret =  PTR_ERR(dev->i2s_regmap);
		goto out_devm_kzalloc;
	}
	spin_lock_init(&dev->lock);
	dev->spdif_buffer = dma_alloc_coherent(
		dev->dev,
		SPDIF_FRAMESIZE*SPDIF_BUFSIZE_FRAMES,
		&dev->spdif_buffer_handle,
		GFP_KERNEL);
	if (dev->spdif_buffer == NULL) {
		dev_err(&pdev->dev, "cannot allocate DMA memory.\n");
		ret = -ENOMEM;
		goto out_devm_kzalloc;
	}

	spdif_encoder_init(&dev->spdif);

	/* get the DMA address from the DT */
	addr = of_get_address(pdev->dev.of_node, 0, NULL, NULL);
	if (!addr) {
		dev_err(&pdev->dev, "could not get DMA-register address\n");
		ret = -EINVAL;
		goto out_dma_alloc;
	}
	dma_base = be32_to_cpup(addr);

	/* get and configure the DMA channel */
	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_cap_set(DMA_CYCLIC, mask);
	dev->i2s_dma = dma_request_slave_channel_compat(mask, NULL, NULL, &pdev->dev, "tx");
	if (dev->i2s_dma == NULL) {
		dev_err(&pdev->dev,
		        "Could not request DMA channel. "
                        "Check if bcm2708_dmaengine.ko is loaded");
		ret= -ENODEV;
		goto out_dma_alloc;
	}

	slave_config.direction= DMA_MEM_TO_DEV;
	slave_config.src_addr= dev->spdif_buffer_handle;
	slave_config.dst_addr= dma_base + BCM2708_I2S_FIFO_A_REG;
	slave_config.src_addr_width= DMA_SLAVE_BUSWIDTH_4_BYTES;
	slave_config.dst_addr_width= DMA_SLAVE_BUSWIDTH_4_BYTES;
	slave_config.src_maxburst= 2;
	slave_config.dst_maxburst= 2;
	slave_config.src_port_window_size = 0;
	slave_config.dst_port_window_size = 0;
	slave_config.device_fc = 0;
	slave_config.peripheral_config = NULL;
	slave_config.peripheral_size = 0;
	ret= dmaengine_slave_config(dev->i2s_dma, &slave_config);
	if( ret < 0 ){
		dev_err(&pdev->dev,
		        "could not configure DMA channel: %d.\n", ret);
		goto out_dma_alloc;
	}

	/*
	 * register ALSA driver
	*/
	ret= snd_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1, "RpiSpdif",
			  THIS_MODULE, 0, &dev->card);
	if( ret<0 ){
		dev_err(&pdev->dev, "could not create ALSA card: %d\n", ret);
		goto out_dma_alloc;
	}
	strcpy(dev->card->driver, "rpi_spdif_drv");
	strcpy(dev->card->shortname, "RPI I2S SPDIF");
	strcpy(dev->card->longname, "Raspberry Pi I2S SPDIF Card");
	snd_card_set_dev(dev->card, dev->dev);
	ret= snd_device_new(dev->card, SNDRV_DEV_LOWLEVEL,
			    dev, &bcm2708_i2s_alsa_device_ops);
	if( ret<0 ){
		dev_err(&pdev->dev, "could not create ALSA device: %d\n", ret);
		goto out_card_create;
	}
	ret= snd_pcm_new(dev->card, dev->card->driver, 0, 1, 0, &dev->pcm);
	if( ret <0 ){
		dev_err(&pdev->dev, "could not create ALSA PCM:%d\n", ret);
		goto out_card_create;
	}
	dev->pcm->private_data= dev;
	strcpy(dev->pcm->name, "spdif");
	snd_pcm_set_ops(dev->pcm,
			SNDRV_PCM_STREAM_PLAYBACK,
			&bcm2708_i2s_pcm_ops);

	snd_pcm_lib_preallocate_pages_for_all(
		dev->pcm,
		SNDRV_DMA_TYPE_CONTINUOUS,
		NULL,
		PCM_BUFSIZE, PCM_BUFSIZE);
	ret = snd_card_register(dev->card);
	if( ret<0 ){
		dev_err(&pdev->dev, "could not register ALSA card:%d\n", ret);
		goto out_card_create;
	}

	/*
	 * configure the I2S interface
	 */
	/* disable RX/TX */
	regmap_write(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, 0);
	/* Setup the DMA parameters */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_RXTHR(1)
			| BCM2708_I2S_TXTHR(1)
			| BCM2708_I2S_DMAEN, 0xffffffff);

	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_DREQ_A_REG,
			  BCM2708_I2S_TX_PANIC(0x10)
			| BCM2708_I2S_RX_PANIC(0x30)
			| BCM2708_I2S_TX(0x30)
			| BCM2708_I2S_RX(0x20), 0xffffffff);

	/* initialize and start the clock */
	bcm_2708_i2s_init_clock(dev, 5644800);

	/* clear TX FIFO */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			   BCM2708_I2S_TXCLR, BCM2708_I2S_TXCLR);

	/*
	 * Toggle the SYNC flag. After 2 PCM clock cycles it can be read back
	 * FIXME: This does not seem to work for slave mode!
	 */
	regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &syncval);
	syncval &= BCM2708_I2S_SYNC;
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_SYNC, ~syncval);
	/* Wait for the SYNC flag changing it's state */
	timeout= 100000;
	while (--timeout) {
		regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &csreg);
		if ((csreg & BCM2708_I2S_SYNC) != syncval)
			break;
	}
	if(!timeout){
		dprintk(DBG_INIT, "sync timeout\n");
	}

	regmap_write(dev->i2s_regmap, BCM2708_I2S_MODE_A_REG,
		BCM2708_I2S_FLEN(31) | BCM2708_I2S_FSLEN(1));


	txcreg= BCM2708_I2S_CH1( BCM2708_I2S_CHWEX|BCM2708_I2S_CHEN|BCM2708_I2S_CHWID(8) );
	regmap_write(dev->i2s_regmap, BCM2708_I2S_TXC_A_REG, txcreg);

	/* start I2S interface */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			   BCM2708_I2S_EN, BCM2708_I2S_EN);

	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_STBY, BCM2708_I2S_STBY);
	regmap_update_bits(dev->i2s_regmap,
			   BCM2708_I2S_CS_A_REG,
			   BCM2708_I2S_TXON, BCM2708_I2S_TXON);

	dev->ss= NULL;
	dprintk(DBG_INIT, "driver sucessfully initialized.\n");

	return 0;

out_card_create:
	snd_card_free(dev->card);
out_dma_alloc:
	dma_free_coherent(dev->dev,
			  SPDIF_FRAMESIZE*SPDIF_BUFSIZE_FRAMES,
			  dev->spdif_buffer,
			  dev->spdif_buffer_handle);
out_devm_kzalloc:
	devm_kfree(&pdev->dev, dev);
	return ret;
}

static int bcm2708_i2s_remove(struct platform_device *pdev)
{
	struct bcm2708_i2s_dev *dev;
	dev= dev_get_drvdata(&pdev->dev);

	dmaengine_terminate_all(dev->i2s_dma);
	dma_release_channel(dev->i2s_dma);
	snd_card_free(dev->card);
	dma_free_coherent(dev->dev,
			  SPDIF_FRAMESIZE*SPDIF_BUFSIZE_FRAMES,
			  dev->spdif_buffer,
			  dev->spdif_buffer_handle);
	devm_kfree(&pdev->dev, dev);
	dprintk(DBG_INIT, "driver unloaded.\n");
	return 0;
}

static const struct of_device_id spdif_of_match[] = {
	{ .compatible = "brcm,bcm2835-i2s" },
	{}
};

static struct platform_driver bcm2708_i2s_driver = {
	.probe		= bcm2708_i2s_probe,
	.remove		= bcm2708_i2s_remove,
	.driver		= {
		.name	= "bcm2835-i2s",
		.of_match_table = spdif_of_match
	},
};

module_platform_driver(bcm2708_i2s_driver);

MODULE_ALIAS("platform:bcm2835-i2s");
MODULE_ALIAS("of:N*T*Cbrcm,bcm2835-i2sC*");
MODULE_ALIAS("of:N*T*Cbrcm,bcm2835-i2s");
MODULE_DESCRIPTION("BCM2708 I2S/SPDIF output interface");
MODULE_AUTHOR("Matti Metsälä");
MODULE_LICENSE("GPL v2");
