#ifndef AWJ_AVIF_PRIMARY_ITEM_ADAPTER_H
#define AWJ_AVIF_PRIMARY_ITEM_ADAPTER_H

#include <avif/avif.h>

#ifdef __cplusplus
extern "C" {
#endif

// Implemented inside the pinned, statically linked libavif read.c, not its public API.
// Precondition: avifDecoderParse(decoder) succeeded; the decoder and its I/O are
// borrowed and must not be changed concurrently. No I/O or pixel decoding occurs.
//
//  1: select PRIMARY_ITEM, then let libavif validate/decode it. Ordinary AV1 file
//     extents are independent of the selected color track's samples. A supported
//     derived primary (grid) remains a primary candidate, not a proven coded frame.
//  0: no declared primary, or its coded extent run aliases a whole color sample.
// -1: invalid primary role, unsafe/ambiguous association or bounds, unavailable
//     file I/O size, disabled frame limit when inspecting a track, or coded IDAT + track
//     (libavif does not retain IDAT's absolute file position).
//
// A negative result is an error, never permission to fall back to animation.
// A positive result is not proof of decodability: propagate PRIMARY_ITEM errors.
int awjAvifHasIndependentPrimary(const avifDecoder * decoder);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AWJ_AVIF_PRIMARY_ITEM_ADAPTER_H
