vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

set(SCNLIB_PATCHES scnlib-pr-136.patch)
# 保持锁定版本，仅为 Linux/libc++ 的迭代器兼容性追加局部补丁。
if(VCPKG_TARGET_IS_LINUX)
    list(APPEND SCNLIB_PATCHES libcxx-wrap-iter.patch)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO eliaskosunen/scnlib
    REF "v${VERSION}"
    SHA512 db14d71da3c1ecb849f00ac1e334f39c532592230e950aa1009ff00ba56670cb71e33ca457fd4ac66595ff43f0dca0e42d45f672848b9cde3cba80f19ef8693f
    HEAD_REF master
    PATCHES ${SCNLIB_PATCHES}
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
      -DSCN_TESTS=OFF
      -DSCN_EXAMPLES=OFF
      -DSCN_BENCHMARKS=OFF
      -DSCN_DOCS=OFF
      -DSCN_USE_EXTERNAL_FAST_FLOAT=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/scn)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/scn"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
