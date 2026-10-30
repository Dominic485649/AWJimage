# libavif v1.4.2 / c5240fc79fe5c2407e10afd35f5505ef6333ea49.
# Append private entry points; leave every upstream byte/function unchanged.
# Reject unknown upstream input and damaged/partial appendices, including on reruns.
if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "libavif gain-map threads patch: SOURCE_DIR is required")
endif()
set(GAINMAP_C "${SOURCE_DIR}/src/gainmap.c")
set(INTERNAL_H "${SOURCE_DIR}/include/avif/internal.h")
set(ADAPTER_H "${CMAKE_CURRENT_LIST_DIR}/../src/codecs/avif_gain_map_thread_adapter.h")
foreach(INPUT IN ITEMS "${GAINMAP_C}" "${INTERNAL_H}" "${ADAPTER_H}")
    if(NOT EXISTS "${INPUT}" OR IS_DIRECTORY "${INPUT}")
        message(FATAL_ERROR "libavif gain-map threads patch: missing ${INPUT}")
    endif()
endforeach()
file(READ "${INTERNAL_H}" INTERNAL)
string(REPLACE "\r\n" "\n" INTERNAL "${INTERNAL}")
string(SHA256 INTERNAL_HASH "${INTERNAL}")
if(NOT INTERNAL_HASH STREQUAL "3b23c7a4a4d83ed9b6b03531b0b205675e71e07284d7e39d01531f94c12189ce")
    message(FATAL_ERROR "libavif gain-map threads patch: unknown internal.h; upstream review required")
endif()
file(READ "${GAINMAP_C}" CURRENT)
string(REPLACE "\r\n" "\n" CURRENT "${CURRENT}")
set(BEGIN_MARKER "\n// AWJ_GAIN_MAP_THREADS_BEGIN\n")
string(FIND "${CURRENT}" "${BEGIN_MARKER}" BEGIN_POS)
if(BEGIN_POS EQUAL -1)
    set(PRISTINE "${CURRENT}")
else()
    string(SUBSTRING "${CURRENT}" 0 ${BEGIN_POS} PRISTINE)
endif()
string(SHA256 PRISTINE_HASH "${PRISTINE}")
if(NOT PRISTINE_HASH STREQUAL "b92807d0e621c956239f092f43d0ca5b95458b2ae333e4a4450dc963f173804b")
    message(FATAL_ERROR "libavif gain-map threads patch: unknown gainmap.c; upstream review required")
endif()

