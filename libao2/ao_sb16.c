
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/nearptr.h>
#include <sys/movedata.h>
#include <unistd.h>

#include "config.h"
#include "audio_out.h"
#include "audio_out_internal.h"
#include "libaf/af_format.h"
#include "mp_msg.h"

extern int __dpmi_allocate_dos_memory(int paragraphs, int *selector);
extern int __dpmi_free_dos_memory(int selector);

static ao_info_t info =
{
	"Sound Blaster 16 driver",
	"sb16",
	"heavily inspired by joncampbell123 efforts",
	""
};

LIBAO_EXTERN(sb16)

static uint16_t sb_port = 0x220;
static int sb_irq = 5;
static int sb_dma8 = 1;
static int sb_dma16 = 5;

static int dma_selector = -1;
static uint32_t dma_buffer_phys = 0;
static int total_size = 32768;
static int block_size = 16384;
static int write_pos = 0;

static uint8_t orig_pic1_mask = 0xff;
static uint8_t orig_pic2_mask = 0xff;
static int pic_masked = 0;
static int play_called = 0;

/* masks hardware interrupts */
static void mask_sb_irq(int irq)
{
	orig_pic1_mask = inportb(0x21);
	orig_pic2_mask = inportb(0xa1);

	/* Always mask IRQ 5 and 7 to prevent interrupt storms
	   if the BLASTER variable is missing or incorrect. */
	uint8_t new_pic1 = orig_pic1_mask | (1 << 5) | (1 << 7);
	
	if (irq < 8) {
		new_pic1 |= (1 << irq);
		outportb(0x21, new_pic1);
	} else {
		outportb(0x21, new_pic1);
		outportb(0xa1, orig_pic2_mask | (1 << (irq - 8)));
	}

	pic_masked = 1;
}

/* restores original interrupts */
static void unmask_sb_irq(int irq) {
	if (!pic_masked) return;

	outportb(0x21, orig_pic1_mask);
	outportb(0xa1, orig_pic2_mask);
	pic_masked = 0;
}

/* creates a busy loop delay */
static inline void delay_ms(int ms) {
	int lps = ms * 1000;

	/* port 0x80 is used for small delays */
	for (volatile int i = 0; i < lps; i++) {
		inportb(0x80);
	}
}

/* resets the digital signal processor */
static int sb16_dsp_reset(uint16_t port) {
	/* trigger reset port */
	outportb(port + 0x6, 1);
	delay_ms(10);
	outportb(port + 0x6, 0);
	delay_ms(10);

	int timeout = 10000;
	while (timeout--) {
		/* wait for ready byte 0xAA */
		if (inportb(port + 0xE) & 0x80) {
			if (inportb(port + 0xA) == 0xAA) {
				return 1;
			}
		}
	}
	return 0;
}

/* writes a byte to the dsp */
static inline void sb16_dsp_write(uint16_t port, uint8_t val) {
	/* dsp busy loop */
	while (inportb(port + 0xC) & 0x80);
	outportb(port + 0xC, val);
}

/* detects configuration from environment */
static int sb16_detect(void) {
	char *blaster = getenv("BLASTER");
	if (!blaster) {
		return 1;
	}

	/* parse BLASTER variable */
	char *p = blaster;
	while (*p) {
		if (*p == 'A' || *p == 'a') sb_port = strtol(p + 1, NULL, 16);
		else if (*p == 'I' || *p == 'i') sb_irq = strtol(p + 1, NULL, 10);
		else if (*p == 'D' || *p == 'd') sb_dma8 = strtol(p + 1, NULL, 10);
		else if (*p == 'H' || *p == 'h') sb_dma16 = strtol(p + 1, NULL, 10);

		p++;
		while (*p && *p != ' ') p++;
		while (*p && *p == ' ') p++;
	}
	return 1;
}

/* allocates dos memory for dma */
static int allocate_dma_buffer(int size) {
	int paragraphs = (size*2+15)/16;
	int segment = __dpmi_allocate_dos_memory(paragraphs, &dma_selector);

	if (segment == -1) {
		return 0;
	}
	uint32_t phys = (uint32_t)segment * 16;

	uint32_t start1 = phys;
	uint32_t end1 = phys + size - 1;

	/* memory calculation: VMs often have buggy 16-bit DMA that wraps at 64KB !? */
	if ((start1 / 65536) == (end1 / 65536)) {
		dma_buffer_phys = start1;
	} else {
		dma_buffer_phys = phys + size;
	}
	return 1;
}

