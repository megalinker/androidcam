/*++

Module Name:

    loopback.h

Abstract:

    PhoneCam render->capture loopback FIFO. Shared by the "PhoneCam Audio"
    (render/speaker) and "PhoneCam Microphone" (capture) streams so that audio
    the receiver plays into the speaker surfaces on the microphone. Both
    endpoints default to 48 kHz / 16-bit / 2ch, so this is a plain byte FIFO.
    Underrun returns silence; overrun drops the oldest bytes.

--*/

#ifndef _PHONECAM_LOOPBACK_H_
#define _PHONECAM_LOOPBACK_H_

// Call once from DriverEntry.
void LoopbackInit();

// Clear the FIFO (e.g. when a stream starts/stops).
void LoopbackReset();

// Render side: push freshly rendered PCM into the FIFO.
void LoopbackWrite(_In_reads_bytes_(bytes) const void* src, _In_ ULONG bytes);

// Capture side: pull PCM from the FIFO into the mic buffer (silence on underrun).
void LoopbackRead(_Out_writes_bytes_(bytes) void* dst, _In_ ULONG bytes);

#endif // _PHONECAM_LOOPBACK_H_
