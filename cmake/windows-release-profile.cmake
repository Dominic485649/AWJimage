include_guard(GLOBAL)

function(awj_enable_msvc_release_options target visibility)
    if(NOT MSVC)
        return()
    endif()
    target_compile_options(${target} ${visibility}
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Gw>
        $<$<CONFIG:Release>:/GF>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${AWJ_ENABLE_X64_V3}>>:/arch:AVX2>)
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "STATIC_LIBRARY")
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/INCREMENTAL:NO>)
    endif()
    if(AVIF_RELEASE_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()

# 从生成器实际使用的目标属性导出；不执行来自回执的命令。
function(awj_write_msvc_build_profile target)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        return()
    endif()
    file(SHA256 "${CMAKE_CXX_COMPILER}" compiler_hash)
    file(SHA256 "${CMAKE_LINKER}" linker_hash)
    set(cpu x64)
    if(AWJ_ENABLE_X64_V3)
        set(cpu AVX2)
    endif()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/awj-msvc-${target}-$<CONFIG>.txt" CONTENT
"schema=1
configuration=$<CONFIG>
target=${target}
compiler_id=${CMAKE_CXX_COMPILER_ID}
compiler=${CMAKE_CXX_COMPILER}
compiler_version=${CMAKE_CXX_COMPILER_VERSION}
compiler_sha256=${compiler_hash}
linker=${CMAKE_LINKER}
linker_sha256=${linker_hash}
minimum_cpu=${cpu}
runtime=$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},MSVC_RUNTIME_LIBRARY>>
ipo=$<BOOL:$<TARGET_PROPERTY:${target},INTERPROCEDURAL_OPTIMIZATION_RELEASE>>
compile_flags=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE};$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>>
link_flags=${CMAKE_EXE_LINKER_FLAGS_RELEASE};${CMAKE_SHARED_LINKER_FLAGS_RELEASE};$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},LINK_OPTIONS>>
ipo_compile_flags=${CMAKE_CXX_COMPILE_OPTIONS_IPO}
ipo_link_flags=${CMAKE_CXX_LINK_OPTIONS_IPO}
")
endfunction()
