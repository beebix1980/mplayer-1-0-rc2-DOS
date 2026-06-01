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

	/* Always mask IRQ 5 and 7 (common SB16 IRQs) to prevent interrupt storms
	   if the BLASTER variable is missing or incorrect.
	   We do NOT mask 9, 10, 11 unconditionally to avoid breaking hard drives. */
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

	/* memory calculation: VMs often have buggy 16-bit DMA that wraps at 64KB! */
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

/* gets current dma transfer count */
static inline uint16_t dma_get_count(void)
{
	uint16_t a, b;

	/* secondary read operation */
	do {
		outportb(0xD8, 0);
		a = inportb(0xC6);
		a |= (inportb(0xC6) << 8);

		outportb(0xD8, 0);
		b = inportb(0xC6);
		b |= (inportb(0xC6) << 8);
	} while (abs((int)a - (int)b) > 64);

	/* ACK 16 BIT INTERRUPT */
	inportb(sb_port + 0xF);

	/* ACK 8 BIT INTERRUPT */
	inportb(sb_port + 0xE);

	if (b > 16384) b = 16384;
	return b;
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
		mp_msg(MSGT_AO, MSGL_ERR, "ao_sb16: Unsupported format, only S16_LE is supported.\n");
		return 0;
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
	sb16_dsp_write(sb_port, 0x30);

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

/* resets write position */
static void reset(void) {
	if (dma_buffer_phys) {
		uint16_t rem_words = dma_get_count();
		int rem_bytes = rem_words * 2;
		int play_pos = total_size - rem_bytes;

		write_pos = play_pos % total_size;
	}
}

/* calculates available buffer space */
static int get_space(void) {
	if (!dma_buffer_phys) return 0;

	uint16_t rem_words = dma_get_count();
	int rem_bytes = rem_words * 2;
	int play_pos = total_size - rem_bytes;

	int free_space = play_pos - write_pos;

	if (free_space < 0) free_space += total_size;

	if (free_space > 1024) {
		return free_space - 1024;
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

/* calculates audio lag */
static float get_delay(void) {
	if (!dma_buffer_phys) return 0.0f;

	int space = get_space();
	int buffered = total_size - space;

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