/* frees dos memory */
static void free_dma_buffer(void) {
	if (dma_selector != -1) {
		__dpmi_free_dos_memory(dma_selector);
		dma_selector = -1;
		dma_buffer_phys = 0;
	}
}

/* Returns the current hardware DMA read offset (in bytes) within the
 * ring buffer [0 .. total_size-1].
 *
 * Do _not_ read sb_port+0xE or sb_port+0xF here — those are interrupt
 * ACK ports and must only be touched at uninit time. */
static inline int dma_get_play_pos(void)
{
	uint16_t addr_a, addr_b;

	do {
		outportb(0xD8, 0);                   /* clear flip-flop */
		addr_a  = inportb(0xC4);             /* word addr low */
		addr_a |= (inportb(0xC4) << 8);     /* word addr high */

		outportb(0xD8, 0);
		addr_b  = inportb(0xC4);
		addr_b |= (inportb(0xC4) << 8);
	} while (abs((int)addr_a - (int)addr_b) > 32);

	uint32_t phys_byte = (uint32_t)addr_b * 2;
	int offset = (int)((phys_byte - dma_buffer_phys) & (uint32_t)(total_size - 1));
	return offset;
}

/* sets up dma controller */
static void dma_setup(uint32_t phys_addr, int size) {
	uint32_t word_addr = phys_addr >> 1;
	uint16_t word_count = size / 2;

	/* program dma channel 5 */
	outportb(0xD4, 5);
	outportb(0xD6, 0x59);
	outportb(0xD8, 0);
	outportb(0x8B, phys_addr >> 16);
	outportb(0xC4, word_addr & 0xFF);
	outportb(0xC4, word_addr >> 8);
	outportb(0xC6, (word_count - 1) & 0xFF);
	outportb(0xC6, (word_count - 1) >> 8);
	outportb(0xD4, 1);
}

/* control callback */
static int control(int cmd, void *arg) {
	if (cmd == AOCONTROL_QUERY_FORMAT) {
		if (*(int *)arg == AF_FORMAT_S16_LE) {
			return CONTROL_TRUE;
		}
		return CONTROL_FALSE;
	}
	return CONTROL_NA;
}

/* initializes audio playback */
static int init(int rate, int channels, int format, int flags) {
	(void)flags;

	if (format != AF_FORMAT_S16_LE) {
		mp_msg(MSGT_AO, MSGL_V, "ao_sb16: Forcing format to S16_LE\n");
		ao_data.format = AF_FORMAT_S16_LE;
		format = AF_FORMAT_S16_LE;
	}

	if (rate > 44100) {
		mp_msg(MSGT_AO, MSGL_V, "ao_sb16: Forcing rate to 44100 (max)\n");
		ao_data.samplerate = 44100;
		rate = 44100;
	}

	if (channels > 2) {
		mp_msg(MSGT_AO, MSGL_V, "ao_sb16: Forcing channels to 2 (max)\n");
		ao_data.channels = 2;
		channels = 2;
	}

	sb16_detect();
	mask_sb_irq(sb_irq);

	if (!sb16_dsp_reset(sb_port)) {
		mp_msg(MSGT_AO, MSGL_ERR, "ao_sb16: DSP reset failed on port 0x%X\n", sb_port);
		unmask_sb_irq(sb_irq);
		return 0;
	}

	if (!allocate_dma_buffer(total_size)) {
		mp_msg(MSGT_AO, MSGL_ERR, "ao_sb16: Failed to allocate %d bytes of DOS memory for DMA!\n", total_size * 2);
		unmask_sb_irq(sb_irq);
		return 0;
	}

	/* clear buffer using dosmemput */
	uint8_t *temp = (uint8_t *)calloc(1, total_size);
	if (temp) {
		dosmemput(temp, total_size, dma_buffer_phys);
		free(temp);
	}

	dma_setup(dma_buffer_phys, total_size);

	/* set sample rate */
	sb16_dsp_write(sb_port, 0x41);
	sb16_dsp_write(sb_port, rate >> 8);
	sb16_dsp_write(sb_port, rate & 0xFF);

	/* start 16 bit auto init dma */
	sb16_dsp_write(sb_port, 0xB6);
	if (channels == 2) {
		sb16_dsp_write(sb_port, 0x30); /* Signed, Stereo */
	} else {
		sb16_dsp_write(sb_port, 0x10); /* Signed, Mono */
	}

	uint16_t dsp_len = (block_size / 2) - 1;
	sb16_dsp_write(sb_port, dsp_len & 0xFF);
	sb16_dsp_write(sb_port, dsp_len >> 8);

	ao_data.samplerate = rate;
	ao_data.channels = channels;
	ao_data.format = AF_FORMAT_S16_LE;
	ao_data.bps = rate * channels * 2;
	ao_data.buffersize = total_size;
	ao_data.outburst = block_size;

	write_pos = 0;
	play_called = 0;

	return 1;
}

