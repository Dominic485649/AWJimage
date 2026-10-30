vcpkg_download_distfile(ARCHIVE
    URLS "https://storage.googleapis.com/aom-releases/libaom-3.15.2.tar.gz"
    FILENAME "libaom-3.15.2.tar.gz"
    SHA512 4ba185a642c9dd32d1ab708916a5aed1a65d3cd6d2599d632f33c83849e6702ca91528ad09a007eb39ac2723f0b10de9a7905ed26abfef42a164f13e21aacb7a
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        aom-rename-static.diff
        aom-uninitialized-pointer.diff
)

vcpkg_find_acquire_program(PERL)

if(VCPKG_TARGET_ARCHITECTURE MATCHES "^(x86|x64)$")
    vcpkg_find_acquire_program(NASM)
    set(aom_nasm_compiler "-DCMAKE_ASM_NASM_COMPILER=${NASM}")
endif()

set(aom_target_cpu "")
if(VCPKG_TARGET_IS_UWP OR (VCPKG_TARGET_IS_WINDOWS AND VCPKG_TARGET_ARCHITECTURE MATCHES "^arm"))
    set(aom_target_cpu "-DAOM_TARGET_CPU=generic")
endif()

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm" AND VCPKG_TARGET_IS_LINUX)
    set(aom_target_cpu "-DENABLE_NEON=OFF")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        ${aom_target_cpu}
        -DENABLE_DOCS=OFF
        -DENABLE_APPS=OFF
        -DENABLE_EXAMPLES=OFF
        -DENABLE_TESTDATA=OFF
        -DENABLE_TESTS=OFF
        -DENABLE_TOOLS=OFF
        -DTHREADS_PREFER_PTHREAD_FLAG=ON
        ${aom_nasm_compiler}
        "-DPERL_EXECUTABLE=${PERL}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/AOM)
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
