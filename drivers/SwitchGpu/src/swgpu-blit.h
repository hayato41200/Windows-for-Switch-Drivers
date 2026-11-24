#pragma once

/* Kernel-only, synchronous IRP_MJ_INTERNAL_DEVICE_CONTROL. MDLs stay valid
 * through completion. Offset is relative to the first byte of each MDL. */
#define SWGPU_BLIT_VERSION 1u
#define SWGPU_BLIT_MAX_RECTS 64u
#define SWGPU_BLIT_WINDOW_BYTES 0x01000000u
#define IOCTL_SWGPU_PRESENT_BLIT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* Emergency reset/drain could not be confirmed. Caller must retain its
 * backing pages and avoid touching the destination until device reset. */
#define SWGPU_BLIT_DMA_UNSAFE ((NTSTATUS)0xE00000ACu)

static const GUID SwGpuBlitDeviceInterface =
    {0x2cc9cb4d, 0x6412, 0x4f31, {0xa7, 0x81, 0x5d, 0x78, 0x1e, 0x09, 0x4b, 0x63}};

typedef struct _SWGPU_BLIT_RECT {
    ULONG SrcOffset, DstOffset, RowBytes, Rows;
} SWGPU_BLIT_RECT;

typedef struct _SWGPU_BLIT_REQUEST {
    ULONG Version, SrcPitch, DstPitch, Count;
    PMDL Src, Dst;
    SWGPU_BLIT_RECT Rect[SWGPU_BLIT_MAX_RECTS];
} SWGPU_BLIT_REQUEST;
