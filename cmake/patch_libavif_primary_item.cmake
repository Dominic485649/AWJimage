# Private adapter for libavif v1.4.2, commit c5240fc79fe5c2407e10afd35f5505ef6333ea49.
# These complete-file pins cover the parser structures and static helpers used
# below. Existing AWJ encoder patches touch write.c/avif.h, not either input.
# Accept only pristine input or exactly pristine input plus this exact appendix.
# Do not repair, strip, or silently accept an unknown/partially patched decoder.
if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "libavif primary-item patch: SOURCE_DIR is required")
endif()
set(_awj_read_c "${SOURCE_DIR}/src/read.c")
set(_awj_internal_h "${SOURCE_DIR}/include/avif/internal.h")
foreach(_awj_input IN ITEMS "${_awj_read_c}" "${_awj_internal_h}")
    if(NOT EXISTS "${_awj_input}" OR IS_DIRECTORY "${_awj_input}")
        message(FATAL_ERROR "libavif primary-item patch: missing input ${_awj_input}")
    endif()
endforeach()
# 与项目锁一致，仅规范化 CRLF 为 LF；指纹仍覆盖完整固定版本源码。
set(_awj_read_sha256 "e1cdbe4ecd139a2d4aff7eea927a48465753aea768d40e30550601ca72e65cdc")
set(_awj_internal_sha256 "3b23c7a4a4d83ed9b6b03531b0b205675e71e07284d7e39d01531f94c12189ce")
file(READ "${_awj_internal_h}" _awj_internal)
string(REPLACE "\r\n" "\n" _awj_internal "${_awj_internal}")
string(SHA256 _awj_actual_internal_sha256 "${_awj_internal}")
if(NOT _awj_actual_internal_sha256 STREQUAL _awj_internal_sha256)
    message(FATAL_ERROR "libavif primary-item patch: internal.h differs from pinned v1.4.2; upstream review required")
endif()

