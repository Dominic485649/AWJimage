if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required.")
endif()

set(WRITE_C "${SOURCE_DIR}/src/write.c")
if(NOT EXISTS "${WRITE_C}")
    message(FATAL_ERROR "libavif write.c not found: ${WRITE_C}")
endif()

file(READ "${WRITE_C}" CONTENT)

# 仅规范本补丁生成块的两个接缝；完整源码仍由后续严格指纹门禁核对。
function(awj_normalize_generated_boundaries OUT_VAR INPUT_TEXT)
    string(REGEX REPLACE "[\r\n]+(// AWJ_PARALLEL_GRID_PATCH_BEGIN)" "\n\\1" TEXT "${INPUT_TEXT}")
    string(REGEX REPLACE "(    // Encode AV1 OBUs)[\r\n]+(    // AWJ_PARALLEL_GRID_LOOP_PATCH_BEGIN)"
        "\\1\n\n\\2" TEXT "${TEXT}")
    set(${OUT_VAR} "${TEXT}" PARENT_SCOPE)
endfunction()

function(awj_remove_marked_block OUT_VAR INPUT_TEXT BEGIN_MARKER END_MARKER)
    set(TEXT "${INPUT_TEXT}")
    string(FIND "${TEXT}" "${BEGIN_MARKER}" BEGIN_POS)
    if(BEGIN_POS EQUAL -1)
        set(${OUT_VAR} "${TEXT}" PARENT_SCOPE)
        return()
    endif()

    string(FIND "${TEXT}" "${END_MARKER}" END_POS)
    if(END_POS EQUAL -1 OR END_POS LESS BEGIN_POS)
        message(FATAL_ERROR "Could not remove stale libavif AWJ patch block: end marker missing.")
    endif()
    string(LENGTH "${END_MARKER}" END_MARKER_LENGTH)
    math(EXPR AFTER_END "${END_POS}+${END_MARKER_LENGTH}")
    string(SUBSTRING "${TEXT}" 0 ${BEGIN_POS} PREFIX)
    string(SUBSTRING "${TEXT}" ${AFTER_END} -1 SUFFIX)
    set(${OUT_VAR} "${PREFIX}${SUFFIX}" PARENT_SCOPE)
endfunction()

function(awj_replace_after_anchor OUT_VAR INPUT_TEXT ANCHOR NEEDLE REPLACEMENT FAILURE_MESSAGE)
    set(TEXT "${INPUT_TEXT}")
    string(FIND "${TEXT}" "${ANCHOR}" ANCHOR_POS)
    if(ANCHOR_POS EQUAL -1)
        message(FATAL_ERROR "${FAILURE_MESSAGE}")
    endif()
    string(SUBSTRING "${TEXT}" ${ANCHOR_POS} -1 ANCHOR_TAIL)
    string(FIND "${ANCHOR_TAIL}" "${NEEDLE}" NEEDLE_RELATIVE_POS)
    if(NEEDLE_RELATIVE_POS EQUAL -1)
        message(FATAL_ERROR "${FAILURE_MESSAGE}")
    endif()
    math(EXPR NEEDLE_POS "${ANCHOR_POS}+${NEEDLE_RELATIVE_POS}")
    string(LENGTH "${NEEDLE}" NEEDLE_LENGTH)
    math(EXPR AFTER_NEEDLE "${NEEDLE_POS}+${NEEDLE_LENGTH}")
    string(SUBSTRING "${TEXT}" 0 ${NEEDLE_POS} PREFIX)
    string(SUBSTRING "${TEXT}" ${AFTER_NEEDLE} -1 SUFFIX)
    set(${OUT_VAR} "${PREFIX}${REPLACEMENT}${SUFFIX}" PARENT_SCOPE)
endfunction()

function(awj_remove_legacy_loop_patch OUT_VAR INPUT_TEXT)
    set(TEXT "${INPUT_TEXT}")
    set(OLD_LOOP_NEEDLE "    const avifBool awjUseParallelGridEncode =\n")
    string(FIND "${TEXT}" "${OLD_LOOP_NEEDLE}" LOOP_BEGIN_POS)
    if(LOOP_BEGIN_POS EQUAL -1)
        set(${OUT_VAR} "${TEXT}" PARENT_SCOPE)
        return()
    endif()

    set(FOR_NEEDLE "    for (uint32_t itemIndex = 0; itemIndex < encoder->data->items.count; ++itemIndex) {\n")
    string(SUBSTRING "${TEXT}" ${LOOP_BEGIN_POS} -1 LOOP_TAIL)
    string(FIND "${LOOP_TAIL}" "${FOR_NEEDLE}" LOOP_FOR_RELATIVE)
    if(LOOP_FOR_RELATIVE EQUAL -1)
        message(FATAL_ERROR "Could not remove stale libavif AWJ loop patch.")
    endif()

    math(EXPR LOOP_FOR_POS "${LOOP_BEGIN_POS}+${LOOP_FOR_RELATIVE}")
    string(SUBSTRING "${TEXT}" 0 ${LOOP_BEGIN_POS} PREFIX)
    string(SUBSTRING "${TEXT}" ${LOOP_FOR_POS} -1 SUFFIX)
    set(${OUT_VAR} "${PREFIX}${SUFFIX}" PARENT_SCOPE)
