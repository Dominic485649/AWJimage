#ifndef AWJ_AVIF_GAIN_MAP_THREAD_ADAPTER_H
#define AWJ_AVIF_GAIN_MAP_THREAD_ADAPTER_H

#include <avif/avif.h>

#ifdef __cplusplus
#define AWJ_AVIF_GAIN_MAP_NOEXCEPT noexcept
extern "C" {
#else
#define AWJ_AVIF_GAIN_MAP_NOEXCEPT
#endif

// Private ABI implemented by the pinned libavif gainmap.c appendix. The patch
// copies this header to include/avif/awj_gain_map.h; these are NOT upstream APIs.
// All inputs are borrowed until this synchronous call returns. No concurrent
// mutation, callback re-entry into this operation, or unloading is permitted.
// The callback can run concurrently on any worker (including the caller), must
// be thread-safe/nonblocking, and must not throw. A nonzero result cancels.
typedef avifBool (*awjAvifGainMapCancelCallback)(void * user_data) AWJ_AVIF_GAIN_MAP_NOEXCEPT;

typedef struct awjAvifGainMapExecution
{
    // Actual peak worker count, INCLUDING the caller; 0 before work starts.
    // This is not CPU utilization or the requested thread budget.
    uint32_t workers_started;
    avifBool cancelled;
} awjAvifGainMapExecution;

// max_threads is the entire operation's budget: 0 means 1, never autodetect.
// Parallelism is capped at 64 and floor(pixel_count / 16384), at least one.
// Inner libavif conversions remain single-threaded and never overlap the pool.
// Per-worker statistics scratch is <= 80 KiB, independent of image dimensions.
// Apply CLLI preserves upstream float accumulation in exact raster order using
// <= 4096 values per worker, not a full-image luminance buffer or reordered sum.
// Cancellation is checked every <= 256 math pixels and around upstream serial
// scale/reformat calls (those library calls cannot be interrupted internally).
//
// execution and cancel may be NULL. On cancellation: UNKNOWN_ERROR, cancelled
// = AVIF_TRUE and a diagnostic containing "cancelled". Allocation/metadata
// errors keep their real avifResult; they are not treated as fallback success.
// Failure leaves output image, metadata and CLLI unchanged. Successful apply
// replaces libavif-owned output pixels; successful compute replaces owned map
// planes while preserving gainMap->image identity and non-math metadata.
// Do not alias output objects or their owned pixel storage with input objects.
avifResult awjAvifRGBImageApplyGainMap(const avifRGBImage * baseImage,
                                     avifColorPrimaries baseColorPrimaries,
                                     avifTransferCharacteristics baseTransferCharacteristics,
                                     const avifGainMap * gainMap,
                                     float hdrHeadroom,
                                     avifColorPrimaries outputColorPrimaries,
                                     avifTransferCharacteristics outputTransferCharacteristics,
                                     avifRGBImage * toneMappedImage,
                                     avifContentLightLevelInformationBox * clli,
                                     avifDiagnostics * diag,
                                     uint32_t max_threads,
                                     awjAvifGainMapCancelCallback cancel,
                                     void * user_data,
                                     awjAvifGainMapExecution * execution) AWJ_AVIF_GAIN_MAP_NOEXCEPT;

avifResult awjAvifRGBImageComputeGainMap(const avifRGBImage * baseRgbImage,
                                       avifColorPrimaries baseColorPrimaries,
                                       avifTransferCharacteristics baseTransferCharacteristics,
                                       const avifRGBImage * altRgbImage,
                                       avifColorPrimaries altColorPrimaries,
                                       avifTransferCharacteristics altTransferCharacteristics,
                                       avifGainMap * gainMap,
                                       avifDiagnostics * diag,
                                       uint32_t max_threads,
                                       awjAvifGainMapCancelCallback cancel,
                                       void * user_data,
                                       awjAvifGainMapExecution * execution) AWJ_AVIF_GAIN_MAP_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif
#undef AWJ_AVIF_GAIN_MAP_NOEXCEPT
#endif // AWJ_AVIF_GAIN_MAP_THREAD_ADAPTER_H