set(_awj_implementation [=[
// AWJ_AVIF_PRIMARY_ITEM_PATCH_BEGIN
#include <stdlib.h> // qsort; only temporary, project-owned copies are sorted.

static int awjAvifPrimaryExtentCompare(const void * lhs, const void * rhs)
{
    const avifExtent * a = (const avifExtent *)lhs;
    const avifExtent * b = (const avifExtent *)rhs;
    if (a->offset != b->offset) {
        return (a->offset < b->offset) ? -1 : 1;
    }
    return (a->size < b->size) ? -1 : (a->size > b->size);
}

static int awjAvifPrimarySampleCompare(const void * lhs, const void * rhs)
{
    const avifDecodeSample * a = (const avifDecodeSample *)lhs;
    const avifDecodeSample * b = (const avifDecodeSample *)rhs;
    if (a->offset != b->offset) {
        return (a->offset < b->offset) ? -1 : 1;
    }
    return (a->size < b->size) ? -1 : (a->size > b->size);
}

// Preserve iloc payload order when coalescing consecutive extents. Sorting only
// afterwards must not make reordered extents look like one shared coded sample.
static avifBool awjAvifPrimaryBuildRuns(const avifDecoderItem * item,
                                       uint64_t sourceSize,
                                       avifExtent ** outRuns,
                                       uint32_t * outCount)
{
    *outRuns = NULL;
    *outCount = 0;
    if (!sourceSize || !item->size || !item->extents.count || !item->extents.extent ||
        SIZE_MAX / item->extents.count < sizeof(avifExtent)) {
        return AVIF_FALSE;
    }
    avifExtent * runs = (avifExtent *)avifAlloc((size_t)item->extents.count * sizeof(avifExtent));
    if (!runs) {
        return AVIF_FALSE;
    }
    size_t total = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < item->extents.count; ++i) {
        const avifExtent * extent = &item->extents.extent[i];
        if (extent->size > UINT64_MAX - extent->offset || extent->offset > sourceSize ||
            extent->size > sourceSize - extent->offset || extent->size > SIZE_MAX - total) {
            goto error;
        }
        total += extent->size;
        if (!extent->size) {
            continue;
        }
        if (count && extent->offset == runs[count - 1].offset + runs[count - 1].size) {
            if (extent->size > SIZE_MAX - runs[count - 1].size) {
                goto error;
            }
            runs[count - 1].size += extent->size;
        } else {
            runs[count++] = *extent;
        }
    }
    if (!count || total != item->size) {
        goto error;
    }
    qsort(runs, count, sizeof(avifExtent), awjAvifPrimaryExtentCompare);
    for (uint32_t i = 1; i < count; ++i) {
        // Reused/overlapping iloc extents are not a safely identifiable cover.
        if (runs[i].offset < runs[i - 1].offset + runs[i - 1].size) {
            goto error;
        }
    }
    *outRuns = runs;
    *outCount = count;
    return AVIF_TRUE;
error:
    avifFree(runs);
    return AVIF_FALSE;
}

// Validate the already parsed stsc/stsz association before asking libavif to
// materialize samples. In particular, do not interpret samples of a different
// description as the selected color codec. No BMFF bytes are reparsed here.
static avifBool awjAvifPrimarySampleCount(const avifSampleTable * table,
                                         avifCodecType codecType,
                                         uint32_t limit,
                                         uint32_t * outCount)
{
    if (!limit || !table->chunks.count || !table->sampleToChunks.count ||
        table->sampleToChunks.count > INT_MAX || !table->sampleToChunks.sampleToChunk ||
        !table->sampleDescriptions.count || !table->sampleDescriptions.description) {
        return AVIF_FALSE;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->sampleToChunks.count; ++i) {
        const avifSampleTableSampleToChunk * entry = &table->sampleToChunks.sampleToChunk[i];
        const uint64_t nextChunk = (i + 1 < table->sampleToChunks.count)
                                       ? table->sampleToChunks.sampleToChunk[i + 1].firstChunk
                                       : (uint64_t)table->chunks.count + 1;
        if (!entry->firstChunk || (i == 0 && entry->firstChunk != 1) ||
            entry->firstChunk > table->chunks.count || nextChunk <= entry->firstChunk ||
            nextChunk > (uint64_t)table->chunks.count + 1 || !entry->samplesPerChunk ||
            !entry->sampleDescriptionIndex || entry->sampleDescriptionIndex > table->sampleDescriptions.count ||
            avifGetCodecType(table->sampleDescriptions.description[entry->sampleDescriptionIndex - 1].format) != codecType) {
            return AVIF_FALSE;
        }
        const uint64_t chunkCount = nextChunk - entry->firstChunk;
        if (chunkCount > (limit - count) / entry->samplesPerChunk) {
            return AVIF_FALSE;
        }
        count += (uint32_t)(chunkCount * entry->samplesPerChunk);
    }
    if (!count || SIZE_MAX / count < sizeof(avifDecodeSample) ||
        (!table->allSamplesSize && count != table->sampleSizes.count)) {
        return AVIF_FALSE;
    }
    *outCount = count;
    return AVIF_TRUE;
}

#ifdef __cplusplus
extern "C" {
#endif
// Also a prototype for C builds using -Wmissing-prototypes. The declaration for
// callers lives in AWJimage's adapter header, not in upstream avif.h.
int awjAvifHasIndependentPrimary(const avifDecoder * decoder);

int awjAvifHasIndependentPrimary(const avifDecoder * decoder)
{
    // There is no public/internal parse-success flag: a successful Parse() is a
    // caller precondition. These checks reject obvious unparsed/invalid handles.
    if (!decoder || !decoder->data || !decoder->data->meta || !decoder->io ||
        !decoder->io->read || !decoder->image || decoder->imageCount <= 0) {
        return -1;
    }
    const avifDecoderData * data = decoder->data;
    if (!data->meta->primaryItemID) {
        return 0; // A pure sequence is not required to declare a primary item.
    }
    const avifDecoderItem * primary = avifMetaFindColorItem(data->meta);
    if (!primary || primary->meta != data->meta || primary->auxForID || primary->thumbnailForID ||
        primary->descForID || avifPropertyArrayFind(&primary->properties, "auxC")) {
        // A declared but unusable primary is not the same as an absent primary.
        return -1;
    }
    if (primary->dimgForID) {
        // ISO Gain Map base is also the first input of its alternate tmap item.
        // That functional role must not turn a displayable primary into a tile.
        const avifDecoderItem * derived = NULL;
        for (uint32_t i = 0; i < data->meta->items.count; ++i) {
            if (data->meta->items.item[i]->id == primary->dimgForID) {
                derived = data->meta->items.item[i];
                break;
            }
        }
        if (!derived || memcmp(derived->type, "tmap", 4) || primary->dimgIdx != 0) {
            return -1;
        }
    }

    avifExtent * runs = NULL;
    uint32_t runCount = 0;
    avifCodecDecodeInput * input = NULL;
    int independent = -1;
    avifDiagnostics diag;
    const avifTrack * colorTrack = NULL;
    avifCodecType colorCodecType = AVIF_CODEC_TYPE_UNKNOWN;
    uint32_t sampleCount = 0;
    avifBool shared = AVIF_FALSE;
    uint32_t runIndex = 0;
    uint32_t sampleIndex = 0;
    const uint64_t sourceSize = primary->idatStored ? (uint64_t)data->meta->idat.size : decoder->io->sizeHint;
    if ((primary->idatStored && !data->meta->idat.data) ||
        !awjAvifPrimaryBuildRuns(primary, sourceSize, &runs, &runCount)) {
        return -1;
    }

    // In this pinned decoder grid is the supported displayable derived primary.
    // Its payload is layout metadata, not a color track sample. Keep this static
    // candidate, including grids with independent tile content; PRIMARY_ITEM must
    // validate the dimg graph, layout and tiles. Unsupported derived types fail
    // avifMetaFindColorItem above, never silently become animation.
    if (!memcmp(primary->type, "grid", 4)) {
        independent = 1;
        goto cleanup;
    }
    if (avifGetCodecType(primary->type) != AVIF_CODEC_TYPE_AV1 || primary->hasDimgFrom) {
        goto cleanup;
    }
    avifDiagnosticsClearError(&diag);
    if (avifDecoderItemValidateProperties(primary, "av1C", &diag, decoder->strictFlags) != AVIF_RESULT_OK) {
        goto cleanup;
    }

    // Exactly the same first eligible color track as SOURCE_TRACKS: no handler
    // or brand heuristics, and no use of imageSequenceTrackPresent as evidence.
    for (uint32_t i = 0; i < data->tracks.count; ++i) {
        const avifTrack * track = &data->tracks.track[i];
        if (!track->sampleTable || !track->id || !track->sampleTable->chunks.count) {
            continue;
        }
        const avifCodecType type = avifSampleTableGetCodecType(track->sampleTable);
        if (type == AVIF_CODEC_TYPE_UNKNOWN || track->auxForID != 0) {
            continue;
        }
        colorTrack = track;
        colorCodecType = type;
        break;
    }
    if (!colorTrack) {
        independent = 1; // No displayable color sequence with which to share data.
        goto cleanup;
    }
    // idat offsets are relative to a copied buffer; its original absolute file
    // position is not retained. Never compare that coordinate space to stco/co64.
    // A finite caller frame limit and I/O bound are required for materialization.
    if (primary->idatStored || !decoder->io->sizeHint) {
        goto cleanup;
    }
    if (!awjAvifPrimarySampleCount(colorTrack->sampleTable, colorCodecType, decoder->imageCountLimit, &sampleCount)) {
        goto cleanup;
    }
    input = avifCodecDecodeInputCreate();
    if (!input) {
        goto cleanup;
    }
    if (avifCodecDecodeInputFillFromSampleTable(input,
                                               colorTrack->sampleTable,
                                               decoder->imageCountLimit,
                                               decoder->io->sizeHint,
                                               &diag) != AVIF_RESULT_OK ||
        input->samples.count != sampleCount) {
        goto cleanup;
    }
    for (uint32_t i = 0; i < sampleCount; ++i) {
        const avifDecodeSample * sample = &input->samples.sample[i];
        if (sample->itemID || !sample->size || sample->size > UINT64_MAX - sample->offset ||
            sample->offset > decoder->io->sizeHint || sample->size > decoder->io->sizeHint - sample->offset) {
            goto cleanup;
        }
    }
    qsort(input->samples.sample, sampleCount, sizeof(avifDecodeSample), awjAvifPrimarySampleCompare);
    for (uint32_t i = 1; i < sampleCount; ++i) {
        const avifDecodeSample * previous = &input->samples.sample[i - 1];
        const avifDecodeSample * sample = &input->samples.sample[i];
        // Repeated references to the same whole sample are harmless. Partial
        // overlap or a different size at the same offset is ambiguous, not cover.
        if (sample->offset < previous->offset + previous->size &&
            (sample->offset != previous->offset || sample->size != previous->size)) {
            goto cleanup;
        }
    }

    while (runIndex < runCount && sampleIndex < sampleCount) {
        const avifExtent * run = &runs[runIndex];
        const avifDecodeSample * sample = &input->samples.sample[sampleIndex];
        const uint64_t runEnd = run->offset + run->size; // Already checked above.
        const uint64_t sampleEnd = sample->offset + sample->size;
        if (runEnd <= sample->offset) {
            ++runIndex;
        } else if (sampleEnd <= run->offset) {
            ++sampleIndex;
        } else if (run->offset == sample->offset && run->size == sample->size) {
            shared = AVIF_TRUE;
            ++sampleIndex;
            // Keep this run for any duplicate reference to the same sample.
        } else {
            // Overlap alone does not prove that a malformed/fragmented primary
            // is a cover. In particular, never discard it by falling back to a
            // sequence when only a prefix/subrange matches a coded sample.
            goto cleanup;
        }
    }
    independent = shared ? 0 : 1;
cleanup:
    if (input) {
        avifCodecDecodeInputDestroy(input);
    }
    avifFree(runs);
    return independent;
}
#ifdef __cplusplus
} // extern "C"
#endif
// AWJ_AVIF_PRIMARY_ITEM_PATCH_END
]=])
# A leading separator is part of the exact appendix, including repeat checks.
set(_awj_appendix "\n${_awj_implementation}")
file(READ "${_awj_read_c}" _awj_read)
string(REPLACE "\r\n" "\n" _awj_read "${_awj_read}")
string(SHA256 _awj_actual_read_sha256 "${_awj_read}")
if(_awj_actual_read_sha256 STREQUAL _awj_read_sha256)
    string(APPEND _awj_read "${_awj_appendix}")
    file(WRITE "${_awj_read_c}" "${_awj_read}")
else()
    string(LENGTH "${_awj_read}" _awj_read_length)
    string(LENGTH "${_awj_appendix}" _awj_appendix_length)
    if(_awj_read_length GREATER_EQUAL _awj_appendix_length)
        math(EXPR _awj_prefix_length "${_awj_read_length} - ${_awj_appendix_length}")
        string(SUBSTRING "${_awj_read}" ${_awj_prefix_length} -1 _awj_tail)
        string(SUBSTRING "${_awj_read}" 0 ${_awj_prefix_length} _awj_prefix)
        string(SHA256 _awj_prefix_sha256 "${_awj_prefix}")
        if(_awj_tail STREQUAL _awj_appendix AND _awj_prefix_sha256 STREQUAL _awj_read_sha256)
            return() # Exact repeat: do not rewrite the file or change its mtime.
        endif()
    endif()
    message(FATAL_ERROR "libavif primary-item patch: read.c differs from pinned v1.4.2 or has an unknown/partial patch; upstream review required")
endif()