endfunction()

if(CONTENT MATCHES "AWJ_PARALLEL_GRID_PATCH")
    if(CONTENT MATCHES "avifEncoder localEncoder = \\*data->encoder" AND
       CONTENT MATCHES "localEncoder.maxThreads = data->perTileThreads" AND
       CONTENT MATCHES "return AVIF_RESULT_OUT_OF_MEMORY;" AND
       CONTENT MATCHES "thread creation failed before encoding workers started" AND
       CONTENT MATCHES "finalResult = AVIF_RESULT_NOT_IMPLEMENTED;" AND
       CONTENT MATCHES "avifGetErrorForItemCategory\\(avifItemCategory itemCategory\\);" AND
       CONTENT MATCHES "#define NOMINMAX" AND
       CONTENT MATCHES "AWJ_PARALLEL_GRID_LOOP_PATCH_BEGIN" AND
       CONTENT MATCHES "AWJ_PARALLEL_GRID_WAVE_BUDGET_V3")
        awj_normalize_generated_boundaries(PATCHED "${CONTENT}")
        if(NOT PATCHED STREQUAL CONTENT)
            file(WRITE "${WRITE_C}" "${PATCHED}")
        endif()
        return()
    endif()

    awj_remove_marked_block(
        CONTENT
        "${CONTENT}"
        "// AWJ_PARALLEL_GRID_PATCH_BEGIN"
        "// AWJ_PARALLEL_GRID_PATCH_END")
    awj_remove_marked_block(
        CONTENT
        "${CONTENT}"
        "    // AWJ_PARALLEL_GRID_LOOP_PATCH_BEGIN"
        "    // AWJ_PARALLEL_GRID_LOOP_PATCH_END")
    awj_remove_legacy_loop_patch(CONTENT "${CONTENT}")
endif()

set(INCLUDE_NEEDLE "#include <assert.h>\n#include <stdint.h>\n#include <string.h>\n#include <time.h>\n")
set(INCLUDE_REPLACEMENT "#include <assert.h>\n#include <stdint.h>\n#include <string.h>\n#include <time.h>\n\n#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#include <process.h>\n#include <windows.h>\n#else\n#include <pthread.h>\n#endif\n")

if(CONTENT MATCHES "#include <process[.]h>" AND
   CONTENT MATCHES "#include <windows[.]h>" AND
   NOT CONTENT MATCHES "#define NOMINMAX")
    string(REPLACE "#if defined(_WIN32)\n#include <process.h>\n#include <windows.h>"
                   "#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#include <process.h>\n#include <windows.h>"
                   PATCHED "${CONTENT}")
    if(PATCHED STREQUAL CONTENT)
        message(FATAL_ERROR "Could not update libavif write.c Windows includes for parallel grid.")
    endif()
    set(CONTENT "${PATCHED}")
elseif(NOT CONTENT MATCHES "#include <process[.]h>" AND
       NOT CONTENT MATCHES "#include <pthread[.]h>")
    string(REPLACE "${INCLUDE_NEEDLE}" "${INCLUDE_REPLACEMENT}" PATCHED "${CONTENT}")
    if(PATCHED STREQUAL CONTENT)
        message(FATAL_ERROR "Could not patch libavif write.c includes for parallel grid.")
    endif()
    set(CONTENT "${PATCHED}")
endif()

