if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(_registry "${SOURCE_DIR}/libheif/plugin_registry.cc")
set(_plugins_cmake "${SOURCE_DIR}/libheif/plugins/CMakeLists.txt")

file(READ "${_registry}" _registry_text)
set(_mask_include "#include \"plugins/encoder_mask.h\"\n")
set(_mask_register "  register_encoder(get_encoder_plugin_mask());\n")
string(REPLACE "${_mask_include}" "" _registry_patched "${_registry_text}")
string(REPLACE "${_mask_register}" "" _registry_patched "${_registry_patched}")
if(_registry_patched STREQUAL _registry_text AND
   (_registry_text MATCHES "encoder_mask" OR _registry_text MATCHES "get_encoder_plugin_mask"))
    message(FATAL_ERROR "libheif mask encoder registry layout changed")
endif()
file(WRITE "${_registry}" "${_registry_patched}")

file(READ "${_plugins_cmake}" _plugins_text)
set(_mask_sources [=[target_sources(heif PRIVATE
               encoder_mask.h
               encoder_mask.cc
               nalu_utils.h
               nalu_utils.cc)]=])
set(_decoder_sources [=[target_sources(heif PRIVATE
               nalu_utils.h
               nalu_utils.cc)]=])
string(REPLACE "${_mask_sources}" "${_decoder_sources}" _plugins_patched "${_plugins_text}")
if(_plugins_patched STREQUAL _plugins_text AND _plugins_text MATCHES "encoder_mask.cc")
    message(FATAL_ERROR "libheif mask encoder source layout changed")
endif()
file(WRITE "${_plugins_cmake}" "${_plugins_patched}")
