cmake_minimum_required(VERSION 3.30)
# Pure profile/metadata negative tests. All payloads, JUnit and logs below are
# explicitly synthetic; this is NOT a formal SDK, compiler, codec or device test.
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
include("${repo}/cmake/sdk-release-profiles.cmake")
if(NOT TEST_ROOT OR NOT IS_ABSOLUTE "${TEST_ROOT}")
    message(FATAL_ERROR "指定全新的绝对 TEST_ROOT=build/release-1.3.1/20261101/profile-tests-*")
endif()
get_filename_component(parent "${TEST_ROOT}" DIRECTORY)
get_filename_component(leaf "${TEST_ROOT}" NAME)
file(REAL_PATH "${parent}" parent)
file(REAL_PATH "${repo}/build/release-1.3.1/20261101" allowed_parent)
if(NOT IS_DIRECTORY "${parent}" OR NOT parent STREQUAL allowed_parent OR NOT leaf MATCHES "^profile-tests-[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "profile 测试根必须是本项目批准的隔离目录")
endif()
set(TEST_ROOT "${parent}/${leaf}")

if(NOT CASE)
    if(EXISTS "${TEST_ROOT}")
        message(FATAL_ERROR "测试根必须全新；保留已有验证记录，不覆盖")
    endif()
    file(MAKE_DIRECTORY "${TEST_ROOT}")
    file(WRITE "${TEST_ROOT}/FIXTURE-ONLY.txt" "Synthetic profile/metadata contract fixtures only. No SDK release, codec build, native consumer execution, or device verification.\n")
    set(cases
        "positive-profiles|PASS"
        "msvc-positive|PASS"
        "msvc-clang-banner|MSVC"
        "msvc-incomplete-banner|MSVC"
        "msvc-legacy-escape|TARGET_PROFILE"
        "msvc-wrong-id|compiler_id"
        "msvc-wrong-hash|compiler_sha256"
        "msvc-ipo-disabled|ipo"
        "msvc-no-gl|优化降档"
        "msvc-no-ltcg|优化降档"
        "msvc-dynamic-runtime|runtime"
        "msvc-override-optimization|优化降档"
        "positive-hmos-target-alias|PASS"
        "positive-metadata|PASS"
        "positive-junit|PASS"
        "positive-junit-selfclosing|PASS"
        "positive-target-evidence|PASS"
        "positive-binary-metadata|PASS"
        "positive-native-link-map|PASS"
        "profile-missing|TARGET_PROFILE"
        "profile-future-missing|TARGET_PROFILE"
        "profile-unknown|TARGET_PROFILE"
        "profile-legacy-escape|TARGET_PROFILE"
        "profile-wrong-host|host"
        "profile-hmos-windows|host"
        "profile-wrong-arch|x64 host"
        "compiler-old|Clang"
        "compiler-incomplete|Clang"
        "proof-wrong-profile|profile"
        "proof-wrong-target|target_triple"
        "proof-wrong-compiler|compiler_version"
        "proof-wrong-compiler-hash|compiler_sha256"
        "proof-wrong-source|source_fingerprint"
        "proof-wrong-library|library_sha256"
        "proof-wrong-report|test_report_sha256"
        "proof-dirty|source_dirty"
        "proof-native-disabled|native_codecs"
        "proof-encoder-missing|五编码器"
        "proof-encoder-duplicate|五编码器"
        "proof-hmos-shm|features"
        "proof-hmos-gpu|gpu"
        "hmos-wrong-sdk|native_sdk_version"
        "hmos-wrong-api|api"
        "hmos-api-string|api"
        "hmos-wrong-libcxx|libcxx_version"
        "hmos-wrong-namespace|libcxx_abi_namespace"
        "hmos-wrong-abi|libcxx_abi_version"
        "hmos-dynamic-runtime|runtime_linkage"
        "junit-only-two|缺少必需测试"
        "junit-missing-worker|sdk_codec_worker_failure"
        "junit-missing-portable|sdk_portable_stdlib_contract"
        "junit-missing-d3d|sdk_d3d11_shared"
        "junit-missing-stream|sdk_stream_cli"
        "junit-skipped-counter|跳过"
        "junit-hidden-skip|跳过"
        "junit-multiline-skip|跳过"
        "junit-disabled-attribute|禁用"
        "junit-hidden-failure|失败"
        "junit-status-notrun|不是 run"
        "junit-duplicate|重复"
        "junit-count-lie|计数不一致"
        "junit-unclosed|未闭合"
        "junit-nested|嵌套"
        "junit-entity|不安全 XML"
        "execution-missing|缺少真实目标执行证据"
        "execution-bare-passed|schema"
        "execution-unbound|sdk_test_log"
        "execution-wrong-source|source_fingerprint"
        "execution-wrong-library|library_sha256"
        "execution-wrong-sdk-report|test_report_sha256"
        "execution-wrong-consumer|consumers"
        "execution-consumer-notrun|exit_code"
        "execution-host-target|system_name"
        "execution-wrong-api|api"
        "execution-missing-build|system_build"
        "execution-loader-wrong-library|loaded_library_sha256"
        "execution-loader-wrong-version|version"
        "execution-loader-wrong-abi|abi"
        "execution-host-runtime|host_runtime_loaded"
        "execution-loader-unresolved|unresolved_symbols"
        "execution-raw-log-tamper|文件摘要不匹配"
        "execution-forged-loader-line|绑定事件"
        "execution-consumer-skipped|跳过"
        "execution-path-traversal|附件路径"
        "execution-path-absolute|附件路径"
        "hash-report-mismatch|文件摘要不匹配"
        "hash-uppercase|SHA256"
        "tree-sensitive|非分发成员"
        "tree-hidden-sensitive|非分发成员"
        "tree-static-archive|非分发成员"
        "tree-ffmpeg-runtime|非分发成员"
        "path-outside|路径越过"
        "binary-wrong-arch|架构"
        "binary-not-shared|shared object"
        "binary-wrong-soname|SONAME"
        "binary-not-image|不是 ELF"
        "binary-host-runtime|闭包依赖"
        "binary-ffmpeg-runtime|闭包依赖"
        "binary-absolute-rpath|运行路径"
        "binary-consumer-unlinked|没有加载本次 SDK"
        "link-map-missing-static|静态闭包成员"
        "link-map-host-unwind|替代/experimental/宿主运行库"
        "link-map-experimental|运行库"
    )
    set(passed 0)
    set(summary "FIXTURE-ONLY: no formal SDK/codec/device acceptance is claimed.\n")
    foreach(spec IN LISTS cases)
        string(REPLACE "|" ";" spec "${spec}")
        list(GET spec 0 name)
        list(GET spec 1 expected)
        execute_process(COMMAND "${CMAKE_COMMAND}" "-DTEST_ROOT=${TEST_ROOT}" "-DCASE=${name}" -P "${CMAKE_CURRENT_LIST_FILE}"
            RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 30)
        file(WRITE "${TEST_ROOT}/${name}.log" "FIXTURE ONLY\nexit=${result}\n${stdout}${stderr}")
        if(expected STREQUAL "PASS")
            if(NOT result EQUAL 0)
                message(FATAL_ERROR "正向 contract fixture 失败：${name}\n${stdout}${stderr}")
            endif()
        elseif(result EQUAL 0 OR NOT "${stdout}${stderr}" MATCHES "${expected}")
            message(FATAL_ERROR "负向 fixture 未在预期门禁失败：${name}，要求 ${expected}\n${stdout}${stderr}")
        endif()
        math(EXPR passed "${passed} + 1")
        string(APPEND summary "PASS ${name}\n")
    endforeach()
    string(APPEND summary "${passed} profile/metadata contract fixtures passed.\n")
    file(WRITE "${TEST_ROOT}/results.txt" "${summary}")
    set(manifest "{\"fixture_only\":true,\"passed\":${passed},\"cmake_version\":\"${CMAKE_VERSION}\",\"source_sha256\":{}}")
    foreach(source IN ITEMS tests/sdk_release_profile_tests.cmake cmake/sdk-release-profiles.cmake scripts/package-sdk-release.cmake)
        file(SHA256 "${repo}/${source}" hash)
        string(JSON manifest SET "${manifest}" source_sha256 "${source}" "\"${hash}\"")
    endforeach()
    file(WRITE "${TEST_ROOT}/verification-inputs.json" "${manifest}\n")
    message(STATUS "${passed} profile/metadata fixtures passed; NOT real SDK/device acceptance. Logs: ${TEST_ROOT}")
    return()
endif()
if(NOT EXISTS "${TEST_ROOT}/FIXTURE-ONLY.txt" OR NOT CASE MATCHES "^[a-z0-9-]+$")
    message(FATAL_ERROR "只允许测试驱动在自己的 fixture 根中运行 case")
endif()
set(case_root "${TEST_ROOT}/fixtures/${CASE}")
file(MAKE_DIRECTORY "${case_root}")
set(version 1.3.1)
string(SHA1 head "FIXTURE ONLY source HEAD")
foreach(field IN ITEMS fingerprint library_hash compiler_hash)
    string(SHA256 "${field}" "FIXTURE ONLY ${field}")
endforeach()
awj_sdk_select_profile("${version}" hmos-arm64-api26-clang23 Linux x86_64 p)
set(compiler_version 23.1.5)
set(proof "{\"schema\":1,\"mode\":\"formal\",\"platform\":\"hmos-arm64-api26\",\"profile\":\"hmos-arm64-api26-clang23\",\"version\":\"${version}\",\"git_head\":\"${head}\",\"source_fingerprint\":\"${fingerprint}\",\"source_dirty\":false,\"native_codecs\":true,\"compiled_source_verified\":true,\"compiler_version\":\"${compiler_version}\",\"compiler_sha256\":\"${compiler_hash}\",\"target_triple\":\"aarch64-unknown-linux-ohos\",\"abi\":1,\"library_sha256\":\"${library_hash}\",\"features\":{\"d3d11\":false,\"shm\":false,\"ffmpeg\":false,\"stream\":false,\"gpu\":false},\"encoders\":[\"png\",\"webp\",\"jxl\",\"jpegli\",\"avif\"],\"native_sdk_version\":\"26.0.0.105\",\"api\":26,\"libcxx_version\":15004,\"libcxx_abi_version\":1,\"libcxx_abi_namespace\":\"__n1\",\"runtime_linkage\":\"native-static\"}")

function(write_junit path names)
    list(LENGTH names count)
    set(xml "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"FIXTURE_ONLY\" tests=\"${count}\" failures=\"0\" disabled=\"0\" skipped=\"0\">\n")
    foreach(name IN LISTS names)
        string(APPEND xml "<testcase name=\"${name}\" status=\"run\" time=\"0.001\"><system-out>FIXTURE ONLY; no real test execution</system-out></testcase>\n")
    endforeach()
    string(APPEND xml "</testsuite>\n")
    file(WRITE "${path}" "${xml}")
endfunction()
awj_sdk_required_tests(p required_tests)
set(junit "${case_root}/sdk-fixture.xml")
write_junit("${junit}" "${required_tests}")
file(SHA256 "${junit}" test_hash)
string(JSON proof SET "${proof}" test_report_sha256 "\"${test_hash}\"")
set(consumer_names installed_c_consumer installed_cxx_consumer installed_cxx17_consumer)
set(consumers "{}")
foreach(name IN LISTS consumer_names)
    if(name STREQUAL "installed_c_consumer")
        set(standard c99)
    elseif(name STREQUAL "installed_cxx_consumer")
        set(standard c++11)
    else()
        set(standard c++17)
    endif()
    string(SHA256 hash "FIXTURE ONLY ${name}")
    string(JSON consumers SET "${consumers}" "${name}" "{\"sha256\":\"${hash}\",\"standard\":\"${standard}\",\"exit_code\":0}")
endforeach()
write_junit("${case_root}/consumers-fixture.xml" "${consumer_names}")
file(SHA256 "${case_root}/consumers-fixture.xml" consumer_hash)
set(base_log "FIXTURE ONLY - NOT A REAL DEVICE LOG\nsystem_name=OHOS\nsystem_build=FIXTURE-ONLY-NOT-A-DEVICE-26\napi=26\nabi=arm64-v8a\nlibrary_sha256=${library_hash}\nsource_fingerprint=${fingerprint}\ntest_report_sha256=${test_hash}\n")
set(sdk_log "${base_log}")
foreach(name IN LISTS required_tests)
    string(APPEND sdk_log "test:${name}=exit:0\n")
endforeach()
file(WRITE "${case_root}/sdk-fixture.log" "${sdk_log}")
set(consumer_log "${base_log}consumer_report_sha256=${consumer_hash}\n")
foreach(name IN LISTS consumer_names)
    awj_sdk_json_get(hash "${consumers}" STRING "${name}" sha256)
    string(APPEND consumer_log "consumer:${name}:sha256=${hash}:exit=0\n")
endforeach()
file(WRITE "${case_root}/consumers-fixture.log" "${consumer_log}")
set(loader_log "${base_log}dlopen=success\ndlsym_awj_version=success\ndlsym_awj_abi_version=success\nawj_version=${version}\nawj_abi_version=1\nunresolved_symbols=0\nhost_runtime_loaded=false\nlibcxx_version=15004\nlibcxx_abi_version=1\nlibcxx_abi_namespace=__n1\n")
file(WRITE "${case_root}/loader-fixture.log" "${loader_log}")
set(evidence "${proof}")
string(JSON evidence SET "${evidence}" kind "\"target-execution\"")
string(JSON evidence SET "${evidence}" status "\"passed\"")
string(JSON evidence SET "${evidence}" consumers "${consumers}")
string(JSON evidence SET "${evidence}" consumer_report_sha256 "\"${consumer_hash}\"")
string(JSON evidence SET "${evidence}" target "{\"system_name\":\"OHOS\",\"system_build\":\"FIXTURE-ONLY-NOT-A-DEVICE-26\",\"api\":26,\"abi\":\"arm64-v8a\",\"process_arch\":\"aarch64\"}")
string(JSON evidence SET "${evidence}" loader "{\"loaded_library_sha256\":\"${library_hash}\",\"unresolved_symbols\":0,\"host_runtime_loaded\":false,\"dlopen\":true,\"dlsym_awj_version\":true,\"dlsym_awj_abi_version\":true,\"version\":\"${version}\",\"abi\":1}")
foreach(field IN ITEMS sdk_test_log consumer_report consumer_log loader_log)
    if(field STREQUAL "sdk_test_log")
        set(file sdk-fixture.log)
    elseif(field STREQUAL "consumer_report")
        set(file consumers-fixture.xml)
    elseif(field STREQUAL "consumer_log")
        set(file consumers-fixture.log)
    else()
        set(file loader-fixture.log)
    endif()
    file(SHA256 "${case_root}/${file}" hash)
    string(JSON evidence SET "${evidence}" "${field}" "{\"path\":\"${file}\",\"sha256\":\"${hash}\"}")
endforeach()
set(hmos_binary "Format: elf64-littleaarch64\nArch: aarch64\nAddressSize: 64bit\nType: SharedObject (0x3)\nSONAME Library soname: [libawj_sdk.so]\nNeededLibraries [\n  libc.so\n  libm.so\n]\n")
set(libcxx /fixture-native/llvm/lib/aarch64-linux-ohos/libc++_static.a)
set(unwind /fixture-native/llvm/lib/aarch64-linux-ohos/libunwind.a)
set(link_map "FIXTURE ONLY\n${libcxx}(vector.cpp.o)\n${unwind}(UnwindLevel1.c.o)\n")

function(fixture_msvc_toolchain out)
    string(CONCAT toolchain
        "{\"compiler_id\":\"MSVC\",\"compiler_version\":\"19.51.36260\",\"compiler_sha256\":\"${compiler_hash}\","
        "\"linker_version\":\"14.51.36260.0\",\"linker_sha256\":\"${compiler_hash}\",\"flags_source\":\"cmake-generated-target-profile\","
        "\"targets\":[{\"name\":\"awj_sdk\",\"configuration\":\"Release\",\"runtime\":\"MultiThreaded\",\"minimum_cpu\":\"x64\",\"ipo\":true,\"profile_sha256\":\"${fingerprint}\","
        "\"compile_flags\":[\"/O2\",\"/Ob2\",\"/Oi\",\"/Ot\",\"/Gy\",\"/Gw\",\"/GF\",\"/GL\",\"/MT\"],"
        "\"link_flags\":[\"/LTCG\",\"/OPT:REF\",\"/OPT:ICF\",\"/INCREMENTAL:NO\"]}]}")
    set(${out} "${toolchain}" PARENT_SCOPE)
endfunction()

if(CASE STREQUAL "positive-profiles")
    awj_sdk_select_profile(1.3.0 "" Windows AMD64 w)
    awj_sdk_select_profile(1.3.0 "" Linux x86_64 l)
    if(NOT w_legacy OR NOT l_legacy OR NOT l_major EQUAL 21)
        message(FATAL_ERROR "legacy profile changed")
    endif()
    awj_sdk_select_profile(1.3.1 windows-x64-msvc Windows AMD64 w)
    awj_sdk_select_profile(1.3.1 linux-x64-clang23 Linux x86_64 l)
    awj_sdk_select_profile(2.0.0 hmos-arm64-api26-clang23 Linux x86_64 h)
    if(w_legacy OR l_legacy OR NOT l_major EQUAL 23 OR NOT h_hmos OR
        NOT h_archive STREQUAL "AWJ_SDK_HMOS_arm64_api26.tar.gz" OR NOT h_library STREQUAL "lib/libawj_sdk.so")
        message(FATAL_ERROR "modern profiles changed")
    endif()
    set(banner "Ubuntu clang version 23.1.5 (++202610-FIXTURE-snapshot)\nTarget: x86_64-pc-linux-gnu\nInstalledDir: /fixture/llvm-23/bin")
    awj_sdk_parse_compiler_banner("${banner}" 23 actual)
    awj_sdk_json_quote(quoted "${banner}")
    awj_sdk_json_expect("{\"banner\":${quoted}}" STRING "${banner}" banner)
    if(NOT actual STREQUAL "23.1.5")
        message(FATAL_ERROR "compiler banner parse changed")
    endif()
elseif(CASE MATCHES "^msvc-")
    if(CASE STREQUAL "msvc-clang-banner")
        awj_sdk_parse_msvc_banner("clang version 23.1.4\nTarget: x86_64-pc-windows-msvc" ignored)
    elseif(CASE STREQUAL "msvc-incomplete-banner")
        awj_sdk_parse_msvc_banner("Microsoft (R) C/C++ Optimizing Compiler Version 19 for x64" ignored)
    elseif(CASE STREQUAL "msvc-legacy-escape")
        awj_sdk_select_profile(1.3.1 windows-x64-clang21 Windows AMD64 ignored)
    else()
        awj_sdk_parse_msvc_banner("Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64" msvc_version)
        if(NOT msvc_version STREQUAL "19.51.36260")
            message(FATAL_ERROR "MSVC banner parse changed")
        endif()
        fixture_msvc_toolchain(msvc)
        set(msvc_proof "{\"compiler_id\":\"MSVC\",\"toolchain\":${msvc}}")
        if(CASE STREQUAL "msvc-wrong-id")
            string(JSON msvc_proof SET "${msvc_proof}" toolchain compiler_id "\"Clang\"")
        elseif(CASE STREQUAL "msvc-wrong-hash")
            string(JSON msvc_proof SET "${msvc_proof}" toolchain compiler_sha256 "\"${fingerprint}\"")
        elseif(CASE STREQUAL "msvc-ipo-disabled")
            string(JSON msvc_proof SET "${msvc_proof}" toolchain targets 0 ipo false)
        elseif(CASE STREQUAL "msvc-no-gl")
            string(JSON msvc_proof REMOVE "${msvc_proof}" toolchain targets 0 compile_flags 7)
        elseif(CASE STREQUAL "msvc-no-ltcg")
            string(JSON msvc_proof REMOVE "${msvc_proof}" toolchain targets 0 link_flags 0)
        elseif(CASE STREQUAL "msvc-dynamic-runtime")
            string(JSON msvc_proof SET "${msvc_proof}" toolchain targets 0 runtime "\"MultiThreadedDLL\"")
        elseif(CASE STREQUAL "msvc-override-optimization")
            string(JSON msvc_proof SET "${msvc_proof}" toolchain targets 0 compile_flags 9 "\"/Od\"")
        endif()
        awj_sdk_validate_msvc_release_proof("${msvc_proof}" "${msvc_version}" "${compiler_hash}")
    endif()
elseif(CASE STREQUAL "positive-hmos-target-alias")
    foreach(target IN ITEMS aarch64-linux-ohos aarch64-unknown-linux-ohos)
        if(NOT target MATCHES "${AWJ_SDK_HMOS_COMPILER_TARGET_PATTERN}")
            message(FATAL_ERROR "合法 OHOS 编译器 target alias 被拒绝：${target}")
        endif()
    endforeach()
elseif(CASE MATCHES "^profile-")
    if(CASE STREQUAL "profile-missing")
        awj_sdk_select_profile(1.3.1 "" Linux x86_64 ignored)
    elseif(CASE STREQUAL "profile-future-missing")
        awj_sdk_select_profile(2.0.0 "" Windows AMD64 ignored)
    elseif(CASE STREQUAL "profile-unknown")
        awj_sdk_select_profile(1.3.1 legacy Linux x86_64 ignored)
    elseif(CASE STREQUAL "profile-legacy-escape")
        awj_sdk_select_profile(1.3.1 linux-x64-clang21 Linux x86_64 ignored)
    elseif(CASE STREQUAL "profile-wrong-host")
        awj_sdk_select_profile(1.3.1 windows-x64-msvc Linux x86_64 ignored)
    elseif(CASE STREQUAL "profile-hmos-windows")
        awj_sdk_select_profile(1.3.1 hmos-arm64-api26-clang23 Windows AMD64 ignored)
    else()
        awj_sdk_select_profile(1.3.1 linux-x64-clang23 Linux aarch64 ignored)
    endif()
elseif(CASE MATCHES "^compiler-")
    if(CASE STREQUAL "compiler-old")
        awj_sdk_parse_compiler_banner("clang version 21.1.8" 23 ignored)
    else()
        awj_sdk_parse_compiler_banner("clang version 23" 23 ignored)
    endif()
elseif(CASE STREQUAL "positive-metadata" OR CASE MATCHES "^(proof-|hmos-)")
    set(changes
        "proof-wrong-profile:profile:\"linux-x64-clang23\""
        "proof-wrong-target:target_triple:\"x86_64-pc-linux-gnu\""
        "proof-wrong-compiler:compiler_version:\"21.1.8\""
        "proof-wrong-compiler-hash:compiler_sha256:\"${fingerprint}\""
        "proof-wrong-source:source_fingerprint:\"${library_hash}\""
        "proof-wrong-library:library_sha256:\"${fingerprint}\""
        "proof-wrong-report:test_report_sha256:\"${fingerprint}\""
        "proof-dirty:source_dirty:true"
        "proof-native-disabled:native_codecs:false"
        "proof-encoder-missing:encoders:[\"png\",\"webp\",\"jxl\",\"avif\"]"
        "proof-encoder-duplicate:encoders:[\"png\",\"webp\",\"jxl\",\"avif\",\"avif\"]"
        "hmos-wrong-sdk:native_sdk_version:\"27.0.0.1\""
        "hmos-wrong-api:api:27"
        "hmos-api-string:api:\"26\""
        "hmos-wrong-libcxx:libcxx_version:230000"
        "hmos-wrong-namespace:libcxx_abi_namespace:\"__1\""
        "hmos-wrong-abi:libcxx_abi_version:2"
        "hmos-dynamic-runtime:runtime_linkage:\"shared\"")
    foreach(change IN LISTS changes)
        if(change MATCHES "^${CASE}:([^:]+):(.*)$")
            string(JSON proof SET "${proof}" "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    if(CASE STREQUAL "proof-hmos-shm")
        string(JSON proof SET "${proof}" features shm true)
    elseif(CASE STREQUAL "proof-hmos-gpu")
        string(JSON proof SET "${proof}" features gpu true)
    endif()
    awj_sdk_validate_build_proof("${proof}" p "${version}" "${head}" "${fingerprint}" "${library_hash}" "${test_hash}" "${compiler_version}" "${compiler_hash}")
    if(CASE STREQUAL "positive-metadata")
        foreach(target IN ITEMS windows-x64-msvc linux-x64-clang23)
            if(target STREQUAL "windows-x64-msvc")
                awj_sdk_select_profile(1.3.1 "${target}" Windows AMD64 desktop)
                set(desktop_version 19.51.36260)
                set(d3d11 true)
            else()
                awj_sdk_select_profile(1.3.1 "${target}" Linux x86_64 desktop)
                set(desktop_version 23.1.5)
                set(d3d11 false)
            endif()
            set(desktop_proof "${proof}")
            foreach(field IN ITEMS profile platform)
                string(JSON desktop_proof SET "${desktop_proof}" "${field}" "\"${desktop_${field}}\"")
            endforeach()
            string(JSON desktop_proof SET "${desktop_proof}" target_triple "\"${desktop_triple}\"")
            string(JSON desktop_proof SET "${desktop_proof}" compiler_version "\"${desktop_version}\"")
            string(JSON desktop_proof SET "${desktop_proof}" features "{\"d3d11\":${d3d11},\"shm\":true,\"ffmpeg\":true,\"stream\":true}")
            if(target STREQUAL "windows-x64-msvc")
                fixture_msvc_toolchain(msvc)
                string(JSON desktop_proof SET "${desktop_proof}" compiler_id "\"MSVC\"")
                string(JSON desktop_proof SET "${desktop_proof}" toolchain "${msvc}")
            endif()
            awj_sdk_validate_build_proof("${desktop_proof}" desktop "${version}" "${head}" "${fingerprint}" "${library_hash}" "${test_hash}" "${desktop_version}" "${compiler_hash}")
        endforeach()
    endif()
elseif(CASE MATCHES "^positive-junit" OR CASE MATCHES "^junit-")
    set(actual "${required_tests}")
    if(CASE STREQUAL "junit-only-two")
        set(actual sdk_core sdk_c_abi)
    elseif(CASE STREQUAL "junit-missing-worker")
        list(REMOVE_ITEM actual sdk_codec_worker_failure)
    elseif(CASE STREQUAL "junit-missing-portable" OR CASE STREQUAL "junit-missing-stream")
        awj_sdk_select_profile(1.3.1 linux-x64-clang23 Linux x86_64 desktop)
        awj_sdk_required_tests(desktop required_tests)
        set(actual "${required_tests}")
        if(CASE STREQUAL "junit-missing-portable")
            list(REMOVE_ITEM actual sdk_portable_stdlib_contract)
        else()
            list(REMOVE_ITEM actual sdk_stream_cli)
        endif()
    elseif(CASE STREQUAL "junit-missing-d3d")
        awj_sdk_select_profile(1.3.1 windows-x64-msvc Windows AMD64 desktop)
        awj_sdk_required_tests(desktop required_tests)
        set(actual "${required_tests}")
        list(REMOVE_ITEM actual sdk_d3d11_shared)
    elseif(CASE STREQUAL "junit-duplicate")
        list(APPEND actual sdk_core)
    endif()
    write_junit("${junit}" "${actual}")
    file(READ "${junit}" xml)
    if(CASE STREQUAL "junit-skipped-counter")
        string(REPLACE "skipped=\"0\"" "skipped=\"1\"" xml "${xml}")
    elseif(CASE STREQUAL "junit-hidden-skip")
        string(REPLACE "<system-out>" "<skipped/><system-out>" xml "${xml}")
    elseif(CASE STREQUAL "junit-multiline-skip")
        string(REPLACE "<system-out>" "<skipped\n message=\"fixture\"/><system-out>" xml "${xml}")
    elseif(CASE STREQUAL "junit-disabled-attribute")
        string(REPLACE "status=\"run\"" "status=\"run\" disabled=\"true\"" xml "${xml}")
    elseif(CASE STREQUAL "positive-junit-selfclosing")
        string(REGEX REPLACE "><system-out>[^<]*</system-out></testcase>" "/>" xml "${xml}")
    elseif(CASE STREQUAL "junit-hidden-failure")
        string(REPLACE "<system-out>" "<failure/><system-out>" xml "${xml}")
    elseif(CASE STREQUAL "junit-status-notrun")
        string(REPLACE "status=\"run\"" "status=\"notrun\"" xml "${xml}")
    elseif(CASE STREQUAL "junit-count-lie")
        string(REPLACE "tests=\"7\"" "tests=\"2\"" xml "${xml}")
    elseif(CASE STREQUAL "junit-unclosed")
        string(REPLACE "</testcase>" "" xml "${xml}")
    elseif(CASE STREQUAL "junit-nested")
        string(REPLACE "<system-out>" "<testcase name=\"nested\"><system-out>" xml "${xml}")
    elseif(CASE STREQUAL "junit-entity")
        string(PREPEND xml "<!DOCTYPE testsuite [<!ENTITY bad SYSTEM 'file:///etc/passwd'>]>")
    endif()
    file(WRITE "${junit}" "${xml}")
    awj_sdk_validate_junit("${junit}" "${required_tests}")
elseif(CASE STREQUAL "positive-target-evidence" OR CASE MATCHES "^execution-")
    if(CASE STREQUAL "execution-missing")
        set(path "${case_root}/missing.json")
    else()
        set(path "${case_root}/target-fixture.json")
    endif()
    if(CASE STREQUAL "execution-bare-passed")
        set(evidence "\"passed\"")
    elseif(CASE STREQUAL "execution-unbound")
        string(JSON evidence REMOVE "${evidence}" sdk_test_log)
    elseif(CASE STREQUAL "execution-wrong-source")
        string(JSON evidence SET "${evidence}" source_fingerprint "\"${library_hash}\"")
    elseif(CASE STREQUAL "execution-wrong-library")
        string(JSON evidence SET "${evidence}" library_sha256 "\"${fingerprint}\"")
    elseif(CASE STREQUAL "execution-wrong-sdk-report")
        string(JSON evidence SET "${evidence}" test_report_sha256 "\"${fingerprint}\"")
    elseif(CASE STREQUAL "execution-wrong-consumer")
        string(JSON evidence SET "${evidence}" consumers installed_cxx17_consumer sha256 "\"${fingerprint}\"")
    elseif(CASE STREQUAL "execution-consumer-notrun")
        string(JSON evidence SET "${evidence}" consumers installed_c_consumer exit_code 77)
    elseif(CASE STREQUAL "execution-host-target")
        string(JSON evidence SET "${evidence}" target system_name "\"Linux\"")
    elseif(CASE STREQUAL "execution-wrong-api")
        string(JSON evidence SET "${evidence}" target api 25)
    elseif(CASE STREQUAL "execution-missing-build")
        string(JSON evidence REMOVE "${evidence}" target system_build)
    elseif(CASE STREQUAL "execution-loader-wrong-library")
        string(JSON evidence SET "${evidence}" loader loaded_library_sha256 "\"${fingerprint}\"")
    elseif(CASE STREQUAL "execution-loader-wrong-version")
        string(JSON evidence SET "${evidence}" loader version "\"1.3.0\"")
    elseif(CASE STREQUAL "execution-loader-wrong-abi")
        string(JSON evidence SET "${evidence}" loader abi 2)
    elseif(CASE STREQUAL "execution-host-runtime")
        string(JSON evidence SET "${evidence}" loader host_runtime_loaded true)
    elseif(CASE STREQUAL "execution-loader-unresolved")
        string(JSON evidence SET "${evidence}" loader unresolved_symbols 1)
    elseif(CASE STREQUAL "execution-raw-log-tamper")
        file(APPEND "${case_root}/loader-fixture.log" "TAMPERED FIXTURE\n")
    elseif(CASE STREQUAL "execution-forged-loader-line")
        string(REPLACE "dlopen=success" "dlopen=not-executed" loader_log "${loader_log}")
        file(WRITE "${case_root}/loader-fixture.log" "${loader_log}")
        file(SHA256 "${case_root}/loader-fixture.log" hash)
        string(JSON evidence SET "${evidence}" loader_log sha256 "\"${hash}\"")
    elseif(CASE STREQUAL "execution-consumer-skipped")
        file(READ "${case_root}/consumers-fixture.xml" xml)
        string(REPLACE "skipped=\"0\"" "skipped=\"1\"" xml "${xml}")
        file(WRITE "${case_root}/consumers-fixture.xml" "${xml}")
        file(SHA256 "${case_root}/consumers-fixture.xml" hash)
        string(JSON evidence SET "${evidence}" consumer_report sha256 "\"${hash}\"")
        string(JSON evidence SET "${evidence}" consumer_report_sha256 "\"${hash}\"")
    elseif(CASE STREQUAL "execution-path-traversal")
        string(JSON evidence SET "${evidence}" loader_log path "\"../outside.log\"")
    elseif(CASE STREQUAL "execution-path-absolute")
        string(JSON evidence SET "${evidence}" loader_log path "\"/outside.log\"")
    endif()
    if(NOT CASE STREQUAL "execution-missing")
        file(WRITE "${path}" "${evidence}\n")
    endif()
    awj_sdk_verify_target_execution("${path}" "${TEST_ROOT}" p "${version}" "${head}" "${fingerprint}" "${library_hash}" "${test_hash}" "${consumers}" result)
elseif(CASE MATCHES "^hash-")
    if(CASE STREQUAL "hash-uppercase")
        string(TOUPPER "${test_hash}" upper)
        awj_sdk_require_hash("${upper}" fixture)
    else()
        string(JSON proof SET "${proof}" test_report_sha256 "\"${fingerprint}\"")
        awj_sdk_check_file_hash("${proof}" "${junit}" test_report_sha256)
    endif()
elseif(CASE MATCHES "^tree-")
    set(tree "${case_root}/tree")
    file(MAKE_DIRECTORY "${tree}")
    if(CASE STREQUAL "tree-sensitive")
        set(file private-key.txt)
    elseif(CASE STREQUAL "tree-hidden-sensitive")
        set(file .secret.txt)
    elseif(CASE STREQUAL "tree-static-archive")
        set(file not-a-real-archive.a)
    else()
        set(file libavcodec.so.61)
    endif()
    file(WRITE "${tree}/${file}" "FIXTURE ONLY; deliberately prohibited member name\n")
    check_tree("${tree}")
elseif(CASE STREQUAL "path-outside")
    assert_under("${case_root}" "${TEST_ROOT}/outside")
elseif(CASE STREQUAL "positive-binary-metadata" OR CASE MATCHES "^binary-")
    if(CASE STREQUAL "binary-wrong-arch")
        string(REPLACE "aarch64" "x86_64" hmos_binary "${hmos_binary}")
    elseif(CASE STREQUAL "binary-not-shared")
        string(REPLACE "Type: SharedObject" "Type: Relocatable" hmos_binary "${hmos_binary}")
    elseif(CASE STREQUAL "binary-wrong-soname")
        string(REPLACE "[libawj_sdk.so]" "[libawj_sdk.so.1]" hmos_binary "${hmos_binary}")
    elseif(CASE STREQUAL "binary-not-image")
        file(WRITE "${case_root}/not-an-image.bin" "FIXTURE ONLY\n")
        awj_sdk_check_binary("${case_root}/not-an-image.bin" p /must-not-execute "${case_root}/unused.log")
    elseif(CASE STREQUAL "binary-host-runtime")
        string(REPLACE "  libm.so" "  libc++_shared.so" hmos_binary "${hmos_binary}")
    elseif(CASE STREQUAL "binary-ffmpeg-runtime")
        string(REPLACE "  libm.so" "  libavutil.so.59" hmos_binary "${hmos_binary}")
    elseif(CASE STREQUAL "binary-absolute-rpath")
        string(APPEND hmos_binary "0x RUNPATH Library runpath: [/home/fixture/build]\n")
    endif()
    if(CASE STREQUAL "binary-consumer-unlinked")
        awj_sdk_check_binary_text(p "${hmos_binary}" consumer)
    else()
        awj_sdk_check_binary_text(p "${hmos_binary}")
    endif()
    if(CASE STREQUAL "positive-binary-metadata")
        awj_sdk_select_profile(1.3.1 windows-x64-msvc Windows AMD64 desktop)
        awj_sdk_check_binary_text(desktop "Format: COFF-x86-64\nArch: x86_64\nAddressSize: 64bit\nIMAGE_FILE_DLL\nImport {\n  Name: KERNEL32.dll\n}\n")
        awj_sdk_select_profile(1.3.1 linux-x64-clang23 Linux x86_64 desktop)
        awj_sdk_check_binary_text(desktop "Format: elf64-x86-64\nArch: x86_64\nAddressSize: 64bit\nType: SharedObject\nSONAME Library soname: [libawj_sdk.so.1]\nNeededLibraries [\n  libc.so.6\n  libm.so.6\n]\n")
        string(REPLACE "  libm.so" "  libm.so\n  libawj_sdk.so" consumer_binary "${hmos_binary}")
        awj_sdk_check_binary_text(p "${consumer_binary}" consumer)
    endif()
elseif(CASE STREQUAL "positive-native-link-map" OR CASE MATCHES "^link-map-")
    if(CASE STREQUAL "link-map-missing-static")
        set(link_map "${libcxx}(vector.cpp.o)\n")
    elseif(CASE STREQUAL "link-map-host-unwind")
        string(APPEND link_map "/usr/lib/llvm-23/lib/libunwind.a(unwind.o)\n")
    elseif(CASE STREQUAL "link-map-experimental")
        string(APPEND link_map "/fixture-native/lib/libc++experimental.a(experimental.o)\n")
    endif()
    awj_sdk_check_hmos_link_map("${link_map}" "${libcxx}" "${unwind}")
else()
    message(FATAL_ERROR "未知 fixture CASE：${CASE}")
endif()
message(STATUS "FIXTURE ONLY case completed: ${CASE}")
