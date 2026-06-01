#ifndef VBE_H
#define VBE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/nearptr.h>

#define VBE_OK 0
#define VBE_VM86_FAIL 1
#define VBE_OUT_OF_DOS_MEM 2
#define VBE_OUT_OF_MEM 3
#define VBE_BROKEN_BIOS 4

#define VBE_VESA_ERROR_MASK 0x10000
#define VBE_VESA_ERRCODE_MASK 0x0FF00

#define VBE_DAC_8BIT 0x01
#define VBE_NONVGA_CRTC 0x02
#define VBE_SNOWED_RAMDAC 0x04
#define VBE_STEREOSCOPIC 0x08
#define VBE_STEREO_EVC 0x10

#define memText 0
#define memCGA 1
#define memHercules 2
#define memPL 3
#define memPK 4
#define mem256 5
#define memRGB 6
#define memYUV 7

#define MODE_ATTR_COLOR 0x08
#define MODE_ATTR_GRAPHICS 0x10
#define MODE_ATTR_LINEAR 0x80
#define MODE_WIN_RELOCATABLE 0x01
#define MODE_WIN_WRITEABLE 0x02

#define VESA_MODE_USE_LINEAR 0x4000

#define NEO_PAL 0
#define NEO_NTSC 1

struct VbeInfoBlock {
    char VESASignature[4];
    uint16_t VESAVersion;
    char *OemStringPtr;
    uint32_t Capabilities;
    uint16_t *VideoModePtr;
    uint16_t TotalMemory;
    uint16_t OemSoftwareRev;
    char *OemVendorNamePtr;
    char *OemProductNamePtr;
    char *OemProductRevPtr;
    uint8_t Reserved[222];
    uint8_t OemData[256];
} __attribute__((packed));

struct VesaModeInfoBlock {
    uint16_t ModeAttributes;
    uint8_t WinAAttributes;
    uint8_t WinBAttributes;
    uint16_t WinGranularity;
    uint16_t WinSize;
    uint16_t WinASegment;
    uint16_t WinBSegment;
    uint32_t WinFuncPtr;
    uint16_t BytesPerScanLine;
    uint16_t XResolution;
    uint16_t YResolution;
    uint8_t XCharSize;
    uint8_t YCharSize;
    uint8_t NumberOfPlanes;
    uint8_t BitsPerPixel;
    uint8_t NumberOfBanks;
    uint8_t MemoryModel;
    uint8_t BankSize;
    uint8_t NumberOfImagePages;
    uint8_t Reserved1;
    uint8_t RedMaskSize;
    uint8_t RedFieldPosition;
    uint8_t GreenMaskSize;
    uint8_t GreenFieldPosition;
    uint8_t BlueMaskSize;
    uint8_t BlueFieldPosition;
    uint8_t RsvdMaskSize;
    uint8_t RsvdFieldPosition;
    uint8_t DirectColorModeInfo;
    uint32_t PhysBasePtr;
    uint32_t Reserved2;
    uint16_t Reserved3;
    uint8_t Reserved4[206];
} __attribute__((packed));

#define VESA_CRTC_DOUBLESCAN 0x01
#define VESA_CRTC_INTERLACED 0x02
#define VESA_CRTC_HSYNC_NEG  0x04
#define VESA_CRTC_VSYNC_NEG  0x08

struct VesaCRTCInfoBlock {
    uint16_t hTotal;     /* Horizontal total in pixels */
    uint16_t hSyncStart; /* Horizontal sync start in pixels */
    uint16_t hSyncEnd;   /* Horizontal sync end in pixels */
    uint16_t vTotal;     /* Vertical total in lines */
    uint16_t vSyncStart; /* Vertical sync start in lines */
    uint16_t vSyncEnd;   /* Vertical sync end in lines */
    uint8_t  Flags;      /* Flags (Interlaced, Double Scan etc) */
    uint32_t PixelClock; /* Pixel clock in units of Hz */
    uint16_t RefreshRate;/* Refresh rate in units of 0.01 Hz*/
    uint8_t  Reserved[40];/* remainder of CRTCInfoBlock*/
} __attribute__((packed));

