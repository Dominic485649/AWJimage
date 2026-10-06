if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
set(CONTEXT_CC "${SOURCE_DIR}/libheif/context.cc")
file(READ "${CONTEXT_CC}" CONTENT)
if(NOT CONTENT MATCHES "AWJ_TMAP_BASE_HANDLE")
    set(ANCHOR "  if (!m_primary_image) {\n    return Error(heif_error_Invalid_input,")
    string(FIND "${CONTENT}" "${ANCHOR}" POS)
    if(POS EQUAL -1)
        message(FATAL_ERROR "libheif tmap patch requires review: primary item path changed")
    endif()
    set(PATCH [=[
  // AWJ_TMAP_BASE_HANDLE: expose the coded base for AWJ's separate ISO compositor.
  // The file's original pitm and item references remain intact.
  if (!m_primary_image) {
    const auto infe = m_heif_file->get_infe_box(m_heif_file->get_primary_image_ID());
    const auto iref = m_heif_file->get_iref_box();
    if (infe && infe->get_item_type_4cc() == fourcc("tmap") && iref) {
      const auto refs = iref->get_references(infe->get_item_ID(), fourcc("dimg"));
      if (refs.size() == 2 && refs[0] != refs[1] && m_all_images.count(refs[0]) && m_all_images.count(refs[1])) {
        m_primary_image = m_all_images.at(refs[0]);
        m_primary_image->set_primary(true);
      }
    }
  }

]=])
    string(REPLACE "${ANCHOR}" "${PATCH}${ANCHOR}" CONTENT "${CONTENT}")
    file(WRITE "${CONTEXT_CC}" "${CONTENT}")
endif()
set(PROPERTIES_CC "${SOURCE_DIR}/libheif/api/libheif/heif_properties.cc")
file(READ "${PROPERTIES_CC}" CONTENT)
if(NOT CONTENT MATCHES "awjHeifItemNclx")
    string(APPEND CONTENT [=[

// AWJ: inspect colr on a derived tmap item, which has no public image handle.
// 1 = nclx, 0 = absent, -1 = invalid/ICC (unsupported by the ISO compositor).
extern "C" LIBHEIF_API int awjHeifItemNclx(const heif_context * ctx, heif_item_id id,
                                         int * primaries, int * transfer, int * matrix)
{
    if (!ctx || !primaries || !transfer || !matrix) return -1;
    std::vector<std::shared_ptr<Box>> properties;
    if (ctx->context->get_heif_file()->get_properties(id, properties)) return -1;
    int found = 0;
    for (const auto& prop : properties) {
        auto colr = std::dynamic_pointer_cast<Box_colr>(prop);
        if (!colr) continue;
        auto nclx = std::dynamic_pointer_cast<const color_profile_nclx>(colr->get_color_profile());
        if (!nclx || found) return -1;
        const auto profile = nclx->get_nclx_color_profile();
        *primaries = profile.m_colour_primaries;
        *transfer = profile.m_transfer_characteristics;
        *matrix = profile.m_matrix_coefficients;
        found = 1;
    }
    return found;
}
]=])
    file(WRITE "${PROPERTIES_CC}" "${CONTENT}")
endif()
