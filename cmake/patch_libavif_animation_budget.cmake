if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
set(WRITE_C "${SOURCE_DIR}/src/write.c")
set(API_H "${SOURCE_DIR}/include/avif/avif.h")
file(READ "${WRITE_C}" CONTENT)
if(NOT CONTENT MATCHES "awjAvifEncoderRetainedBytes")
    set(ANCHOR "typedef struct avifEncoderItem\n{\n")
    string(FIND "${CONTENT}" "${ANCHOR}" POS)
    if(POS EQUAL -1)
        message(FATAL_ERROR "libavif animation budget patch needs upstream review: encoder item changed")
    endif()
    string(REPLACE "${ANCHOR}" "${ANCHOR}    uint32_t awjAccountedSamples;\n    size_t awjSampleBytes;\n" CONTENT "${CONTENT}")
    string(APPEND CONTENT [=[

// AWJ: count retained packets before Finish() (ioStats is only filled by Finish).
size_t awjAvifEncoderRetainedBytes(avifEncoder * encoder)
{
    if (!encoder || !encoder->data) return 0;
    size_t total = 0;
    for (uint32_t i = 0; i < encoder->data->items.count; ++i) {
        avifEncoderItem * item = &encoder->data->items.item[i];
        size_t bytes = item->metadataPayload.size;
        if (item->encodeOutput) {
            const avifEncodeSampleArray * samples = &item->encodeOutput->samples;
            for (; item->awjAccountedSamples < samples->count; ++item->awjAccountedSamples) {
                size_t added = samples->sample[item->awjAccountedSamples].data.size;
                if (added > SIZE_MAX - item->awjSampleBytes) return SIZE_MAX;
                item->awjSampleBytes += added;
            }
            if (item->awjSampleBytes > SIZE_MAX - bytes) return SIZE_MAX;
            bytes += item->awjSampleBytes;
            if ((size_t)samples->capacity > (SIZE_MAX - bytes) / sizeof(avifEncodeSample)) return SIZE_MAX;
            bytes += (size_t)samples->capacity * sizeof(avifEncodeSample);
        }
        if (bytes > SIZE_MAX - total) return SIZE_MAX;
        total += bytes;
    }
    return total;
}
]=])
    file(WRITE "${WRITE_C}" "${CONTENT}")
endif()
file(READ "${API_H}" CONTENT)
if(NOT CONTENT MATCHES "awjAvifEncoderRetainedBytes")
    set(ANCHOR "AVIF_API size_t avifEncoderGetGainMapSizeBytes(avifEncoder * encoder);")
    string(FIND "${CONTENT}" "${ANCHOR}" POS)
    if(POS EQUAL -1)
        message(FATAL_ERROR "libavif animation budget patch needs upstream review: API changed")
    endif()
    string(REPLACE "${ANCHOR}" "${ANCHOR}\nAVIF_API size_t awjAvifEncoderRetainedBytes(avifEncoder * encoder);" CONTENT "${CONTENT}")
    file(WRITE "${API_H}" "${CONTENT}")
endif()
