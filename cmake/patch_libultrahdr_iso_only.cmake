# Keep the pinned libultrahdr engine, but make its decoder ISO 21496-1 only.
# UHDR_WRITE_XMP controls encoding, not this decoder fallback. Match the complete
# function so a future upstream change (including a partly patched tree) fails
# closed. The independent CPU-budget patch can be applied in either order.
if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
set(JPEGR_CPP "${SOURCE_DIR}/lib/src/jpegr.cpp")
file(READ "${JPEGR_CPP}" CONTENT)
string(REPLACE "\r\n" "\n" CONTENT "${CONTENT}")
set(ORIGINAL [=[uhdr_error_info_t UltraHdr::parseGainMapMetadata(uint8_t* iso_data, size_t iso_size, uint8_t* xmp_data,
                                                 size_t xmp_size, uint8_t* exif_data, int exif_size,
                                                 uhdr_gainmap_metadata_ext_t* uhdr_metadata) {
  if (iso_size > 0) {
    if (iso_size < kIsoNameSpace.size() + 1) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "iso block size needs to be atleast %zd but got %zd", kIsoNameSpace.size() + 1,
               iso_size);
      return status;
    }
    uhdr_gainmap_metadata_frac decodedMetadata;
    std::vector<uint8_t> iso_vec;
    for (size_t i = kIsoNameSpace.size() + 1; i < iso_size; i++) {
      iso_vec.push_back(iso_data[i]);
    }

    UHDR_ERR_CHECK(uhdr_gainmap_metadata_frac::decodeGainmapMetadata(iso_vec, &decodedMetadata));
    UHDR_ERR_CHECK(uhdr_gainmap_metadata_frac::gainmapMetadataFractionToFloat(&decodedMetadata,
                                                                              uhdr_metadata));
  } else if (xmp_size > 0) {
    UHDR_ERR_CHECK(getMetadataFromXMP(xmp_data, xmp_size, exif_data, exif_size, uhdr_metadata));
  } else {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "received no valid buffer to parse gainmap metadata");
    return status;
  }

  return g_no_error;
}]=])
set(PATCHED [=[uhdr_error_info_t UltraHdr::parseGainMapMetadata(uint8_t* iso_data, size_t iso_size, uint8_t* xmp_data,
                                                 size_t xmp_size, uint8_t* exif_data, int exif_size,
                                                 uhdr_gainmap_metadata_ext_t* uhdr_metadata) {
  // AWJ ISO 21496-1 only: legacy XMP/Exif is never a metadata fallback.
  (void)xmp_data;
  (void)xmp_size;
  (void)exif_data;
  (void)exif_size;
  if (iso_size > 0) {
    if (iso_size < kIsoNameSpace.size() + 1) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "iso block size needs to be atleast %zd but got %zd", kIsoNameSpace.size() + 1,
               iso_size);
      return status;
    }
    uhdr_gainmap_metadata_frac decodedMetadata;
    std::vector<uint8_t> iso_vec;
    for (size_t i = kIsoNameSpace.size() + 1; i < iso_size; i++) {
      iso_vec.push_back(iso_data[i]);
    }

    UHDR_ERR_CHECK(uhdr_gainmap_metadata_frac::decodeGainmapMetadata(iso_vec, &decodedMetadata));
    UHDR_ERR_CHECK(uhdr_gainmap_metadata_frac::gainmapMetadataFractionToFloat(&decodedMetadata,
                                                                              uhdr_metadata));
  } else {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_INVALID_PARAM;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "ISO 21496-1 gain map metadata is required; legacy XMP gain maps are unsupported");
    return status;
  }

  return g_no_error;
}]=])

# Count literal occurrences without treating C++ punctuation as a regex or list.
function(count_literal HAYSTACK NEEDLE OUTPUT)
    string(LENGTH "${HAYSTACK}" BEFORE)
    string(REPLACE "${NEEDLE}" "" REST "${HAYSTACK}")
    string(LENGTH "${REST}" AFTER)
    string(LENGTH "${NEEDLE}" SIZE)
    math(EXPR COUNT "(${BEFORE} - ${AFTER}) / ${SIZE}")
    set(${OUTPUT} "${COUNT}" PARENT_SCOPE)
endfunction()
count_literal("${CONTENT}" "${ORIGINAL}" ORIGINAL_COUNT)
count_literal("${CONTENT}" "${PATCHED}" PATCHED_COUNT)
count_literal("${CONTENT}" "UltraHdr::parseGainMapMetadata(" FUNCTION_COUNT)
string(REGEX MATCHALL "getMetadataFromXMP[ \t\r\n]*\\(" XMP_CALLS "${CONTENT}")
list(LENGTH XMP_CALLS XMP_CALL_COUNT)
if(ORIGINAL_COUNT EQUAL 1 AND PATCHED_COUNT EQUAL 0 AND FUNCTION_COUNT EQUAL 1 AND XMP_CALL_COUNT EQUAL 1)
    string(REPLACE "${ORIGINAL}" "${PATCHED}" CONTENT "${CONTENT}")
    file(WRITE "${JPEGR_CPP}" "${CONTENT}")
elseif(NOT (ORIGINAL_COUNT EQUAL 0 AND PATCHED_COUNT EQUAL 1 AND FUNCTION_COUNT EQUAL 1 AND XMP_CALL_COUNT EQUAL 0))
    message(FATAL_ERROR "libultrahdr ISO-only patch anchor mismatch; review the locked upstream source")
endif()