set(APPENDIX [=[
// AWJ_GAIN_MAP_THREADS_BEGIN
#include "avif/awj_gain_map.h"
#include <errno.h>
#include <stdlib.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#define AWJ_GAIN_WORKERS 64
#define AWJ_GAIN_BLOCK 4096
#define AWJ_GAIN_BUCKETS 10000

// A persistent, per-call pool: no global executor, hardware query, nested work,
// detached lifetime, or exception-capable C++ runtime. Each epoch is a barrier.
// Creation completes before any pixel work. Partial resource-exhaustion failure
// leaves a smaller pool covering the SAME complete partition, with caller 0.
typedef struct awjGainPool awjGainPool;
typedef struct awjGainWorker
{
    awjGainPool * pool;
    size_t begin, end;
    void * scratch;
    float min[3], max[3];
    avifResult result;
    avifBool cancelled;
    avifDiagnostics diag;
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif
} awjGainWorker;
typedef void (*awjGainTask)(awjGainWorker *, void *);
struct awjGainPool
{
    awjGainWorker workers[AWJ_GAIN_WORKERS];
    uint32_t count;
    uint32_t remaining;
    uint64_t epoch;
    avifBool initialized, shutdown;
    awjGainTask task;
    void * context;
    void * scratch;
    awjAvifGainMapCancelCallback cancel;
    void * userData;
    awjAvifGainMapExecution * execution;
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE work, done;
#else
    pthread_mutex_t mutex;
    pthread_cond_t work, done;
#endif
};

// Compiled ONLY in the renamed test translation unit, never in production.
#ifdef AWJ_AVIF_GAIN_MAP_TESTING
static int awjGainTestThreadLimit = -1;
static int awjGainTestAllocationFailure = 0;
void awjAvifGainMapTestFaults(int threadLimit, int allocationFailure)
{
    awjGainTestThreadLimit = threadLimit;
    awjGainTestAllocationFailure = allocationFailure;
}
#endif

// Errors for a known-valid lock/join are broken runtime invariants, not a safe
// fallback: returning while a worker can still borrow this stack would be UAF.
static void awjGainLock(awjGainPool * p)
{
#if defined(_WIN32)
    EnterCriticalSection(&p->mutex);
#else
    if (pthread_mutex_lock(&p->mutex) != 0) abort();
#endif
}
static void awjGainUnlock(awjGainPool * p)
{
#if defined(_WIN32)
    LeaveCriticalSection(&p->mutex);
#else
    if (pthread_mutex_unlock(&p->mutex) != 0) abort();
#endif
}
static void awjGainWake(awjGainPool * p, avifBool done)
{
#if defined(_WIN32)
    WakeAllConditionVariable(done ? &p->done : &p->work);
#else
    if (pthread_cond_broadcast(done ? &p->done : &p->work) != 0) abort();
#endif
}
static void awjGainWait(awjGainPool * p, avifBool done)
{
#if defined(_WIN32)
    if (!SleepConditionVariableCS(done ? &p->done : &p->work, &p->mutex, INFINITE)) abort();
#else
    if (pthread_cond_wait(done ? &p->done : &p->work, &p->mutex) != 0) abort();
#endif
}
#if defined(_WIN32)
static unsigned int __stdcall awjGainThread(void * opaque)
#else
static void * awjGainThread(void * opaque)
#endif
{
    awjGainWorker * w = (awjGainWorker *)opaque;
    awjGainPool * p = w->pool;
    uint64_t seen = 0;
    awjGainLock(p);
    for (;;) {
        while (!p->shutdown && seen == p->epoch) awjGainWait(p, AVIF_FALSE);
        if (p->shutdown) break;
        seen = p->epoch;
        awjGainUnlock(p);
        p->task(w, p->context);
        awjGainLock(p);
        --p->remaining;
        if (p->remaining == 0) awjGainWake(p, AVIF_TRUE);
    }
    awjGainUnlock(p);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
static void awjGainPoolDestroy(awjGainPool * p)
{
    if (p->initialized) {
        awjGainLock(p);
        p->shutdown = AVIF_TRUE;
        awjGainWake(p, AVIF_FALSE);
        awjGainUnlock(p);
        for (uint32_t k = 1; k < p->count; ++k) {
#if defined(_WIN32)
            if (WaitForSingleObject(p->workers[k].thread, INFINITE) != WAIT_OBJECT_0) abort();
            if (!CloseHandle(p->workers[k].thread)) abort();
#else
            if (pthread_join(p->workers[k].thread, NULL) != 0) abort();
#endif
        }
#if defined(_WIN32)
        DeleteCriticalSection(&p->mutex);
#else
        if (pthread_cond_destroy(&p->done) != 0 || pthread_cond_destroy(&p->work) != 0 ||
            pthread_mutex_destroy(&p->mutex) != 0) abort();
#endif
        p->initialized = AVIF_FALSE;
    }
    avifFree(p->scratch);
    p->scratch = NULL;
}
static avifResult awjGainPoolInit(awjGainPool * p, size_t pixels, uint32_t budget, size_t scratchSize)
{
    size_t wanted = AVIF_MIN((size_t)(budget ? budget : 1), (size_t)AWJ_GAIN_WORKERS);
    wanted = AVIF_MAX((size_t)1, AVIF_MIN(wanted, pixels / 16384));
    if (scratchSize) {
#ifdef AWJ_AVIF_GAIN_MAP_TESTING
        if (awjGainTestAllocationFailure) return AVIF_RESULT_OUT_OF_MEMORY;
#endif
        p->scratch = avifAlloc(wanted * scratchSize); // both factors are fixed/capped
        if (!p->scratch) return AVIF_RESULT_OUT_OF_MEMORY;
    }
    p->count = 1;
    for (size_t k = 0; k < wanted; ++k) {
        p->workers[k].pool = p;
        p->workers[k].scratch = scratchSize ? (uint8_t *)p->scratch + k * scratchSize : NULL;
    }
    if (wanted > 1) {
#if defined(_WIN32)
        if (!InitializeCriticalSectionEx(&p->mutex, 0, 0)) return AVIF_RESULT_OUT_OF_MEMORY;
        InitializeConditionVariable(&p->work);
        InitializeConditionVariable(&p->done);
#else
        int error = pthread_mutex_init(&p->mutex, NULL);
        if (error) return error == ENOMEM ? AVIF_RESULT_OUT_OF_MEMORY : AVIF_RESULT_UNKNOWN_ERROR;
        error = pthread_cond_init(&p->work, NULL);
        if (error) {
            pthread_mutex_destroy(&p->mutex);
            return error == ENOMEM ? AVIF_RESULT_OUT_OF_MEMORY : AVIF_RESULT_UNKNOWN_ERROR;
        }
        error = pthread_cond_init(&p->done, NULL);
        if (error) {
            pthread_cond_destroy(&p->work);
            pthread_mutex_destroy(&p->mutex);
            return error == ENOMEM ? AVIF_RESULT_OUT_OF_MEMORY : AVIF_RESULT_UNKNOWN_ERROR;
        }
#endif
        p->initialized = AVIF_TRUE;
        for (uint32_t k = 1; k < (uint32_t)wanted; ++k) {
            int error = 0;
#ifdef AWJ_AVIF_GAIN_MAP_TESTING
            if (awjGainTestThreadLimit >= 0 && k > (uint32_t)awjGainTestThreadLimit) error = EAGAIN;
#endif
            if (!error) {
#if defined(_WIN32)
                p->workers[k].thread = (HANDLE)_beginthreadex(NULL, 0, awjGainThread, &p->workers[k], 0, NULL);
                if (!p->workers[k].thread) error = errno ? errno : EAGAIN;
#else
                error = pthread_create(&p->workers[k].thread, NULL, awjGainThread, &p->workers[k]);
#endif
            }
            if (error) {
                if (p->execution) p->execution->workers_started = p->count;
                // Only a thread-resource limit degrades. Real allocation errors
                // and other API failures remain failures and are fully joined.
                if (error == EAGAIN) break;
                return error == ENOMEM ? AVIF_RESULT_OUT_OF_MEMORY : AVIF_RESULT_UNKNOWN_ERROR;
            }
            ++p->count;
        }
    }
    if (p->execution) p->execution->workers_started = p->count;
    return AVIF_RESULT_OK;
}
static avifResult awjGainCancel(awjGainPool * p, avifDiagnostics * diag)
{
    if (p->cancel && p->cancel(p->userData)) {
        if (p->execution) p->execution->cancelled = AVIF_TRUE;
        avifDiagnosticsPrintf(diag, "AWJ gain map operation cancelled");
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
    return AVIF_RESULT_OK;
}
static avifBool awjGainWorkerCancel(awjGainWorker * w, size_t pixel)
{
    if (((pixel - w->begin) & 255) == 0 && w->pool->cancel && w->pool->cancel(w->pool->userData)) {
        w->cancelled = AVIF_TRUE;
        w->result = AVIF_RESULT_UNKNOWN_ERROR;
        return AVIF_TRUE;
    }
    return AVIF_FALSE;
}
static avifResult awjGainRun(awjGainPool * p, size_t begin, size_t count, awjGainTask task, void * context, avifDiagnostics * diag)
{
    AVIF_CHECKRES(awjGainCancel(p, diag));
    if (p->initialized) awjGainLock(p);
    p->task = task;
    p->context = context;
    size_t cursor = begin;
    for (uint32_t k = 0; k < p->count; ++k) {
        awjGainWorker * w = &p->workers[k];
        w->begin = cursor;
        cursor += count / p->count + (k < count % p->count ? 1 : 0);
        w->end = cursor;
        w->result = AVIF_RESULT_OK;
        w->cancelled = AVIF_FALSE;
        avifDiagnosticsClearError(&w->diag);
    }
    if (p->initialized) {
        p->remaining = p->count - 1;
        ++p->epoch;
        awjGainWake(p, AVIF_FALSE);
        awjGainUnlock(p);
    }
    task(&p->workers[0], context);
    if (p->initialized) {
        awjGainLock(p);
        while (p->remaining) awjGainWait(p, AVIF_TRUE);
        awjGainUnlock(p);
    }
    // Stable worker-order diagnostics; real errors take precedence over cancel.
    avifBool cancelled = AVIF_FALSE;
    for (uint32_t k = 0; k < p->count; ++k) {
        awjGainWorker * w = &p->workers[k];
        cancelled |= w->cancelled;
        if (w->result != AVIF_RESULT_OK && !w->cancelled) {
            avifDiagnosticsPrintf(diag, "%s", w->diag.error);
            return w->result;
        }
    }
    if (cancelled) {
        if (p->execution) p->execution->cancelled = AVIF_TRUE;
        avifDiagnosticsPrintf(diag, "AWJ gain map operation cancelled");
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
    return awjGainCancel(p, diag);
}

static avifBool awjGainValidRGB(const avifRGBImage * rgb)
{
    avifRGBColorSpaceInfo info;
    return rgb && rgb->pixels && rgb->width && rgb->height && avifGetRGBColorSpaceInfo(rgb, &info) &&
           rgb->width <= UINT32_MAX / info.pixelBytes && rgb->rowBytes >= rgb->width * info.pixelBytes &&
           rgb->height <= PTRDIFF_MAX / rgb->rowBytes &&
           (info.channelBytes == 1 || (((uintptr_t)rgb->pixels | rgb->rowBytes) & 1) == 0);
}
static avifBool awjGainValidYUV(const avifImage * image)
{
    if (!image || !image->width || !image->height ||
        (image->depth != 8 && image->depth != 10 && image->depth != 12 && image->depth != 16) ||
        image->yuvFormat <= AVIF_PIXEL_FORMAT_NONE || image->yuvFormat >= AVIF_PIXEL_FORMAT_COUNT) return AVIF_FALSE;
    const uint32_t bytes = image->depth > 8 ? 2 : 1;
    for (int c = 0; c < (image->yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? 1 : 3); ++c) {
        const uint32_t width = avifImagePlaneWidth(image, c), height = avifImagePlaneHeight(image, c);
        if (!width || !height || !image->yuvPlanes[c] || width > UINT32_MAX / bytes || image->yuvRowBytes[c] < width * bytes ||
            height > PTRDIFF_MAX / image->yuvRowBytes[c] ||
            (bytes == 2 && (((uintptr_t)image->yuvPlanes[c] | image->yuvRowBytes[c]) & 1))) return AVIF_FALSE;
    }
    if (image->alphaPlane &&
        (image->width > UINT32_MAX / bytes || image->alphaRowBytes < image->width * bytes ||
         image->height > PTRDIFF_MAX / image->alphaRowBytes ||
         (bytes == 2 && (((uintptr_t)image->alphaPlane | image->alphaRowBytes) & 1)))) return AVIF_FALSE;
    return AVIF_TRUE;
}

#define AWJ_GAIN_TRY(expr) do { res = (expr); if (res != AVIF_RESULT_OK) goto cleanup; } while (0)
typedef struct awjGainApply
{
    const avifRGBImage * base;
    avifRGBImage * output;
    const avifRGBImage * map;
    avifRGBColorSpaceInfo baseInfo, outputInfo, mapInfo;
    avifTransferFunction toLinear, toGamma;
    avifBool inputConversion, outputConversion, transferConversion, clli;
    double inputMatrix[3][3], outputMatrix[3][3];
    float weight, gammaInv[3], min[3], max[3], baseOffset[3], altOffset[3];
    int mode; // 0 = exact copy, 1 = no gain (convert), 2 = gain map
} awjGainApply;

static void awjGainApplyPixels(awjGainWorker * w, void * opaque)
{
    awjGainApply * a = (awjGainApply *)opaque; // read-only context; upstream matrix API lacks const
    float rgbMaxLinear = 0.0f;
    for (size_t p = w->begin; p < w->end; ++p) {
        if (awjGainWorkerCancel(w, p)) return;
        const uint32_t j = (uint32_t)(p / a->base->width), i = (uint32_t)(p % a->base->width);
        if (a->mode == 0) {
            memcpy(a->output->pixels + (size_t)j * a->output->rowBytes + (size_t)i * a->outputInfo.pixelBytes,
                   a->base->pixels + (size_t)j * a->base->rowBytes + (size_t)i * a->baseInfo.pixelBytes,
                   a->outputInfo.pixelBytes);
            continue;
        }
        float basePixelRGBA[4];
        avifGetRGBAPixel(a->base, i, j, &a->baseInfo, basePixelRGBA);
        if (a->mode == 1) {
            if (a->transferConversion || a->inputConversion) {
                for (int c = 0; c < 3; ++c) basePixelRGBA[c] = a->toLinear(basePixelRGBA[c]);
                if (a->inputConversion) avifLinearRGBConvertColorSpace(basePixelRGBA, a->inputMatrix);
                for (int c = 0; c < 3; ++c) basePixelRGBA[c] = avifNanSafeClamp(a->toGamma(basePixelRGBA[c]));
            }
            avifSetRGBAPixel(a->output, i, j, &a->outputInfo, basePixelRGBA);
            continue;
        }
        float gainMapRGBA[4];
        avifGetRGBAPixel(a->map, i, j, &a->mapInfo, gainMapRGBA);
        float toneMappedPixelRGBA[4];
        float pixelRgbMaxLinear = 0.0f;
        for (int c = 0; c < 3; ++c) basePixelRGBA[c] = a->toLinear(basePixelRGBA[c]);
        if (a->inputConversion) avifLinearRGBConvertColorSpace(basePixelRGBA, a->inputMatrix);
        for (int c = 0; c < 3; ++c) {
            const float baseLinear = basePixelRGBA[c];
            const float gainMapValue = gainMapRGBA[c];
            const float gainMapLog2 = lerp(a->min[c], a->max[c], powf(gainMapValue, a->gammaInv[c]));
            const float toneMappedLinear = (baseLinear + a->baseOffset[c]) * exp2f(gainMapLog2 * a->weight) - a->altOffset[c];
            if (toneMappedLinear > rgbMaxLinear) rgbMaxLinear = toneMappedLinear;
            if (toneMappedLinear > pixelRgbMaxLinear) pixelRgbMaxLinear = toneMappedLinear;
            toneMappedPixelRGBA[c] = toneMappedLinear;
        }
        if (a->outputConversion) avifLinearRGBConvertColorSpace(toneMappedPixelRGBA, a->outputMatrix);
        for (int c = 0; c < 3; ++c) {
            if (isnan(toneMappedPixelRGBA[c])) {
                avifDiagnosticsPrintf(&w->diag, "Degenerate gain map parameters produce NaN at pixel (%u, %u)", i, j);
                w->result = AVIF_RESULT_INVALID_TONE_MAPPED_IMAGE;
                return;
            }
            toneMappedPixelRGBA[c] = avifNanSafeClamp(a->toGamma(toneMappedPixelRGBA[c]));
        }
        toneMappedPixelRGBA[3] = basePixelRGBA[3];
        if (a->clli) ((float *)w->scratch)[p - w->begin] = pixelRgbMaxLinear;
        avifSetRGBAPixel(a->output, i, j, &a->outputInfo, toneMappedPixelRGBA);
    }
    w->max[0] = rgbMaxLinear;
}

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
                                     awjAvifGainMapExecution * execution)
{
    avifDiagnosticsClearError(diag);
    if (execution) memset(execution, 0, sizeof(*execution));
    if (!awjGainValidRGB(baseImage) || !gainMap || !toneMappedImage || baseImage == toneMappedImage ||
        baseImage->pixels == toneMappedImage->pixels || !isfinite(hdrHeadroom) || hdrHeadroom < 0.0f) {
        avifDiagnosticsPrintf(diag, "Invalid AWJ gain map input, alias, or headroom");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    AVIF_CHECKRES(avifGainMapValidateMetadata(gainMap, diag));
    awjGainPool pool = { 0 };
    pool.cancel = cancel;
    pool.userData = user_data;
    pool.execution = execution;
    avifResult res = AVIF_RESULT_OK;
    awjGainApply a = { 0 };
    avifRGBImage output = *toneMappedImage, rgbMap = { 0 };
    avifImage * scaledMap = NULL;
    output.pixels = NULL;
    output.rowBytes = 0;
    output.width = baseImage->width;
    output.height = baseImage->height;
    avifContentLightLevelInformationBox newCLLI = { 0 };
    if (clli) newCLLI = *clli;
    a.base = baseImage;
    a.output = &output;
    a.map = &rgbMap;
    a.weight = avifGetGainMapWeight(hdrHeadroom, gainMap);
    if (!avifGetRGBColorSpaceInfo(baseImage, &a.baseInfo) || !avifGetRGBColorSpaceInfo(&output, &a.outputInfo)) {
        res = AVIF_RESULT_NOT_IMPLEMENTED;
        goto cleanup;
    }
    AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
    AWJ_GAIN_TRY(avifRGBImageAllocatePixels(&output));
    a.toLinear = avifTransferCharacteristicsGetGammaToLinearFunction(baseTransferCharacteristics);
    a.toGamma = avifTransferCharacteristicsGetLinearToGammaFunction(outputTransferCharacteristics);
    a.transferConversion = (baseTransferCharacteristics != outputTransferCharacteristics);
    if (a.weight == 0.0f) {
        a.inputConversion = (baseColorPrimaries != outputColorPrimaries);
        a.mode = !a.transferConversion && !a.inputConversion && baseImage->format == output.format &&
                         baseImage->depth == output.depth && baseImage->isFloat == output.isFloat ? 0 : 1;
        if (a.inputConversion && !avifColorPrimariesComputeRGBToRGBMatrix(baseColorPrimaries, outputColorPrimaries, a.inputMatrix)) {
            res = AVIF_RESULT_NOT_IMPLEMENTED;
            goto cleanup;
        }
    } else {
        a.mode = 2;
        a.clli = clli != NULL;
        if (!awjGainValidYUV(gainMap->image)) {
            res = AVIF_RESULT_INVALID_ARGUMENT;
            goto cleanup;
        }
        const avifColorPrimaries mathPrimaries =
            (gainMap->useBaseColorSpace || gainMap->altColorPrimaries == AVIF_COLOR_PRIMARIES_UNSPECIFIED)
                ? baseColorPrimaries : gainMap->altColorPrimaries;
        a.inputConversion = (baseColorPrimaries != mathPrimaries);
        a.outputConversion = (mathPrimaries != outputColorPrimaries);
        if ((a.inputConversion && !avifColorPrimariesComputeRGBToRGBMatrix(baseColorPrimaries, mathPrimaries, a.inputMatrix)) ||
            (a.outputConversion && !avifColorPrimariesComputeRGBToRGBMatrix(mathPrimaries, outputColorPrimaries, a.outputMatrix))) {
            res = AVIF_RESULT_NOT_IMPLEMENTED;
            goto cleanup;
        }
        const avifImage * mapImage = gainMap->image;
        if (mapImage->width != output.width || mapImage->height != output.height) {
            scaledMap = avifImageCreateEmpty();
            if (!scaledMap) { res = AVIF_RESULT_OUT_OF_MEMORY; goto cleanup; }
            const avifCropRect rect = { 0, 0, mapImage->width, mapImage->height };
            AWJ_GAIN_TRY(avifImageSetViewRect(scaledMap, mapImage, &rect));
            AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
            AWJ_GAIN_TRY(avifImageScale(scaledMap, output.width, output.height, diag));
            AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
            mapImage = scaledMap;
        }
        avifRGBImageSetDefaults(&rgbMap, mapImage);
        rgbMap.maxThreads = 1;
        AWJ_GAIN_TRY(avifRGBImageAllocatePixels(&rgbMap));
        AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
        AWJ_GAIN_TRY(avifImageYUVToRGB(mapImage, &rgbMap));
        AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
        if (!avifGetRGBColorSpaceInfo(&rgbMap, &a.mapInfo)) { res = AVIF_RESULT_NOT_IMPLEMENTED; goto cleanup; }
        for (int c = 0; c < 3; ++c) {
            a.gammaInv[c] = 1.0f / avifUnsignedFractionToFloat(gainMap->gainMapGamma[c]);
            a.min[c] = avifSignedFractionToFloat(gainMap->gainMapMin[c]);
            a.max[c] = avifSignedFractionToFloat(gainMap->gainMapMax[c]);
            a.baseOffset[c] = avifSignedFractionToFloat(gainMap->baseOffset[c]);
            a.altOffset[c] = avifSignedFractionToFloat(gainMap->alternateOffset[c]);
        }
    }
    const size_t numPixels = (size_t)output.width * output.height;
    AWJ_GAIN_TRY(awjGainPoolInit(&pool, numPixels, max_threads, a.clli ? sizeof(float) * AWJ_GAIN_BLOCK : 0));
    float rgbMaxLinear = 0.0f, rgbSumLinear = 0.0f;
    if (a.clli) {
        // A wave is contiguous in raster order; worker partitions are contiguous
        // too. Add each original float once, in upstream order, on the caller.
        // This deliberately does NOT sum per-thread/row/block partial sums.
        for (size_t start = 0; start < numPixels;) {
            const size_t count = AVIF_MIN(numPixels - start, (size_t)pool.count * AWJ_GAIN_BLOCK);
            AWJ_GAIN_TRY(awjGainRun(&pool, start, count, awjGainApplyPixels, &a, diag));
            for (uint32_t k = 0; k < pool.count; ++k) {
                const awjGainWorker * w = &pool.workers[k];
                if (w->max[0] > rgbMaxLinear) rgbMaxLinear = w->max[0];
                for (size_t p = 0; p < w->end - w->begin; ++p) rgbSumLinear += ((const float *)w->scratch)[p];
            }
            start += count;
        }
        newCLLI.maxCLL = (uint16_t)AVIF_CLAMP(avifRoundf(rgbMaxLinear * SDR_WHITE_NITS), 0.0f, (float)UINT16_MAX);
        const float rgbAverageLinear = rgbSumLinear / numPixels;
        newCLLI.maxPALL = (uint16_t)AVIF_CLAMP(avifRoundf(rgbAverageLinear * SDR_WHITE_NITS), 0.0f, (float)UINT16_MAX);
    } else {
        AWJ_GAIN_TRY(awjGainRun(&pool, 0, numPixels, awjGainApplyPixels, &a, diag));
    }
    AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
cleanup:
    awjGainPoolDestroy(&pool);
    avifRGBImageFreePixels(&rgbMap);
    if (scaledMap) avifImageDestroy(scaledMap);
    if (res == AVIF_RESULT_OK) {
        avifRGBImageFreePixels(toneMappedImage);
        *toneMappedImage = output;
        output.pixels = NULL;
        if (clli) *clli = newCLLI;
    }
    avifRGBImageFreePixels(&output);
    return res;
}

typedef enum awjGainPhase
{
    AWJ_GAIN_OFFSET, AWJ_GAIN_RAW, AWJ_GAIN_FLIP, AWJ_GAIN_MINMAX,
    AWJ_GAIN_HISTOGRAM, AWJ_GAIN_NORMALIZE, AWJ_GAIN_WRITE
} awjGainPhase;
typedef struct awjGainCompute
{
    const avifRGBImage * base;
    const avifRGBImage * alt;
    avifRGBImage * output;
    avifRGBColorSpaceInfo baseInfo, altInfo, outputInfo;
    avifTransferFunction baseToLinear, altToLinear;
    avifBool colorSpacesDiffer, useBase, singleChannel;
    double matrix[3][3];
    float yCoeffs[3], baseOffset[3], altOffset[3];
    float * values[3];
    float min[3], max[3], gamma[3];
    int channels, channel, numBuckets;
    awjGainPhase phase;
} awjGainCompute;

static void awjGainComputePixels(awjGainWorker * w, void * opaque)
{
    awjGainCompute * a = (awjGainCompute *)opaque; // read-only context; upstream matrix API lacks const
    for (int c = 0; c < 3; ++c) { w->min[c] = 0.0f; w->max[c] = 1.0f; }
    if (a->phase == AWJ_GAIN_MINMAX) {
        w->min[0] = FLT_MAX;
        w->max[0] = -FLT_MAX;
    }
    if (a->phase == AWJ_GAIN_HISTOGRAM) memset(w->scratch, 0, sizeof(uint64_t) * a->numBuckets);
    for (size_t p = w->begin; p < w->end; ++p) {
        if (awjGainWorkerCancel(w, p)) return;
        if (a->phase == AWJ_GAIN_FLIP) {
            for (int c = 0; c < a->channels; ++c) a->values[c][p] *= -1.f;
        } else if (a->phase == AWJ_GAIN_MINMAX) {
            w->min[0] = AVIF_MIN(w->min[0], a->values[a->channel][p]);
            w->max[0] = AVIF_MAX(w->max[0], a->values[a->channel][p]);
        } else if (a->phase == AWJ_GAIN_HISTOGRAM) {
            const int c = a->channel;
            ++((uint64_t *)w->scratch)[avifValueToBucketIdx(a->values[c][p], a->min[c], a->max[c], a->numBuckets)];
        } else if (a->phase == AWJ_GAIN_NORMALIZE) {
            for (int c = 0; c < a->channels; ++c) {
                const float range = AVIF_MAX(a->max[c] - a->min[c], 0.0f);
                if (range == 0.0f) {
                    a->values[c][p] = 0.0f;
                } else {
                    float v = a->values[c][p];
                    v = AVIF_CLAMP(v, a->min[c], a->max[c]);
                    v = powf((v - a->min[c]) / range, a->gamma[c]);
                    a->values[c][p] = avifNanSafeClamp(v);
                }
            }
        } else {
            const uint32_t j = (uint32_t)(p / a->base->width), i = (uint32_t)(p % a->base->width);
            if (a->phase == AWJ_GAIN_WRITE) {
                const float r = a->values[0][p];
                const float g = a->singleChannel ? r : a->values[1][p];
                const float b = a->singleChannel ? r : a->values[2][p];
                const float rgba[4] = { r, g, b, 1.0f };
                avifSetRGBAPixel(a->output, i, j, &a->outputInfo, rgba);
            } else if (a->phase == AWJ_GAIN_OFFSET) {
                float rgba[4];
                avifGetRGBAPixel(a->useBase ? a->alt : a->base, i, j, a->useBase ? &a->altInfo : &a->baseInfo, rgba);
                for (int c = 0; c < 3; ++c) rgba[c] = a->useBase ? a->altToLinear(rgba[c]) : a->baseToLinear(rgba[c]);
                avifLinearRGBConvertColorSpace(rgba, a->matrix);
                for (int c = 0; c < 3; ++c) {
                    if (!isfinite(rgba[c])) goto invalid;
                    w->min[c] = AVIF_MIN(w->min[c], rgba[c]);
                }
            } else {
                float baseRGBA[4], altRGBA[4];
                avifGetRGBAPixel(a->base, i, j, &a->baseInfo, baseRGBA);
                avifGetRGBAPixel(a->alt, i, j, &a->altInfo, altRGBA);
                for (int c = 0; c < 3; ++c) {
                    baseRGBA[c] = a->baseToLinear(baseRGBA[c]);
                    altRGBA[c] = a->altToLinear(altRGBA[c]);
                }
                if (a->colorSpacesDiffer) avifLinearRGBConvertColorSpace(a->useBase ? altRGBA : baseRGBA, a->matrix);
                for (int c = 0; c < a->channels; ++c) {
                    float base = baseRGBA[c], alt = altRGBA[c];
                    if (a->singleChannel) {
                        base = a->yCoeffs[0] * baseRGBA[0] + a->yCoeffs[1] * baseRGBA[1] + a->yCoeffs[2] * baseRGBA[2];
                        alt = a->yCoeffs[0] * altRGBA[0] + a->yCoeffs[1] * altRGBA[1] + a->yCoeffs[2] * altRGBA[2];
                    }
                    if (!isfinite(base) || !isfinite(alt)) goto invalid;
                    if (base > w->max[0]) w->max[0] = base;
                    if (alt > w->max[1]) w->max[1] = alt;
                    const float ratio = (alt + a->altOffset[c]) / (base + a->baseOffset[c]);
                    const float ratioLog2 = log2f(AVIF_MAX(ratio, kEpsilon));
                    if (!isfinite(ratioLog2)) goto invalid;
                    a->values[c][p] = ratioLog2;
                }
            }
        }
    }
    return;
invalid:
    avifDiagnosticsPrintf(&w->diag, "Non-finite pixels or gain ratio in AWJ gain map computation");
    w->result = AVIF_RESULT_INVALID_ARGUMENT;
}

static avifResult awjGainRange(awjGainPool * pool, awjGainCompute * a, size_t numPixels, int channel, avifDiagnostics * diag)
{
    a->channel = channel;
    a->phase = AWJ_GAIN_MINMAX;
    AVIF_CHECKRES(awjGainRun(pool, 0, numPixels, awjGainComputePixels, a, diag));
    float min = pool->workers[0].min[0], max = pool->workers[0].max[0];
    for (uint32_t k = 1; k < pool->count; ++k) {
        min = AVIF_MIN(min, pool->workers[k].min[0]);
        max = AVIF_MAX(max, pool->workers[k].max[0]);
    }
    a->min[channel] = min;
    a->max[channel] = max;
    const float bucketSize = 0.01f, maxOutliersRatio = 0.001f;
    // Preserve upstream's float threshold/rounding, widen only integer counts.
    const uint64_t maxOutliersOnEachSide = (uint64_t)avifRoundf(numPixels * maxOutliersRatio / 2.0f);
    if (max - min <= bucketSize * 2 || maxOutliersOnEachSide == 0) return AVIF_RESULT_OK;
    a->numBuckets = (int)AVIF_MIN(ceilf((max - min) / bucketSize), (float)AWJ_GAIN_BUCKETS);
    a->phase = AWJ_GAIN_HISTOGRAM;
    AVIF_CHECKRES(awjGainRun(pool, 0, numPixels, awjGainComputePixels, a, diag));
    uint64_t * histogram = (uint64_t *)pool->workers[0].scratch;
    for (uint32_t k = 1; k < pool->count; ++k) {
        const uint64_t * local = (const uint64_t *)pool->workers[k].scratch;
        for (int i = 0; i < a->numBuckets; ++i) histogram[i] += local[i];
    }
    uint64_t leftOutliers = 0;
    for (int i = 0; i < a->numBuckets; ++i) {
        leftOutliers += histogram[i];
        if (leftOutliers > maxOutliersOnEachSide) break;
        if (histogram[i] == 0) a->min[channel] = avifBucketIdxToValue(i + 1, min, max, a->numBuckets);
    }
    uint64_t rightOutliers = 0;
    for (int i = a->numBuckets - 1; i >= 0; --i) {
        rightOutliers += histogram[i];
        if (rightOutliers > maxOutliersOnEachSide) break;
        if (histogram[i] == 0) a->max[channel] = avifBucketIdxToValue(i, min, max, a->numBuckets);
    }
    return AVIF_RESULT_OK;
}

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
                                       awjAvifGainMapExecution * execution)
{
    avifDiagnosticsClearError(diag);
    if (execution) memset(execution, 0, sizeof(*execution));
    if (!awjGainValidRGB(baseRgbImage) || !awjGainValidRGB(altRgbImage) || !gainMap || !gainMap->image ||
        baseRgbImage->width != altRgbImage->width || baseRgbImage->height != altRgbImage->height) {
        avifDiagnosticsPrintf(diag, "Invalid or mismatched AWJ gain map endpoints");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    avifImage * destination = gainMap->image;
    if (!destination->width || !destination->height ||
        (destination->depth != 8 && destination->depth != 10 && destination->depth != 12 && destination->depth != 16) ||
        destination->yuvFormat <= AVIF_PIXEL_FORMAT_NONE || destination->yuvFormat >= AVIF_PIXEL_FORMAT_COUNT) {
        avifDiagnosticsPrintf(diag, "Invalid AWJ gain map output dimensions, depth or format");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    const size_t numPixels = (size_t)baseRgbImage->width * baseRgbImage->height;
    if (numPixels > SIZE_MAX / sizeof(float)) return AVIF_RESULT_INVALID_ARGUMENT;
    awjGainPool pool = { 0 };
    pool.cancel = cancel;
    pool.userData = user_data;
    pool.execution = execution;
    avifResult res = AVIF_RESULT_OK;
    awjGainCompute a = { 0 };
    avifRGBImage rgb = { 0 };
    avifImage * image = NULL;
    avifGainMap metadata = *gainMap; // no allocations/ownership transfer in this shallow copy
    avifColorPrimaries mathPrimaries;
    a.base = baseRgbImage;
    a.alt = altRgbImage;
    a.output = &rgb;
    a.colorSpacesDiffer = (baseColorPrimaries != altColorPrimaries);
    a.singleChannel = destination->yuvFormat == AVIF_PIXEL_FORMAT_YUV400;
    a.channels = a.singleChannel ? 1 : 3;
    AWJ_GAIN_TRY(avifChooseColorSpaceForGainMapMath(baseColorPrimaries, altColorPrimaries, &mathPrimaries));
    if (!avifGetRGBColorSpaceInfo(a.base, &a.baseInfo) || !avifGetRGBColorSpaceInfo(a.alt, &a.altInfo)) {
        res = AVIF_RESULT_NOT_IMPLEMENTED;
        goto cleanup;
    }
    AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
    image = avifImageCreate(baseRgbImage->width, baseRgbImage->height, destination->depth, destination->yuvFormat);
    if (!image) { res = AVIF_RESULT_OUT_OF_MEMORY; goto cleanup; }
    image->matrixCoefficients = destination->matrixCoefficients;
    image->colorPrimaries = destination->colorPrimaries;
    image->transferCharacteristics = destination->transferCharacteristics;
    image->yuvRange = destination->yuvRange;
    image->yuvChromaSamplePosition = destination->yuvChromaSamplePosition;
    image->alphaPremultiplied = destination->alphaPremultiplied;
    metadata.image = image;
    for (int c = 0; c < a.channels; ++c) {
        a.values[c] = (float *)avifAlloc(numPixels * sizeof(float));
        if (!a.values[c]) { res = AVIF_RESULT_OUT_OF_MEMORY; goto cleanup; }
    }
    // Same one/three shared full-resolution float planes as upstream. No worker
    // owns a full image; range/histogram analysis uses fixed per-worker scratch.
    avifGainMapSetEncodingDefaults(&metadata);
    metadata.useBaseColorSpace = (mathPrimaries == baseColorPrimaries);
    a.useBase = metadata.useBaseColorSpace;
    a.baseToLinear = avifTransferCharacteristicsGetGammaToLinearFunction(baseTransferCharacteristics);
    a.altToLinear = avifTransferCharacteristicsGetGammaToLinearFunction(altTransferCharacteristics);
    avifColorPrimariesComputeYCoeffs(mathPrimaries, a.yCoeffs);
    if (a.colorSpacesDiffer && !avifColorPrimariesComputeRGBToRGBMatrix(a.useBase ? altColorPrimaries : baseColorPrimaries,
                                                                      a.useBase ? baseColorPrimaries : altColorPrimaries, a.matrix)) {
        res = AVIF_RESULT_NOT_IMPLEMENTED;
        goto cleanup;
    }
    for (int c = 0; c < 3; ++c) {
        a.baseOffset[c] = avifSignedFractionToFloat(metadata.baseOffset[c]);
        a.altOffset[c] = avifSignedFractionToFloat(metadata.alternateOffset[c]);
        a.gamma[c] = avifUnsignedFractionToFloat(metadata.gainMapGamma[c]);
    }
    AWJ_GAIN_TRY(awjGainPoolInit(&pool, numPixels, max_threads, sizeof(uint64_t) * AWJ_GAIN_BUCKETS));
    if (a.colorSpacesDiffer) {
        a.phase = AWJ_GAIN_OFFSET;
        AWJ_GAIN_TRY(awjGainRun(&pool, 0, numPixels, awjGainComputePixels, &a, diag));
        for (int c = 0; c < 3; ++c) {
            float channelMin = 0.0f;
            for (uint32_t k = 0; k < pool.count; ++k) channelMin = AVIF_MIN(channelMin, pool.workers[k].min[c]);
            if (channelMin < -kEpsilon) {
                const float maxOffset = 0.1f;
                if (a.useBase) a.altOffset[c] = AVIF_MIN(a.altOffset[c] - channelMin, maxOffset);
                else a.baseOffset[c] = AVIF_MIN(a.baseOffset[c] - channelMin, maxOffset);
            }
        }
    }
    a.phase = AWJ_GAIN_RAW;
    AWJ_GAIN_TRY(awjGainRun(&pool, 0, numPixels, awjGainComputePixels, &a, diag));
    float baseMax = 1.0f, altMax = 1.0f;
    for (uint32_t k = 0; k < pool.count; ++k) {
        baseMax = AVIF_MAX(baseMax, pool.workers[k].max[0]);
        altMax = AVIF_MAX(altMax, pool.workers[k].max[1]);
    }
    const double baseHeadroom = log2f(AVIF_MAX(baseMax, kEpsilon));
    const double alternateHeadroom = log2f(AVIF_MAX(altMax, kEpsilon));
    if (!avifDoubleToUnsignedFraction(baseHeadroom, &metadata.baseHdrHeadroom) ||
        !avifDoubleToUnsignedFraction(alternateHeadroom, &metadata.alternateHdrHeadroom)) {
        res = AVIF_RESULT_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (alternateHeadroom < baseHeadroom) {
        a.phase = AWJ_GAIN_FLIP;
        AWJ_GAIN_TRY(awjGainRun(&pool, 0, numPixels, awjGainComputePixels, &a, diag));
    }
    for (int c = 0; c < a.channels; ++c) AWJ_GAIN_TRY(awjGainRange(&pool, &a, numPixels, c, diag));
    for (int c = 0; c < 3; ++c) {
        if (!avifDoubleToSignedFraction(a.min[a.singleChannel ? 0 : c], &metadata.gainMapMin[c]) ||
            !avifDoubleToSignedFraction(a.max[a.singleChannel ? 0 : c], &metadata.gainMapMax[c]) ||
            !avifDoubleToSignedFraction(a.altOffset[c], &metadata.alternateOffset[c]) ||
            !avifDoubleToSignedFraction(a.baseOffset[c], &metadata.baseOffset[c])) {
            res = AVIF_RESULT_INVALID_ARGUMENT;
            goto cleanup;
        }
    }
    a.phase = AWJ_GAIN_NORMALIZE;
    AWJ_GAIN_TRY(awjGainRun(&pool, 0, numPixels, awjGainComputePixels, &a, diag));
    AWJ_GAIN_TRY(avifImageAllocatePlanes(image, AVIF_PLANES_YUV));
    avifRGBImageSetDefaults(&rgb, image);
    rgb.maxThreads = 1;
    AWJ_GAIN_TRY(avifRGBImageAllocatePixels(&rgb));
    if (!avifGetRGBColorSpaceInfo(&rgb, &a.outputInfo)) { res = AVIF_RESULT_NOT_IMPLEMENTED; goto cleanup; }
    a.phase = AWJ_GAIN_WRITE;
    AWJ_GAIN_TRY(awjGainRun(&pool, 0, numPixels, awjGainComputePixels, &a, diag));
    AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
    AWJ_GAIN_TRY(avifImageRGBToYUV(image, &rgb));
    AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
    if (destination->width != image->width || destination->height != image->height) {
        AWJ_GAIN_TRY(avifImageScale(image, destination->width, destination->height, diag));
        AWJ_GAIN_TRY(awjGainCancel(&pool, diag));
    }
cleanup:
    awjGainPoolDestroy(&pool);
    for (int c = 0; c < 3; ++c) avifFree(a.values[c]);
    avifRGBImageFreePixels(&rgb);
    if (res == AVIF_RESULT_OK) {
        // Commit once, after all workers joined. Preserve the image object's
        // identity, unrelated metadata and the caller's alternate description.
        destination->width = image->width;
        destination->height = image->height;
        avifImageStealPlanes(destination, image, AVIF_PLANES_ALL);
        for (int c = 0; c < 3; ++c) {
            gainMap->gainMapMin[c] = metadata.gainMapMin[c];
            gainMap->gainMapMax[c] = metadata.gainMapMax[c];
            gainMap->gainMapGamma[c] = metadata.gainMapGamma[c];
            gainMap->baseOffset[c] = metadata.baseOffset[c];
            gainMap->alternateOffset[c] = metadata.alternateOffset[c];
        }
        gainMap->baseHdrHeadroom = metadata.baseHdrHeadroom;
        gainMap->alternateHdrHeadroom = metadata.alternateHdrHeadroom;
        gainMap->useBaseColorSpace = metadata.useBaseColorSpace;
    }
    if (image) avifImageDestroy(image);
    return res;
}
#undef AWJ_GAIN_TRY
#undef AWJ_GAIN_WORKERS
#undef AWJ_GAIN_BLOCK
#undef AWJ_GAIN_BUCKETS
// AWJ_GAIN_MAP_THREADS_END
]=])

set(EXPECTED "${PRISTINE}\n${APPENDIX}")
if(NOT BEGIN_POS EQUAL -1 AND NOT CURRENT STREQUAL EXPECTED)
    message(FATAL_ERROR "libavif gain-map threads patch: damaged, duplicate, or unknown appendix")
endif()
file(READ "${ADAPTER_H}" HEADER)
string(REPLACE "\r\n" "\n" HEADER "${HEADER}")
set(OUTPUT_HEADER "${SOURCE_DIR}/include/avif/awj_gain_map.h")
if(EXISTS "${OUTPUT_HEADER}")
    file(READ "${OUTPUT_HEADER}" CURRENT_HEADER)
    string(REPLACE "\r\n" "\n" CURRENT_HEADER "${CURRENT_HEADER}")
    if(NOT CURRENT_HEADER STREQUAL HEADER)
        message(FATAL_ERROR "libavif gain-map threads patch: unknown or damaged generated ABI header")
    endif()
elseif(NOT BEGIN_POS EQUAL -1)
    message(FATAL_ERROR "libavif gain-map threads patch: patched source has no generated ABI header")
endif()
# Validate all inputs first. Never repair an incomplete/damaged prior patch.
if(NOT CURRENT STREQUAL EXPECTED)
    file(WRITE "${OUTPUT_HEADER}" "${HEADER}")
    file(WRITE "${GAINMAP_C}" "${EXPECTED}")
endif()