/* stops audio and cleans up */
static void uninit(int immed) {
	(void)immed;

	if (dma_buffer_phys) {
		/* send halt command */
		sb16_dsp_write(sb_port, 0xD5);
		outportb(0xD4, 5);
		inportb(sb_port + 0xF);
		inportb(sb_port + 0xE);

		sb16_dsp_reset(sb_port);
		unmask_sb_irq(sb_irq);
		free_dma_buffer();
	}
}

/* resets write position to current hardware playback position */
static void reset(void) {
	if (dma_buffer_phys) {
		/* Set write_pos just ahead of the current hardware read position
		 * using the absolute DMA address, consistent with get_space. */
		write_pos = dma_get_play_pos();
	}
}

/* calculates available buffer space.
 *
 * The SB16 is running autoinit DMA over the full total_size ring buffer.
 * We poll the DMA address register to get the absolute hardware read
 * position. This is reliable across both halves of the buffer
 *
 * free_space = (play_pos - write_pos) mod total_size,
 * minus a small guard zone to avoid overwriting data mid-read.
 */
static int get_space(void) {
	if (!dma_buffer_phys) return 0;

	/* Absolute byte offset that the hardware DMA is currently reading */
	int play_pos = dma_get_play_pos();

	int free_space = play_pos - write_pos;
	if (free_space <= 0) free_space += total_size;

	if (free_space > 1024) {
		/* Return space rounded down to block_size to avoid trickle feeding */
		return ((free_space - 1024) / block_size) * block_size;
	}

	return 0;
}

/* pushes audio data to dma buffer */
static int play(void *data, int len, int flags) {
	(void)flags;

	if (!play_called) play_called = 1;
	if (!dma_buffer_phys) return 0;

	int space = get_space();

	if (len > space) len = space;
	if (len <= 0) return 0;

	int first_part = total_size - write_pos;
	if (first_part > len) first_part = len;

	/* write first part into ring buffer */
	dosmemput(data, first_part, dma_buffer_phys + write_pos);

	if (first_part < len) {
		/* wrap around and write remainder */
		dosmemput((uint8_t *)data + first_part, len - first_part, dma_buffer_phys);
	}

	write_pos = (write_pos + len) % total_size;
	return len;
}

/* calculates audio lag.
 * Returns the amount of audio currently buffered,
 *
 * Uses the same absolute DMA address as get_space() for consistency. */
static float get_delay(void) {
	if (!dma_buffer_phys) return 0.0f;

	int play_pos = dma_get_play_pos();

	int buffered = write_pos - play_pos;
	if (buffered < 0) buffered += total_size;

	if (buffered <= 0) return 0.0f;
	return (float)buffered / (float)ao_data.bps;
}

/* pauses playback */
static void audio_pause(void) {
	/* 0xD5 halts playback */
	if (dma_buffer_phys) sb16_dsp_write(sb_port, 0xD5);
}

/* resumes playback */
static void audio_resume(void) {
	/* 0xD6 resumes playback */
	if (dma_buffer_phys) sb16_dsp_write(sb_port, 0xD6);
}

