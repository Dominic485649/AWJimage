if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
set(header "${SOURCE_DIR}/api/cpp/include/private/slint_models.h")
file(READ "${header}" content)
if(NOT content MATCHES "AWJ_FILTER_INDEX_FIX")
    set(before [=[        if (added_accepted_rows.empty()) {
            return;
        }

        auto insertion_point = std::lower_bound(accepted_rows.begin(), accepted_rows.end(), index);]=])
    set(after [=[        // AWJ_FILTER_INDEX_FIX: rejected insertions still shift source indices.
        auto insertion_point = std::lower_bound(accepted_rows.begin(), accepted_rows.end(), index);
        if (added_accepted_rows.empty()) {
            for (auto it = insertion_point; it != accepted_rows.end(); ++it) *it += count;
            return;
        }]=])
    string(FIND "${content}" "${before}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Slint FilterModel insertion patch does not match")
    endif()
    string(REPLACE "${before}" "${after}" content "${content}")
    string(REPLACE
        "(mapped_row_start != accepted_rows.end() && *mapped_row_start == index)"
        "(mapped_removed_len > 0)" content "${content}")
    file(WRITE "${header}" "${content}")
endif()
if(NOT content MATCHES "AWJ_FILTER_MAPPING_NOTIFY")
    string(REPLACE
        "for (auto it = insertion_point; it != accepted_rows.end(); ++it) *it += count;"
        "for (auto it = insertion_point; it != accepted_rows.end(); ++it) *it += count;\n            target_model.notify_reset(); // AWJ_FILTER_MAPPING_NOTIFY"
        content "${content}")
    string(REPLACE [=[        if (mapped_removed_index) {
            target_model.notify_row_removed(*mapped_removed_index, mapped_removed_len);
        }]=] [=[        // Source indices also change when only rejected rows are removed.
        target_model.notify_reset();]=] content "${content}")
    file(WRITE "${header}" "${content}")
endif()
