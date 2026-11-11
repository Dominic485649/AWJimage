/* Compile with the helper extracted from the project's actually patched write.c.
 * Only private libavif data shapes and the encode callback are stubbed here;
 * budgeting, wave dispatch, worker bodies and joins are the production C code.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef int avifBool;
typedef int avifResult;
typedef int avifItemCategory;
typedef uint32_t avifEncoderChanges;
typedef uint32_t avifAddImageFlags;
enum {
    AVIF_FALSE = 0, AVIF_TRUE = 1,
    AVIF_RESULT_OK = 0, AVIF_RESULT_UNKNOWN_ERROR = 1,
    AVIF_RESULT_NOT_IMPLEMENTED = 2, AVIF_RESULT_OUT_OF_MEMORY = 3,
    AVIF_RESULT_ENCODE_COLOR_FAILED = 4,
    AVIF_ITEM_COLOR = 0, AVIF_ITEM_ALPHA = 1,
    AVIF_CODEC_CHOICE_AOM = 1, AVIF_SAMPLE_TRANSFORM_NONE = 0,
    AVIF_ADD_IMAGE_FLAG_SINGLE = 2, AVIF_CHAN_Y = 0
};
typedef struct avifDiagnostics { char error[256]; } avifDiagnostics;
typedef struct avifImage avifImage;
typedef struct avifGainMap { avifImage * image; } avifGainMap;
struct avifImage {
    uint32_t width, height, depth;
    int yuvFormat, yuvRange, yuvChromaSamplePosition;
    int colorPrimaries, transferCharacteristics, matrixCoefficients;
    uint8_t * yuvPlanes[3];
    uint8_t * alphaPlane;
    avifGainMap * gainMap;
};
typedef struct avifEncoder avifEncoder;
typedef struct avifCodec avifCodec;
struct avifCodec {
    avifDiagnostics * diag;
    int maxThreads;
    avifResult (*encodeImage)(avifCodec *, const avifEncoder *, const avifImage *,
                             avifBool, int, int, int, avifEncoderChanges,
                             avifBool, avifAddImageFlags, void *);
};
typedef struct avifEncoderItem {
    avifCodec * codec;
    uint32_t cellIndex;
    avifItemCategory itemCategory;
    void * encodeOutput;
} avifEncoderItem;
typedef struct avifEncoderData {
    struct { uint32_t count; avifEncoderItem * item; } items;
    struct { uint32_t count; } frames;
    avifBool alphaPresent;
    int tileRowsLog2, tileColsLog2;
} avifEncoderData;
struct avifEncoder {
    avifEncoderData * data;
    int codecChoice, maxThreads, sampleTransformRecipe, extraLayerCount;
    avifDiagnostics diag;
};

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "%s (line %d)\n", message, __LINE__); exit(1); \
} } while (0)

static unsigned create_attempts, created, joined, live, allocation_attempts, freed;
static unsigned fail_create_at;
static int fail_allocation, freed_before_join;
static const avifEncoder * original_encoder;

static void avifDiagnosticsClearError(avifDiagnostics * diag) { diag->error[0] = 0; }
static void avifDiagnosticsPrintf(avifDiagnostics * diag, const char * format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(diag->error, sizeof(diag->error), format, arguments);
    va_end(arguments);
}
static void * avifAlloc(size_t bytes) {
    ++allocation_attempts;
    return fail_allocation ? NULL : malloc(bytes);
}
static void avifFree(void * pointer) {
    if (live != 0) freed_before_join = 1;
    ++freed;
    free(pointer);
}

/* Inject creation failure at an exact call, but keep real threads and joins.
 * These counters are used only by the calling thread, not by encode callbacks.
 */
