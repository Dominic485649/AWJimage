# No configure/build or dependency mutation: every patch runs in a fresh sandbox.
if(NOT DEFINED TEST_DIR OR NOT DEFINED PATCH_SCRIPT OR NOT DEFINED THREAD_PATCH_SCRIPT)
    message(FATAL_ERROR "TEST_DIR, PATCH_SCRIPT and THREAD_PATCH_SCRIPT are required")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef NONCE)
set(SANDBOX "${TEST_DIR}/iso-contract-${NONCE}")
if(EXISTS "${SANDBOX}")
    message(FATAL_ERROR "Patch test sandbox already exists")
endif()
file(MAKE_DIRECTORY "${SANDBOX}")
# Pinned v2.0.2 function, independently recorded as the pristine test fixture.
set(PRISTINE [=[uhdr_error_info_t UltraHdr::parseGainMapMetadata(uint8_t* iso_data, size_t iso_size, uint8_t* xmp_data,
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
set(CPU [=[unsigned int GetCPUCoreCount() { return (std::max)(1u, std::thread::hardware_concurrency()); }]=])
function(fixture NAME CONTENT)
    file(MAKE_DIRECTORY "${SANDBOX}/${NAME}/lib/src")
    file(WRITE "${SANDBOX}/${NAME}/lib/src/jpegr.cpp" "${CONTENT}")
endfunction()
function(apply NAME SCRIPT)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SANDBOX}/${NAME}" -P "${SCRIPT}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "Patch test ${NAME} failed: ${OUTPUT}${ERROR}")
    endif()
endfunction()
function(reject NAME CONTENT)
    fixture("${NAME}" "${CONTENT}")
    file(SHA256 "${SANDBOX}/${NAME}/lib/src/jpegr.cpp" BEFORE)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SANDBOX}/${NAME}" -P "${PATCH_SCRIPT}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
    file(SHA256 "${SANDBOX}/${NAME}/lib/src/jpegr.cpp" AFTER)
    if(RESULT EQUAL 0 OR NOT ERROR MATCHES "ISO-only patch anchor mismatch" OR NOT BEFORE STREQUAL AFTER)
        message(FATAL_ERROR "Patch accepted or changed unknown source ${NAME}: ${OUTPUT}${ERROR}")
    endif()
endfunction()

fixture(first "${CPU}\n${PRISTINE}\n")
apply(first "${PATCH_SCRIPT}")
file(READ "${SANDBOX}/first/lib/src/jpegr.cpp" PATCHED)
foreach(KEPT IN ITEMS "decodeGainmapMetadata(iso_vec, &decodedMetadata)"
        "gainmapMetadataFractionToFloat(&decodedMetadata," "if (iso_size > 0)"
        "ISO 21496-1 gain map metadata is required; legacy XMP gain maps are unsupported")
    string(FIND "${PATCHED}" "${KEPT}" POS)
    if(POS EQUAL -1)
        message(FATAL_ERROR "ISO-only patch lost required behavior: ${KEPT}")
    endif()
endforeach()
if(PATCHED MATCHES "getMetadataFromXMP|else if \\(xmp_size")
    message(FATAL_ERROR "ISO-only patch retained a legacy decoder fallback")
endif()
file(SHA256 "${SANDBOX}/first/lib/src/jpegr.cpp" FIRST_HASH)
apply(first "${PATCH_SCRIPT}")
file(SHA256 "${SANDBOX}/first/lib/src/jpegr.cpp" REPEAT_HASH)
if(NOT FIRST_HASH STREQUAL REPEAT_HASH)
    message(FATAL_ERROR "ISO-only patch is not idempotent")
endif()

reject(unknown "// incompatible upstream source\n")
reject(marker_only "// AWJ ISO 21496-1 only\n")
reject(duplicate "${PRISTINE}\n${PRISTINE}\n")
string(REPLACE "if (iso_size > 0)" "if (iso_size > 1)" BAD "${PRISTINE}")
reject(changed_iso "${BAD}")
string(REPLACE "else if (xmp_size > 0)" "else if (xmp_size > 1)" BAD "${PRISTINE}")
reject(changed_fallback "${BAD}")
string(REPLACE "if (iso_size > 0)" "if (iso_size > 1)" BAD "${PATCHED}")
reject(damaged_patch "${BAD}")
reject(stray_fallback "${PATCHED}\ngetMetadataFromXMP(data, size);\n")

# The old CPU-budget patch and new ISO-only patch must compose in either order.
fixture(iso_first "${CPU}\n${PRISTINE}\n")
apply(iso_first "${PATCH_SCRIPT}")
apply(iso_first "${THREAD_PATCH_SCRIPT}")
apply(iso_first "${PATCH_SCRIPT}")
apply(iso_first "${THREAD_PATCH_SCRIPT}")
fixture(threads_first "${CPU}\n${PRISTINE}\n")
apply(threads_first "${THREAD_PATCH_SCRIPT}")
apply(threads_first "${PATCH_SCRIPT}")
file(SHA256 "${SANDBOX}/iso_first/lib/src/jpegr.cpp" ISO_FIRST_HASH)
file(SHA256 "${SANDBOX}/threads_first/lib/src/jpegr.cpp" THREADS_FIRST_HASH)
if(NOT ISO_FIRST_HASH STREQUAL THREADS_FIRST_HASH)
    message(FATAL_ERROR "ISO and thread patches do not compose")
endif()

# If supplied, also patch only a copy of the actual populated dependency.
if(DEFINED LIBULTRAHDR_SOURCE_DIR)
    set(REAL_CPP "${LIBULTRAHDR_SOURCE_DIR}/lib/src/jpegr.cpp")
    file(SHA256 "${REAL_CPP}" REAL_BEFORE)
    file(READ "${REAL_CPP}" REAL_SOURCE)
    fixture(actual "${REAL_SOURCE}")
    apply(actual "${PATCH_SCRIPT}")
    apply(actual "${THREAD_PATCH_SCRIPT}")
    apply(actual "${PATCH_SCRIPT}")
    file(SHA256 "${REAL_CPP}" REAL_AFTER)
    if(NOT REAL_BEFORE STREQUAL REAL_AFTER)
        message(FATAL_ERROR "Patch contract changed the actual dependency")
    endif()
endif()
file(READ "${PATCH_SCRIPT}" PATCH_TEXT)
string(REPLACE "\r\n" "\n" PATCH_TEXT "${PATCH_TEXT}")
string(SHA256 PATCH_HASH "${PATCH_TEXT}")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../cmake/codec-dependencies.json" LOCK)
string(JSON EXPECTED_HASH GET "${LOCK}" local_build_sha256 "cmake/patch_libultrahdr_iso_only.cmake")
if(NOT PATCH_HASH STREQUAL EXPECTED_HASH)
    message(FATAL_ERROR "Normalized ISO-only patch hash differs from the dependency lock")
endif()
message(STATUS "Normalized ISO-only patch SHA-256: ${PATCH_HASH}")
message(STATUS "ISO-only patch contract passed: first, repeat, 7 fail-closed cases, thread composition, isolated real-source copy")