#ifdef __DJGPP__

static inline void convert_real_to_flat(void **ptr) {
    uint32_t val = (uint32_t)(*ptr);
    if (val) {
        uint32_t seg = val >> 16;
        uint32_t off = val & 0xffff;
        *ptr = (void *)(__djgpp_conventional_base + (seg << 4) + off);
    }
}

static inline int vbeInit(void) {
    return VBE_OK;
}

static inline int vbeGetControllerInfo(struct VbeInfoBlock *vib) {
    static char static_OemString[256];
    static uint16_t static_VideoModes[512];
    static char static_OemVendorName[256];
    static char static_OemProductName[256];
    static char static_OemProductRev[256];

    if (!vib) return 1;
    dosmemput(vib, sizeof(struct VbeInfoBlock), _go32_info_block.linear_address_of_transfer_buffer);
    
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F00;
    regs.x.es = _go32_info_block.linear_address_of_transfer_buffer >> 4;
    regs.x.di = 0;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    
    dosmemget(_go32_info_block.linear_address_of_transfer_buffer, sizeof(struct VbeInfoBlock), vib);
    
    convert_real_to_flat((void **)&vib->OemStringPtr);
    convert_real_to_flat((void **)&vib->VideoModePtr);
    convert_real_to_flat((void **)&vib->OemVendorNamePtr);
    convert_real_to_flat((void **)&vib->OemProductNamePtr);
    convert_real_to_flat((void **)&vib->OemProductRevPtr);

    if (vib->OemStringPtr) {
        strncpy(static_OemString, vib->OemStringPtr, 255);
        static_OemString[255] = '\0';
        vib->OemStringPtr = static_OemString;
    }
    if (vib->VideoModePtr) {
        int count = 0;
        while (vib->VideoModePtr[count] != 0xffff && count < 511) {
            static_VideoModes[count] = vib->VideoModePtr[count];
            count++;
        }
        static_VideoModes[count] = 0xffff;
        vib->VideoModePtr = static_VideoModes;
    }
    if (vib->OemVendorNamePtr) {
        strncpy(static_OemVendorName, vib->OemVendorNamePtr, 255);
        static_OemVendorName[255] = '\0';
        vib->OemVendorNamePtr = static_OemVendorName;
    }
    if (vib->OemProductNamePtr) {
        strncpy(static_OemProductName, vib->OemProductNamePtr, 255);
        static_OemProductName[255] = '\0';
        vib->OemProductNamePtr = static_OemProductName;
    }
    if (vib->OemProductRevPtr) {
        strncpy(static_OemProductRev, vib->OemProductRevPtr, 255);
        static_OemProductRev[255] = '\0';
        vib->OemProductRevPtr = static_OemProductRev;
    }
    
    return VBE_OK;
}

static inline int vbeGetModeInfo(int mode, struct VesaModeInfoBlock *vmib) {
    if (!vmib) return 1;
    
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F01;
    regs.x.cx = mode;
    regs.x.es = _go32_info_block.linear_address_of_transfer_buffer >> 4;
    regs.x.di = 0;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    
    dosmemget(_go32_info_block.linear_address_of_transfer_buffer, sizeof(struct VesaModeInfoBlock), vmib);
    
    return VBE_OK;
}

static inline int vbeSetMode(int mode, struct VesaCRTCInfoBlock *crtc) {
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F02;
    regs.x.bx = mode;
    
    if (crtc) {
        regs.x.bx |= 0x8000;
        dosmemput(crtc, sizeof(struct VesaCRTCInfoBlock), _go32_info_block.linear_address_of_transfer_buffer);
        regs.x.es = _go32_info_block.linear_address_of_transfer_buffer >> 4;
        regs.x.di = 0;
    }
    
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    return VBE_OK;
}

static inline int vbeGetMode(int *mode) {
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F03;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    if (mode) *mode = regs.x.bx;
    return VBE_OK;
}

