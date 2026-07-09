/*++

Module Name:

    loopback.cpp

Abstract:

    PhoneCam render->capture loopback FIFO (see loopback.h).

    A single process-wide ring buffer connects the one virtual speaker (render)
    to the one virtual microphone (capture). The render stream's UpdatePosition
    taps the consumed PCM via LoopbackWrite(); the capture stream's WriteBytes()
    fills the mic DMA buffer via LoopbackRead(). Access is serialized with a
    spin lock because both run in the periodic DPC at DISPATCH_LEVEL.

    Buffers live in non-paged global data (the DPC path may not touch paged
    memory). ~0.5 s of headroom absorbs jitter; in steady state (render and
    capture advance at the same 48k/16/2 rate) the FIFO stays near-empty, so the
    added latency is small.

--*/

#include "definitions.h"
#include "loopback.h"

// ~0.5 second at 48 kHz / 16-bit / 2ch = 192000 bytes/sec.
#define PHONECAM_LOOPBACK_CAPACITY  (96000)

static KSPIN_LOCK   g_LoopLock;
static BOOLEAN      g_LoopInit = FALSE;
static ULONG        g_Head = 0;     // next write index
static ULONG        g_Tail = 0;     // next read index
static ULONG        g_Count = 0;    // valid bytes currently in the ring
static BYTE         g_Ring[PHONECAM_LOOPBACK_CAPACITY];

#pragma code_seg()   // non-paged: called from the DISPATCH_LEVEL DPC

void LoopbackInit()
{
    KeInitializeSpinLock(&g_LoopLock);
    g_Head = g_Tail = g_Count = 0;
    g_LoopInit = TRUE;
}

void LoopbackReset()
{
    KIRQL irql;
    if (!g_LoopInit) { return; }
    KeAcquireSpinLock(&g_LoopLock, &irql);
    g_Head = g_Tail = g_Count = 0;
    KeReleaseSpinLock(&g_LoopLock, irql);
}

void LoopbackWrite(const void* src, ULONG bytes)
{
    KIRQL irql;
    const BYTE* p = (const BYTE*)src;

    if (!g_LoopInit || p == NULL || bytes == 0) { return; }

    KeAcquireSpinLock(&g_LoopLock, &irql);
    for (ULONG i = 0; i < bytes; ++i)
    {
        g_Ring[g_Head] = p[i];
        g_Head = (g_Head + 1) % PHONECAM_LOOPBACK_CAPACITY;
        if (g_Count < PHONECAM_LOOPBACK_CAPACITY)
        {
            g_Count++;
        }
        else
        {
            // Overrun: drop the oldest byte so the freshest audio is kept.
            g_Tail = (g_Tail + 1) % PHONECAM_LOOPBACK_CAPACITY;
        }
    }
    KeReleaseSpinLock(&g_LoopLock, irql);
}

void LoopbackRead(void* dst, ULONG bytes)
{
    KIRQL irql;
    BYTE* p = (BYTE*)dst;

    if (p == NULL || bytes == 0) { return; }
    if (!g_LoopInit) { RtlZeroMemory(dst, bytes); return; }

    KeAcquireSpinLock(&g_LoopLock, &irql);
    for (ULONG i = 0; i < bytes; ++i)
    {
        if (g_Count > 0)
        {
            p[i] = g_Ring[g_Tail];
            g_Tail = (g_Tail + 1) % PHONECAM_LOOPBACK_CAPACITY;
            g_Count--;
        }
        else
        {
            p[i] = 0;   // underrun -> silence
        }
    }
    KeReleaseSpinLock(&g_LoopLock, irql);
}