set(MARKER_NEEDLE "static avifResult avifEncoderAddImageItems(avifEncoder * encoder,\n")
set(HELPER_CODE [=[
// AWJ_PARALLEL_GRID_PATCH_BEGIN
static avifResult avifGetErrorForItemCategory(avifItemCategory itemCategory);

typedef struct avifAWJParallelGridEncodeData
{
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif
    avifEncoderItem * item;
    avifEncoder * encoder;
    const avifImage * cellImage;
    avifDiagnostics diag;
    avifEncoderChanges encoderChanges;
    avifAddImageFlags addImageFlags;
    int perTileThreads;
    int quality;
    avifResult result;
    avifBool threadCreated;
} avifAWJParallelGridEncodeData;

static avifBool avifAWJCanParallelEncodeSimpleGrid(const avifEncoder * encoder,
                                                   uint32_t gridCols,
                                                   uint32_t gridRows,
                                                   uint64_t durationInTimescales,
                                                   avifAddImageFlags addImageFlags,
                                                   const avifImage * const * cellImages)
{
    if (encoder->codecChoice != AVIF_CODEC_CHOICE_AOM) {
        return AVIF_FALSE;
    }
    if (gridCols == 0 || gridRows == 0) {
        return AVIF_FALSE;
    }
    const uint64_t cellCount64 = (uint64_t)gridCols * gridRows;
    if (cellCount64 < 2 || cellCount64 > UINT32_MAX) {
        return AVIF_FALSE;
    }
    if (encoder->maxThreads < 2 || durationInTimescales != 1 ||
        !(addImageFlags & AVIF_ADD_IMAGE_FLAG_SINGLE) ||
        encoder->sampleTransformRecipe != AVIF_SAMPLE_TRANSFORM_NONE ||
        encoder->data->frames.count != 0 || encoder->data->alphaPresent ||
        encoder->extraLayerCount != 0) {
        return AVIF_FALSE;
    }

    const avifImage * firstCell = cellImages[0];
    if (!firstCell || firstCell->alphaPlane ||
        (firstCell->gainMap && firstCell->gainMap->image) ||
        !firstCell->yuvPlanes[AVIF_CHAN_Y]) {
        return AVIF_FALSE;
    }
    const uint32_t cellCount = (uint32_t)cellCount64;
    for (uint32_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        const avifImage * cellImage = cellImages[cellIndex];
        if (!cellImage || cellImage->alphaPlane ||
            (cellImage->gainMap && cellImage->gainMap->image) ||
            !cellImage->yuvPlanes[AVIF_CHAN_Y] ||
            cellImage->width != firstCell->width ||
            cellImage->height != firstCell->height ||
            cellImage->depth != firstCell->depth ||
            cellImage->yuvFormat != firstCell->yuvFormat ||
            cellImage->yuvRange != firstCell->yuvRange ||
            cellImage->yuvChromaSamplePosition != firstCell->yuvChromaSamplePosition ||
            cellImage->colorPrimaries != firstCell->colorPrimaries ||
            cellImage->transferCharacteristics != firstCell->transferCharacteristics ||
            cellImage->matrixCoefficients != firstCell->matrixCoefficients) {
            return AVIF_FALSE;
        }
    }

    uint32_t encodableItemCount = 0;
    for (uint32_t itemIndex = 0; itemIndex < encoder->data->items.count; ++itemIndex) {
        const avifEncoderItem * item = &encoder->data->items.item[itemIndex];
        if (!item->codec) {
            continue;
        }
        if (item->itemCategory != AVIF_ITEM_COLOR || item->cellIndex >= cellCount) {
            return AVIF_FALSE;
        }
        ++encodableItemCount;
    }
    return encodableItemCount >= 2;
}

#if defined(_WIN32)
static unsigned int __stdcall avifAWJParallelGridEncodeWorker(void * arg)
#else
static void * avifAWJParallelGridEncodeWorker(void * arg)
#endif
{
    avifAWJParallelGridEncodeData * data = (avifAWJParallelGridEncodeData *)arg;
    avifDiagnosticsClearError(&data->diag);

    avifEncoder localEncoder = *data->encoder;
    avifEncoderData localEncoderData = *data->encoder->data;
    localEncoder.data = &localEncoderData;
    localEncoder.maxThreads = data->perTileThreads;
    avifDiagnosticsClearError(&localEncoder.diag);

    data->item->codec->diag = &data->diag;
    data->item->codec->maxThreads = data->perTileThreads;
    data->result = data->item->codec->encodeImage(data->item->codec,
                                                  &localEncoder,
                                                  data->cellImage,
                                                  AVIF_FALSE,
                                                  data->encoder->data->tileRowsLog2,
                                                  data->encoder->data->tileColsLog2,
                                                  data->quality,
                                                  data->encoderChanges,
                                                  AVIF_FALSE,
                                                  data->addImageFlags,
                                                  data->item->encodeOutput);
    if (data->result == AVIF_RESULT_UNKNOWN_ERROR) {
        data->result = avifGetErrorForItemCategory(data->item->itemCategory);
    }
    if (data->diag.error[0] == 0 && localEncoder.diag.error[0] != 0) {
        memcpy(data->diag.error, localEncoder.diag.error, sizeof(data->diag.error));
        data->diag.error[sizeof(data->diag.error) - 1] = 0;
    }
    data->item->codec->diag = &data->encoder->diag;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static avifBool avifAWJCreateParallelGridEncodeThread(avifAWJParallelGridEncodeData * data)
{
#if defined(_WIN32)
    data->thread = (HANDLE)_beginthreadex(/*security=*/NULL,
                                          /*stack_size=*/0,
                                          &avifAWJParallelGridEncodeWorker,
                                          data,
                                          /*initflag=*/0,
                                          /*thrdaddr=*/NULL);
    return data->thread != NULL;
#else
    return pthread_create(&data->thread, NULL, &avifAWJParallelGridEncodeWorker, data) == 0;
#endif
}

static avifBool avifAWJJoinParallelGridEncodeThread(avifAWJParallelGridEncodeData * data)
{
#if defined(_WIN32)
    const avifBool waited = WaitForSingleObject(data->thread, INFINITE) == WAIT_OBJECT_0;
    const avifBool closed = CloseHandle(data->thread) != 0;
    data->thread = NULL;
    return waited && closed;
#else
    return pthread_join(data->thread, NULL) == 0;
#endif
}

// AWJ_PARALLEL_GRID_WAVE_BUDGET_V3
// workerCount is the number of encodable cells in this wave, including the
// caller. Distribute the complete file budget, including tail waves/remainders.
static int avifAWJGridWaveWorkerThreadBudget(const avifEncoder * encoder, uint32_t workerCount, uint32_t workerIndex)
{
    const int threadsPerWorker = encoder->maxThreads / (int)workerCount;
    const uint32_t remainder = (uint32_t)encoder->maxThreads % workerCount;
    return threadsPerWorker + (workerIndex < remainder ? 1 : 0);
}

static avifResult avifAWJEncodeSimpleGridParallel(avifEncoder * encoder,
                                                  const avifImage * const * cellImages,
                                                  avifEncoderChanges encoderChanges,
                                                  avifAddImageFlags addImageFlags,
                                                  int quality)
{
    uint32_t encodableItemCount = 0;
    for (uint32_t itemIndex = 0; itemIndex < encoder->data->items.count; ++itemIndex) {
        if (encoder->data->items.item[itemIndex].codec) {
            ++encodableItemCount;
        }
    }

    uint32_t tileParallelism = encodableItemCount;
    if (tileParallelism > (uint32_t)encoder->maxThreads) {
        tileParallelism = (uint32_t)encoder->maxThreads;
    }
    if (tileParallelism < 2) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    const size_t byteCount = sizeof(avifAWJParallelGridEncodeData) * tileParallelism;
    avifAWJParallelGridEncodeData * threadData = (avifAWJParallelGridEncodeData *)avifAlloc(byteCount);
    if (!threadData) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    memset(threadData, 0, byteCount);

    uint32_t nextItemIndex = 0;
    avifResult finalResult = AVIF_RESULT_OK;
    avifBool encodingStarted = AVIF_FALSE;
    while (nextItemIndex < encoder->data->items.count) {
        uint32_t workerCount = 0;
        for (; workerCount < tileParallelism && nextItemIndex < encoder->data->items.count; ++nextItemIndex) {
            avifEncoderItem * item = &encoder->data->items.item[nextItemIndex];
            if (!item->codec) {
                continue;
            }
            avifAWJParallelGridEncodeData * data = &threadData[workerCount++];
            memset(data, 0, sizeof(*data));
            data->item = item;
            data->encoder = encoder;
            data->cellImage = cellImages[item->cellIndex];
            data->encoderChanges = encoderChanges;
            data->addImageFlags = addImageFlags;
            data->quality = quality;
        }
        if (workerCount == 0) {
            continue;
        }
        for (uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
            threadData[workerIndex].perTileThreads = avifAWJGridWaveWorkerThreadBudget(encoder, workerCount, workerIndex);
        }

        avifBool threadCreationFailed = AVIF_FALSE;
        uint32_t createdWorkerCount = 1; // worker 0 runs on the caller thread.
        for (uint32_t workerIndex = 1; workerIndex < workerCount; ++workerIndex) {
            threadData[workerIndex].threadCreated = avifAWJCreateParallelGridEncodeThread(&threadData[workerIndex]);
            if (!threadData[workerIndex].threadCreated) {
                threadCreationFailed = AVIF_TRUE;
                break;
            }
            ++createdWorkerCount;
            encodingStarted = AVIF_TRUE;
        }

        if (threadCreationFailed) {
            for (uint32_t workerIndex = 1; workerIndex < createdWorkerCount; ++workerIndex) {
                if (threadData[workerIndex].threadCreated && !avifAWJJoinParallelGridEncodeThread(&threadData[workerIndex])) {
                    avifDiagnosticsPrintf(&encoder->diag, "AWJ parallel grid worker join failed after thread creation failure");
                }
            }
            if (createdWorkerCount == 1 && !encodingStarted) {
                avifDiagnosticsPrintf(&encoder->diag, "AWJ parallel grid thread creation failed before encoding workers started");
                finalResult = AVIF_RESULT_NOT_IMPLEMENTED;
            } else {
                avifDiagnosticsPrintf(&encoder->diag, "AWJ parallel grid thread creation failed after encoding workers started");
                finalResult = AVIF_RESULT_UNKNOWN_ERROR;
            }
            break;
        }

        encodingStarted = AVIF_TRUE;
        avifAWJParallelGridEncodeWorker(&threadData[0]);
        for (uint32_t workerIndex = 1; workerIndex < workerCount; ++workerIndex) {
            if (threadData[workerIndex].threadCreated && !avifAWJJoinParallelGridEncodeThread(&threadData[workerIndex])) {
                avifDiagnosticsPrintf(&encoder->diag, "AWJ parallel grid worker join failed");
                finalResult = AVIF_RESULT_UNKNOWN_ERROR;
            }
        }
        for (uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
            if (threadData[workerIndex].result != AVIF_RESULT_OK && finalResult == AVIF_RESULT_OK) {
                finalResult = threadData[workerIndex].result;
                if (threadData[workerIndex].diag.error[0] != 0) {
                    memcpy(encoder->diag.error, threadData[workerIndex].diag.error, sizeof(encoder->diag.error));
                    encoder->diag.error[sizeof(encoder->diag.error) - 1] = 0;
                }
            }
        }
        if (finalResult != AVIF_RESULT_OK) {
            break;
        }
    }

    avifFree(threadData);
    return finalResult;
}
// AWJ_PARALLEL_GRID_PATCH_END

]=])

string(REPLACE "${MARKER_NEEDLE}" "${HELPER_CODE}${MARKER_NEEDLE}" PATCHED "${CONTENT}")
if(PATCHED STREQUAL CONTENT)
    message(FATAL_ERROR "Could not insert libavif parallel grid helpers.")
endif()
set(CONTENT "${PATCHED}")

set(LOOP_ANCHOR [=[
    // -----------------------------------------------------------------------
    // Encode AV1 OBUs

]=])
set(LOOP_NEEDLE [=[
    for (uint32_t itemIndex = 0; itemIndex < encoder->data->items.count; ++itemIndex) {
]=])
set(LOOP_REPLACEMENT [=[
    // AWJ_PARALLEL_GRID_LOOP_PATCH_BEGIN
    const avifBool awjUseParallelGridEncode =
        avifAWJCanParallelEncodeSimpleGrid(encoder, gridCols, gridRows, durationInTimescales, addImageFlags, cellImages);
    if (awjUseParallelGridEncode) {
        const avifResult parallelEncodeResult =
            avifAWJEncodeSimpleGridParallel(encoder, cellImages, encoderChanges, addImageFlags, encoder->data->quality);
        if (parallelEncodeResult == AVIF_RESULT_OK) {
            avifEncoderFrame * frame = (avifEncoderFrame *)avifArrayPush(&encoder->data->frames);
            AVIF_CHECKERR(frame != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            frame->durationInTimescales = durationInTimescales;
            avifCodecSpecificOptionsClear(encoder->csOptions);
            return AVIF_RESULT_OK;
        }
        if (parallelEncodeResult != AVIF_RESULT_NOT_IMPLEMENTED) {
            AVIF_CHECKRES(parallelEncodeResult);
        }
    }
    // AWJ_PARALLEL_GRID_LOOP_PATCH_END

    for (uint32_t itemIndex = 0; itemIndex < encoder->data->items.count; ++itemIndex) {
]=])

awj_replace_after_anchor(
    PATCHED
    "${CONTENT}"
    "${LOOP_ANCHOR}"
    "${LOOP_NEEDLE}"
    "${LOOP_REPLACEMENT}"
    "Could not patch libavif AV1 OBU encode loop.")

awj_normalize_generated_boundaries(PATCHED "${PATCHED}")
file(WRITE "${WRITE_C}" "${PATCHED}")