static inline int vbeSetWindow(int window, int page) {
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F05;
    regs.h.bh = 0;
    regs.h.bl = window;
    regs.x.dx = page;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    return VBE_OK;
}

static inline int vbeSetDisplayStart(int offset, int vsync) {
    // Under DJGPP, these are defined in vo_vesa.c
    extern struct VesaModeInfoBlock video_mode_info;
    extern uint32_t dstBpp;
    int pixel_size = (dstBpp + 7) / 8;
    int bpl = video_mode_info.BytesPerScanLine;
    int line = bpl ? (offset / bpl) : 0;
    int pixel = (bpl && pixel_size) ? ((offset % bpl) / pixel_size) : 0;
    
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F07;
    regs.h.bh = vsync ? 0x80 : 0x00;
    regs.h.bl = 0;
    regs.x.cx = pixel;
    regs.x.dx = line;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    return VBE_OK;
}

static inline void *vbeMapVideoBuffer(unsigned long phys_addr, unsigned long size) {
    __dpmi_meminfo mem;
    mem.address = phys_addr;
    mem.size = size;
    if (__dpmi_physical_address_mapping(&mem) == -1) {
        return NULL;
    }
    return (void *)(mem.address + __djgpp_conventional_base);
}

static inline void vbeUnmapVideoBuffer(unsigned long ptr, unsigned long size) {
    __dpmi_meminfo mem;
    mem.address = ptr - __djgpp_conventional_base;
    mem.size = size;
    __dpmi_free_physical_address_mapping(&mem);
}

static inline int get_state_size(void) {
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F04;
    regs.h.dl = 0;
    regs.x.cx = 0xF;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    if (regs.x.ax != 0x004F) return 0;
    return regs.x.bx * 64;
}

static inline int vbeSaveState(void **state) {
    int size = get_state_size();
    if (size == 0) return 1;
    uint8_t *buf = malloc(size + 4);
    if (!buf) return VBE_OUT_OF_MEM;
    *(uint32_t *)buf = size;
    
    if (size > _go32_info_block.size_of_transfer_buffer) {
        free(buf);
        return VBE_OUT_OF_DOS_MEM;
    }
    
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F04;
    regs.h.dl = 1;
    regs.x.cx = 0xF;
    regs.x.es = _go32_info_block.linear_address_of_transfer_buffer >> 4;
    regs.x.bx = 0;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    if (regs.x.ax != 0x004F) {
        free(buf);
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    
    dosmemget(_go32_info_block.linear_address_of_transfer_buffer, size, buf + 4);
    *state = buf;
    return VBE_OK;
}

static inline int vbeRestoreState(void *state) {
    if (!state) return VBE_OK;
    uint8_t *buf = (uint8_t *)state;
    uint32_t size = *(uint32_t *)buf;
    
    if (size > _go32_info_block.size_of_transfer_buffer) {
        return VBE_OUT_OF_DOS_MEM;
    }
    
    dosmemput(buf + 4, size, _go32_info_block.linear_address_of_transfer_buffer);
    
    __dpmi_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x4F04;
    regs.h.dl = 2;
    regs.x.cx = 0xF;
    regs.x.es = _go32_info_block.linear_address_of_transfer_buffer >> 4;
    regs.x.bx = 0;
    __dpmi_simulate_real_mode_interrupt(0x10, &regs);
    
    if (regs.x.ax != 0x004F) {
        return VBE_VESA_ERROR_MASK | regs.x.ax;
    }
    return VBE_OK;
}

static inline int vbeGetPixelClock(unsigned *mode, unsigned *pixclk) {
    return VBE_OK;
}

static inline int vbeSetTV(int mode, int tvnorm) {
    return VBE_OK;
}

static inline void vbeWriteString(int x, int y, int attr, const char *str) {
}

static inline void *PhysToVirtSO(uint16_t seg, uint16_t off) {
    return (void *)(__djgpp_conventional_base + (seg << 4) + off);
}

static inline void vbeDestroy(void) {
}

#endif // __DJGPP__

#endif // VBE_H