#if defined(_WIN32)
static uintptr_t test_beginthreadex(void * security, unsigned stack_size,
                                  unsigned (__stdcall * start)(void *), void * arg,
                                  unsigned flags, unsigned * id) {
    uintptr_t handle;
    ++create_attempts;
    if (create_attempts == fail_create_at) return 0;
    handle = _beginthreadex(security, stack_size, start, arg, flags, id);
    if (handle != 0) { ++created; ++live; }
    return handle;
}
static DWORD test_wait(HANDLE handle, DWORD timeout) {
    DWORD result = WaitForSingleObject(handle, timeout);
    if (result == WAIT_OBJECT_0) ++joined;
    return result;
}
static BOOL test_close(HANDLE handle) {
    BOOL result = CloseHandle(handle);
    if (result) --live;
    return result;
}
#define _beginthreadex test_beginthreadex
#define WaitForSingleObject test_wait
#define CloseHandle test_close
#else
static int test_pthread_create(pthread_t * thread, const pthread_attr_t * attributes,
                               void * (*start)(void *), void * arg) {
    int result;
    ++create_attempts;
    if (create_attempts == fail_create_at) return 1;
    result = pthread_create(thread, attributes, start, arg);
    if (result == 0) { ++created; ++live; }
    return result;
}
static int test_pthread_join(pthread_t thread, void ** result_pointer) {
    int result = pthread_join(thread, result_pointer);
    if (result == 0) { ++joined; --live; }
    return result;
}
#define pthread_create test_pthread_create
#define pthread_join test_pthread_join
#endif

#include "awj_grid_wave_helpers.h"

static avifResult avifGetErrorForItemCategory(avifItemCategory category) {
    return category == AVIF_ITEM_COLOR ? AVIF_RESULT_ENCODE_COLOR_FAILED : AVIF_RESULT_UNKNOWN_ERROR;
}

#define MAX_CELLS 16
typedef struct Record {
    unsigned calls;
    int budget;
    int bad_arguments;
    int fail;
    const avifImage * expected_image;
} Record;
typedef struct Fixture {
    avifEncoder encoder;
    avifEncoderData data;
    avifEncoderItem items[MAX_CELLS * 2 + 2];
    avifCodec codecs[MAX_CELLS];
    avifImage images[MAX_CELLS];
    const avifImage * views[MAX_CELLS];
    Record records[MAX_CELLS];
    unsigned cells;
} Fixture;

static avifResult record_encode(avifCodec * codec, const avifEncoder * encoder,
                                const avifImage * image, avifBool alpha,
                                int tile_rows, int tile_cols, int quality,
                                avifEncoderChanges changes, avifBool disable_lag,
                                avifAddImageFlags flags, void * output) {
    Record * record = (Record *)output;
    ++record->calls;
    record->budget = encoder->maxThreads;
    record->bad_arguments = (codec->maxThreads != encoder->maxThreads ||
        encoder == original_encoder || encoder->data == original_encoder->data ||
        codec->diag == &original_encoder->diag || image != record->expected_image ||
        alpha != AVIF_FALSE || tile_rows != 1 || tile_cols != 2 || quality != 77 ||
        changes != 0xbeef || disable_lag != AVIF_FALSE || flags != AVIF_ADD_IMAGE_FLAG_SINGLE);
    if (record->fail) {
        avifDiagnosticsPrintf(codec->diag, "injected cell failure");
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
    return record->bad_arguments ? AVIF_RESULT_UNKNOWN_ERROR : AVIF_RESULT_OK;
}

static void setup(Fixture * fixture, int budget, unsigned cells, int metadata_items) {
    static uint8_t pixel;
    unsigned c;
    CHECK(live == 0, "previous scenario left an unjoined worker");
    memset(fixture, 0, sizeof(*fixture));
    create_attempts = created = joined = live = allocation_attempts = freed = 0;
    fail_create_at = 0; fail_allocation = freed_before_join = 0;
    fixture->cells = cells;
    fixture->encoder.data = &fixture->data;
    fixture->encoder.maxThreads = budget;
    fixture->encoder.codecChoice = AVIF_CODEC_CHOICE_AOM;
    fixture->data.items.item = fixture->items;
    fixture->data.tileRowsLog2 = 1; fixture->data.tileColsLog2 = 2;
    original_encoder = &fixture->encoder;
    for (c = 0; c < cells; ++c) {
        avifEncoderItem * item;
        CHECK(c < MAX_CELLS, "fixture cell capacity exceeded");
        if (metadata_items) ++fixture->data.items.count;
        item = &fixture->items[fixture->data.items.count++];
        item->codec = &fixture->codecs[c]; item->cellIndex = c;
        item->itemCategory = AVIF_ITEM_COLOR; item->encodeOutput = &fixture->records[c];
        fixture->codecs[c].encodeImage = record_encode;
        fixture->codecs[c].diag = &fixture->encoder.diag;
        fixture->images[c].width = fixture->images[c].height = 64;
        fixture->images[c].depth = 8; fixture->images[c].yuvFormat = 1;
        fixture->images[c].yuvPlanes[0] = &pixel;
        fixture->views[c] = &fixture->images[c];
        fixture->records[c].expected_image = &fixture->images[c];
    }
    if (metadata_items) ++fixture->data.items.count;
}

static avifResult run(Fixture * fixture) {
    avifResult result = avifAWJEncodeSimpleGridParallel(&fixture->encoder, fixture->views,
        0xbeef, AVIF_ADD_IMAGE_FLAG_SINGLE, 77);
    unsigned c;
    CHECK(live == 0 && created == joined && !freed_before_join,
          "production grid helper did not join every created worker before freeing its state");
    for (c = 0; c < fixture->cells; ++c) {
        CHECK(!fixture->records[c].bad_arguments, "worker did not receive its independent local encoder/budget/image");
        CHECK(fixture->codecs[c].diag == &fixture->encoder.diag, "worker left its codec pointing at released diagnostics");
    }
    return result;
}

static void success_case(int budget, unsigned cells, unsigned expected_threads, int metadata_items,
                         const int * expected_budgets) {
    Fixture fixture;
    unsigned c;
    setup(&fixture, budget, cells, metadata_items);
    CHECK(run(&fixture) == AVIF_RESULT_OK, "production grid wave failed");
    CHECK(fixture.encoder.maxThreads == budget, "grid helper rewrote the whole-file budget");
    CHECK(created == expected_threads && joined == expected_threads && freed == 1,
          "grid helper created/joined an unexpected number of outer workers");
    for (c = 0; c < cells; ++c) {
        CHECK(fixture.records[c].calls == 1 && fixture.records[c].budget == expected_budgets[c],
              "grid helper lost/duplicated a cell or assigned the wrong wave budget");
    }
    if (budget == 8 && cells == 9) {
        for (c = 0; c < 8; ++c) CHECK(fixture.records[c].budget == 1, "8/9 first wave must use eight one-slot cells");
        CHECK(fixture.records[8].budget == 8, "8/9 tail's only cell must receive all eight slots");
    }
    if (budget == 8 && cells == 3)
        CHECK(fixture.records[0].budget == 3 && fixture.records[1].budget == 3 && fixture.records[2].budget == 2,
              "8/3 remainder must be distributed 3,3,2");
}

int main(void) {
    Fixture fixture;
    unsigned c;
    int sum = 0;
    setup(&fixture, 8, 3, 0);
    CHECK(avifAWJCanParallelEncodeSimpleGrid(&fixture.encoder, 3, 1, 1, AVIF_ADD_IMAGE_FLAG_SINGLE, fixture.views),
          "simple opaque still grid unexpectedly lost its parallel path");
    for (c = 0; c < 3; ++c) sum += avifAWJGridWaveWorkerThreadBudget(&fixture.encoder, 3, c);
    CHECK(sum == 8 && avifAWJGridWaveWorkerThreadBudget(&fixture.encoder, 1, 0) == 8,
          "actual C helper did not conserve the complete wave budget");
    fixture.images[1].alphaPlane = fixture.images[1].yuvPlanes[0];
    CHECK(!avifAWJCanParallelEncodeSimpleGrid(&fixture.encoder, 3, 1, 1, AVIF_ADD_IMAGE_FLAG_SINGLE, fixture.views),
          "alpha grid bypassed its serial fallback");
    fixture.images[1].alphaPlane = NULL;
    fixture.encoder.extraLayerCount = 1;
    CHECK(!avifAWJCanParallelEncodeSimpleGrid(&fixture.encoder, 3, 1, 1, AVIF_ADD_IMAGE_FLAG_SINGLE, fixture.views),
          "layered grid bypassed its serial fallback");
    fixture.encoder.extraLayerCount = 0;
    {
        avifGainMap map = { &fixture.images[2] };
        fixture.images[1].gainMap = &map;
        CHECK(!avifAWJCanParallelEncodeSimpleGrid(&fixture.encoder, 3, 1, 1, AVIF_ADD_IMAGE_FLAG_SINGLE, fixture.views),
              "gain-map grid bypassed its serial fallback");
        fixture.images[1].gainMap = NULL;
    }
    setup(&fixture, 1, 2, 0);
    CHECK(avifAWJGridWaveWorkerThreadBudget(&fixture.encoder, 1, 0) == 1 &&
          run(&fixture) == AVIF_RESULT_NOT_IMPLEMENTED && created == 0 && allocation_attempts == 0,
          "one-slot budget started extra grid workers");
    setup(&fixture, 8, 0, 1);
    CHECK(run(&fixture) == AVIF_RESULT_NOT_IMPLEMENTED && created == 0 && allocation_attempts == 0,
          "no encodable items reached wave division/allocation");
    setup(&fixture, 8, 1, 1);
    CHECK(run(&fixture) == AVIF_RESULT_NOT_IMPLEMENTED && created == 0 && allocation_attempts == 0,
          "single item unexpectedly started parallel grid encoding");
    {
        const int eight_nine[] = {1, 1, 1, 1, 1, 1, 1, 1, 8};
        const int eight_three[] = {3, 3, 2};
        const int eight_eleven[] = {1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2};
        const int eight_eight[] = {1, 1, 1, 1, 1, 1, 1, 1};
        const int two_three[] = {1, 1, 2};
        success_case(8, 9, 7, 1, eight_nine);
        success_case(8, 3, 2, 0, eight_three);
        success_case(8, 11, 9, 1, eight_eleven);
        success_case(8, 8, 7, 1, eight_eight); /* trailing metadata produces an empty scan wave */
        success_case(2, 3, 1, 1, two_three);
    }
    setup(&fixture, 128, 9, 0);
    sum = 0;
    for (c = 0; c < 9; ++c) {
        const int expected = c < 2 ? 15 : 14;
        const int actual = avifAWJGridWaveWorkerThreadBudget(&fixture.encoder, 9, c);
        CHECK(actual == expected, "128/9 remainder changed"); sum += actual;
    }
    CHECK(sum == 128, "upper resolved budget was not conserved");

    setup(&fixture, 8, 9, 1); fail_allocation = 1;
    CHECK(run(&fixture) == AVIF_RESULT_OUT_OF_MEMORY && created == 0 && freed == 0,
          "allocation failure silently fell back or started workers");
    setup(&fixture, 8, 9, 1); fail_create_at = 1;
    CHECK(run(&fixture) == AVIF_RESULT_NOT_IMPLEMENTED && created == 0 && joined == 0 && freed == 1,
          "first creation failure lost the safe not-started fallback");
    for (c = 0; c < fixture.cells; ++c) CHECK(fixture.records[c].calls == 0, "first creation failure already encoded a cell");
    setup(&fixture, 8, 9, 1); fail_create_at = 3;
    CHECK(run(&fixture) == AVIF_RESULT_UNKNOWN_ERROR && created == 2 && joined == 2 && freed == 1,
          "partial creation failure did not stop/join all started workers");
    for (c = 0; c < fixture.cells; ++c)
        CHECK(fixture.records[c].calls == (c == 1 || c == 2 ? 1u : 0u), "partial creation failure ran an unstarted/caller cell");
    setup(&fixture, 8, 9, 1); fixture.records[2].fail = 1;
    CHECK(run(&fixture) == AVIF_RESULT_ENCODE_COLOR_FAILED && created == 7 && joined == 7 && freed == 1,
          "codec failure did not join the full wave or map its error");
    CHECK(fixture.records[8].calls == 0 && strstr(fixture.encoder.diag.error, "injected cell failure"),
          "codec error was lost or a later wave ran after failure");
    setup(&fixture, 8, 11, 1); fail_create_at = 9;
    CHECK(run(&fixture) == AVIF_RESULT_UNKNOWN_ERROR && created == 8 && joined == 8,
          "later partial creation failure left an earlier/current wave unjoined");
    setup(&fixture, 8, 11, 1); fail_create_at = 8;
    /* A NOT_IMPLEMENTED here would make the serial fallback re-encode cells
     * already committed by wave one. Only a globally not-started operation may
     * safely request that fallback, not merely a not-started current wave. */
    CHECK(run(&fixture) == AVIF_RESULT_UNKNOWN_ERROR && created == 7 && joined == 7,
          "later first-worker failure must not retry already encoded earlier waves serially");
    for (c = 0; c < fixture.cells; ++c)
        CHECK(fixture.records[c].calls == (c < 8 ? 1u : 0u), "later first-worker failure changed committed cell order");
    puts("grid wave budget, fallbacks, deterministic creation failures and all joins passed");
    return 0;
}
