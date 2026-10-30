#include "../src/codecs/avif_gain_map_thread_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

extern "C" {
// The fault clone is deliberately built with the test target's release IPO/ISA
// flags. Its unmodified upstream functions are the exact arithmetic oracle for
// that clone; production libavif can have different per-target FP contraction.
decltype(avifRGBImageApplyGainMap) awjTestOfficialRGBApply;
decltype(avifRGBImageComputeGainMap) awjTestOfficialRGBCompute;
decltype(awjAvifRGBImageApplyGainMap) awjTestRGBApply;
decltype(awjAvifRGBImageComputeGainMap) awjTestRGBCompute;
void awjAvifGainMapTestFaults(int threadLimit, int allocationFailure);
}
static_assert(std::is_nothrow_invocable_r_v<avifBool, awjAvifGainMapCancelCallback, void*>);

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void check(avifResult result, const avifDiagnostics& diag) {
    if (result != AVIF_RESULT_OK)
        throw std::runtime_error(std::string(avifResultToString(result)) + ": " + diag.error);
}
struct Rgb {
    avifRGBImage image{};
    Rgb() { image.format = AVIF_RGB_FORMAT_RGBA; image.depth = 16; image.maxThreads = 1; }
    Rgb(std::uint32_t width, std::uint32_t height) : Rgb() {
        image.width = width; image.height = height;
        avifDiagnostics diag{};
        check(avifRGBImageAllocatePixels(&image), diag);
    }
    ~Rgb() { avifRGBImageFreePixels(&image); }
    Rgb(const Rgb&) = delete;
    Rgb& operator=(const Rgb&) = delete;
};
using Map = std::unique_ptr<avifGainMap, decltype(&avifGainMapDestroy)>;
struct Color { avifColorPrimaries primaries; avifTransferCharacteristics transfer; };
constexpr Color sdr{AVIF_COLOR_PRIMARIES_BT709, AVIF_TRANSFER_CHARACTERISTICS_SRGB};
constexpr Color hdr{AVIF_COLOR_PRIMARIES_BT2020, AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084};
enum class Engine { official, awj, fault, fault_official };

Map make_map(std::uint32_t width, std::uint32_t height, avifPixelFormat format, Color alternate, std::uint32_t depth = 8) {
    Map map(avifGainMapCreate(), &avifGainMapDestroy);
    require(bool(map), "gain map allocation failed");
    map->image = avifImageCreate(width, height, depth, format);
    require(map->image != nullptr, "gain map image allocation failed");
    map->image->yuvRange = AVIF_RANGE_FULL;
    map->image->matrixCoefficients = static_cast<avifMatrixCoefficients>(
        format == AVIF_PIXEL_FORMAT_YUV444 ? AVIF_MATRIX_COEFFICIENTS_IDENTITY : AVIF_MATRIX_COEFFICIENTS_BT709);
    map->altColorPrimaries = alternate.primaries;
    map->altTransferCharacteristics = alternate.transfer;
    map->altDepth = 16;
    map->altPlaneCount = 3;
    return map;
}
void fill(Rgb& rgb, bool high, bool uniform = false) {
    // Entirely synthetic, integer-defined inputs. Vary channels, alpha, isolated
    // extremes and broad gain ratios to exercise outlier histograms and edges.
    for (std::uint32_t y = 0; y < rgb.image.height; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(rgb.image.pixels + std::size_t(y) * rgb.image.rowBytes);
        for (std::uint32_t x = 0; x < rgb.image.width; ++x) {
            for (std::uint32_t c = 0; c < 3; ++c) {
                const auto n = (x * 7919u + y * 104729u + c * 12347u) % 50021u;
                row[4 * x + c] = uniform ? 24000 : static_cast<std::uint16_t>(high ? 32000u + n % 22000u : 256u + n);
            }
            if (!uniform && (std::uint64_t(y) * rgb.image.width + x) % 65521 == 0)
                row[4 * x] = high ? 65535 : 1;
            row[4 * x + 3] = static_cast<std::uint16_t>((x * 13u + y * 43u) & 65535u);
        }
    }
}
avifResult compute(Engine engine, const Rgb& base, Color bc, const Rgb& alt, Color ac, avifGainMap* map,
                   std::uint32_t threads, awjAvifGainMapExecution& execution, avifDiagnostics& diag,
                   awjAvifGainMapCancelCallback cancel = nullptr, void* data = nullptr) {
    if (engine == Engine::official || engine == Engine::fault_official) {
        execution = {1, AVIF_FALSE};
        const auto fn = engine == Engine::official ? &avifRGBImageComputeGainMap : &awjTestOfficialRGBCompute;
        return fn(&base.image, bc.primaries, bc.transfer, &alt.image, ac.primaries, ac.transfer, map, &diag);
    }
    const auto fn = engine == Engine::awj ? &awjAvifRGBImageComputeGainMap : &awjTestRGBCompute;
    return fn(&base.image, bc.primaries, bc.transfer, &alt.image, ac.primaries, ac.transfer, map, &diag, threads, cancel, data, &execution);
}
avifResult apply(Engine engine, const Rgb& base, Color bc, const avifGainMap* map, float headroom,
                 Color outputColor, Rgb& output, std::uint32_t threads, awjAvifGainMapExecution& execution,
                 avifContentLightLevelInformationBox* clli, avifDiagnostics& diag,
                 awjAvifGainMapCancelCallback cancel = nullptr, void* data = nullptr) {
    if (engine == Engine::official || engine == Engine::fault_official) {
        execution = {1, AVIF_FALSE};
        const auto fn = engine == Engine::official ? &avifRGBImageApplyGainMap : &awjTestOfficialRGBApply;
        return fn(&base.image, bc.primaries, bc.transfer, map, headroom,
            outputColor.primaries, outputColor.transfer, &output.image, clli, &diag);
    }
    const auto fn = engine == Engine::awj ? &awjAvifRGBImageApplyGainMap : &awjTestRGBApply;
    return fn(&base.image, bc.primaries, bc.transfer, map, headroom, outputColor.primaries, outputColor.transfer,
              &output.image, clli, &diag, threads, cancel, data, &execution);
}
template<class T> bool fraction_equal(const T& a, const T& b) { return a.n == b.n && a.d == b.d; }
bool same_math(const avifGainMap& a, const avifGainMap& b) {
    if (a.useBaseColorSpace != b.useBaseColorSpace || !fraction_equal(a.baseHdrHeadroom, b.baseHdrHeadroom) ||
        !fraction_equal(a.alternateHdrHeadroom, b.alternateHdrHeadroom)) return false;
    for (int c = 0; c < 3; ++c)
        if (!fraction_equal(a.gainMapMin[c], b.gainMapMin[c]) || !fraction_equal(a.gainMapMax[c], b.gainMapMax[c]) ||
            !fraction_equal(a.gainMapGamma[c], b.gainMapGamma[c]) || !fraction_equal(a.baseOffset[c], b.baseOffset[c]) ||
            !fraction_equal(a.alternateOffset[c], b.alternateOffset[c])) return false;
    return true;
}
void same_map(const avifGainMap& a, const avifGainMap& b) {
    require(same_math(a, b), "headroom/gamma/gain/offset/color-space metadata differ from upstream");
    require(a.altColorPrimaries == b.altColorPrimaries && a.altTransferCharacteristics == b.altTransferCharacteristics &&
            a.altDepth == b.altDepth && a.altPlaneCount == b.altPlaneCount, "alternate metadata changed");
    const auto& x = *a.image; const auto& y = *b.image;
    require(x.width == y.width && x.height == y.height && x.depth == y.depth && x.yuvFormat == y.yuvFormat &&
            x.matrixCoefficients == y.matrixCoefficients && x.yuvRange == y.yuvRange, "gain map geometry/format differs");
    for (int c = 0; c < (x.yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? 1 : 3); ++c) {
        const auto bytes = std::size_t(avifImagePlaneWidth(&x, c)) * (x.depth > 8 ? 2 : 1);
        for (std::uint32_t r = 0; r < avifImagePlaneHeight(&x, c); ++r)
            require(std::memcmp(x.yuvPlanes[c] + std::size_t(r) * x.yuvRowBytes[c],
                                y.yuvPlanes[c] + std::size_t(r) * y.yuvRowBytes[c], bytes) == 0, "gain map pixels differ from upstream");
    }
    require(bool(x.alphaPlane) == bool(y.alphaPlane), "gain map alpha-plane presence differs");
    if (x.alphaPlane) {
        const auto bytes = std::size_t(x.width) * (x.depth > 8 ? 2 : 1);
        for (std::uint32_t r = 0; r < x.height; ++r)
            require(std::memcmp(x.alphaPlane + std::size_t(r) * x.alphaRowBytes,
                                y.alphaPlane + std::size_t(r) * y.alphaRowBytes, bytes) == 0,
                    "gain map alpha pixels differ from upstream");
    }
}
void same_rgb(const Rgb& a, const Rgb& b, const char* context = "unspecified") {
    require(a.image.width == b.image.width && a.image.height == b.image.height && a.image.depth == b.image.depth &&
            a.image.format == b.image.format && a.image.isFloat == b.image.isFloat, "apply output geometry/format differs");
    const auto pixelBytes = avifRGBImagePixelSize(&a.image);
    const auto channelBytes = a.image.depth > 8 ? 2u : 1u;
    const auto bytes = std::size_t(a.image.width) * pixelBytes;
    for (std::uint32_t y = 0; y < a.image.height; ++y) {
        const auto* expected = a.image.pixels + std::size_t(y) * a.image.rowBytes;
        const auto* actual = b.image.pixels + std::size_t(y) * b.image.rowBytes;
        if (std::memcmp(expected, actual, bytes) == 0) continue;
        for (std::size_t offset = 0; offset < bytes; offset += channelBytes) {
            std::uint16_t referenceValue = 0, candidateValue = 0;
            std::memcpy(&referenceValue, expected + offset, channelBytes);
            std::memcpy(&candidateValue, actual + offset, channelBytes);
            if (referenceValue != candidateValue) {
                std::fprintf(stderr, "apply mismatch: %s x=%zu y=%u component=%zu depth=%u official=%u awj=%u\n",
                    context, offset / pixelBytes, y, (offset % pixelBytes) / channelBytes,
                    a.image.depth, static_cast<unsigned>(referenceValue), static_cast<unsigned>(candidateValue));
                throw std::runtime_error("apply pixels/alpha differ from upstream");
            }
        }
    }
}
void budget_check(const awjAvifGainMapExecution& execution, std::uint32_t budget, std::size_t pixels) {
    const auto allowed = std::max<std::size_t>(1, std::min({std::size_t(budget ? budget : 1), std::size_t(64), pixels / 16384}));
    require(execution.workers_started >= 1 && execution.workers_started <= allowed && !execution.cancelled, "worker budget/report violated");
    if (allowed > 1) require(execution.workers_started > 1, "parallel coverage unavailable: OS started no extra worker");
}
float value(avifUnsignedFraction f) { return static_cast<float>(f.n) / f.d; }
std::uint64_t rgb_hash(const Rgb& rgb);

void correctness_case(std::uint32_t width, std::uint32_t height, bool reverse, bool different, avifPixelFormat format, bool uniform = false) {
    Rgb low(width, height), high(width, height);
    fill(low, false, uniform); fill(high, true, uniform);
    const auto lowBefore = rgb_hash(low), highBefore = rgb_hash(high);
    const Color lowColor = different ? Color{AVIF_COLOR_PRIMARIES_SMPTE432, AVIF_TRANSFER_CHARACTERISTICS_SRGB} : sdr;
    const Color highColor = uniform ? lowColor : Color{static_cast<avifColorPrimaries>(different ? AVIF_COLOR_PRIMARIES_BT2020 : lowColor.primaries), hdr.transfer};
    const Rgb& base = reverse ? high : low; const Rgb& alt = reverse ? low : high;
    const Color bc = reverse ? highColor : lowColor, ac = reverse ? lowColor : highColor;
    const auto mapWidth = std::max(1u, width / 3), mapHeight = std::max(1u, height / 5);
    auto reference = make_map(mapWidth, mapHeight, format, ac);
    avifDiagnostics diag{}; awjAvifGainMapExecution execution{};
    check(compute(Engine::official, base, bc, alt, ac, reference.get(), 1, execution, diag), diag);
    constexpr std::array budgets{0u, 1u, 2u, 5u, std::numeric_limits<std::uint32_t>::max()};
    for (auto budget : budgets) {
        auto candidate = make_map(mapWidth, mapHeight, format, ac);
        auto* identity = candidate->image;
        check(compute(Engine::awj, base, bc, alt, ac, candidate.get(), budget, execution, diag), diag);
        budget_check(execution, budget, std::size_t(width) * height);
        require(candidate->image == identity, "compute replaced caller's image object");
        same_map(*reference, *candidate);
    }
    // Exercise non-unit gamma and offsets when applying an already encoded map;
    // compute intentionally preserves upstream's unit-gamma encoding policy.
    reference->gainMapGamma[0] = {5, 4}; reference->gainMapGamma[1] = {4, 5}; reference->gainMapGamma[2] = {7, 6};
    const float baseHeadroom = value(reference->baseHdrHeadroom), altHeadroom = value(reference->alternateHdrHeadroom);
    for (const float headroom : {altHeadroom, (baseHeadroom + altHeadroom) * 0.5f, baseHeadroom}) {
        for (const Color outColor : {ac, bc}) {
            Rgb expected;
            avifContentLightLevelInformationBox expectedCLLI{17, 19};
            check(apply(Engine::official, base, bc, reference.get(), headroom, outColor, expected, 1, execution, &expectedCLLI, diag), diag);
            const auto compare = [&](const Rgb& result, std::uint32_t budget, bool hasCLLI) {
                char context[384];
                std::snprintf(context, sizeof(context),
                    "size=%ux%u reverse=%d different=%d map_format=%d uniform=%d headroom=%.9g base_cicp=%u/%u alt_cicp=%u/%u output_cicp=%u/%u budget=%u workers=%u clli=%d",
                    width, height, int(reverse), int(different), int(format), int(uniform), double(headroom),
                    unsigned(bc.primaries), unsigned(bc.transfer), unsigned(ac.primaries), unsigned(ac.transfer),
                    unsigned(outColor.primaries), unsigned(outColor.transfer), budget, execution.workers_started, int(hasCLLI));
                same_rgb(expected, result, context);
            };
            for (auto budget : budgets) {
                Rgb result;
                avifContentLightLevelInformationBox resultCLLI{17, 19};
                check(apply(Engine::awj, base, bc, reference.get(), headroom, outColor, result, budget, execution, &resultCLLI, diag), diag);
                budget_check(execution, budget, std::size_t(width) * height);
                compare(result, budget, true);
                require(expectedCLLI.maxCLL == resultCLLI.maxCLL && expectedCLLI.maxPALL == resultCLLI.maxPALL,
                        "CLLI differs: float accumulation order/precision changed");
            }
            Rgb noCLLI;
            check(apply(Engine::awj, base, bc, reference.get(), headroom, outColor, noCLLI, 5, execution, nullptr, diag), diag);
            compare(noCLLI, 5, false);
        }
    }
    require(rgb_hash(low) == lowBefore && rgb_hash(high) == highBefore, "borrowed endpoint pixels were modified");
}

struct CancelState { std::atomic<std::uint32_t> polls{0}; std::uint32_t limit; };
avifBool cancel_after(void* pointer) noexcept {
    auto& state = *static_cast<CancelState*>(pointer);
    return state.polls.fetch_add(1, std::memory_order_relaxed) + 1 >= state.limit ? AVIF_TRUE : AVIF_FALSE;
}
void cancellation_and_faults() {
    Rgb low(513, 257), high(513, 257); fill(low, false); fill(high, true);
    auto reference = make_map(171, 51, AVIF_PIXEL_FORMAT_YUV444, hdr);
    avifDiagnostics diag{}; awjAvifGainMapExecution execution{};
    check(compute(Engine::official, low, sdr, high, hdr, reference.get(), 1, execution, diag), diag);
    auto faultReference = make_map(171, 51, AVIF_PIXEL_FORMAT_YUV444, hdr);
    check(compute(Engine::fault_official, low, sdr, high, hdr, faultReference.get(), 1, execution, diag), diag);
    Rgb expected;
    avifContentLightLevelInformationBox expectedCLLI{};
    const float headroom = value(reference->alternateHdrHeadroom);
    check(apply(Engine::fault_official, low, sdr, faultReference.get(), value(faultReference->alternateHdrHeadroom),
                hdr, expected, 1, execution, &expectedCLLI, diag), diag);
    for (const int limit : {0, 1}) {
        // Failure before any spawned worker and after a successful partial start.
        awjAvifGainMapTestFaults(limit, 0);
        auto candidate = make_map(171, 51, AVIF_PIXEL_FORMAT_YUV444, hdr);
        check(compute(Engine::fault, low, sdr, high, hdr, candidate.get(), 5, execution, diag), diag);
        require(execution.workers_started == static_cast<unsigned>(limit + 1), "partial-start worker report incorrect");
        same_map(*faultReference, *candidate);
        Rgb output; avifContentLightLevelInformationBox clli{};
        check(apply(Engine::fault, low, sdr, faultReference.get(), value(faultReference->alternateHdrHeadroom),
                    hdr, output, 5, execution, &clli, diag), diag);
        require(execution.workers_started == static_cast<unsigned>(limit + 1), "apply partial-start worker report incorrect");
        char faultContext[160];
        std::snprintf(faultContext, sizeof(faultContext), "fault size=513x257 reverse=0 different=1 map_format=1 budget=5 created_limit=%d workers=%u clli=1", limit, execution.workers_started);
        same_rgb(expected, output, faultContext);
        require(clli.maxCLL == expectedCLLI.maxCLL && clli.maxPALL == expectedCLLI.maxPALL, "partial-start CLLI changed");
    }
    awjAvifGainMapTestFaults(-1, 0);
    for (const unsigned limit : {1u, 32u}) {
        CancelState computeCancel{{0}, limit};
        auto candidate = make_map(171, 51, AVIF_PIXEL_FORMAT_YUV444, hdr);
        candidate->gainMapMin[0] = {123, 7};
        check(avifImageAllocatePlanes(candidate->image, AVIF_PLANES_YUV), diag);
        for (int c = 0; c < 3; ++c)
            std::memset(candidate->image->yuvPlanes[c], 0x5a,
                        std::size_t(candidate->image->yuvRowBytes[c]) * candidate->image->height);
        const avifGainMap before = *candidate;
        auto* previousPlane = candidate->image->yuvPlanes[0];
        const auto result = compute(Engine::awj, low, sdr, high, hdr, candidate.get(), 5, execution, diag, &cancel_after, &computeCancel);
        require(result == AVIF_RESULT_UNKNOWN_ERROR && execution.cancelled && std::strstr(diag.error, "cancelled"), "compute cancellation not reported");
        require(same_math(before, *candidate) && candidate->image == before.image && candidate->image->width == 171 &&
                candidate->image->height == 51 && candidate->image->yuvPlanes[0] == previousPlane &&
                previousPlane[0] == 0x5a, "cancelled compute published partial metadata/planes");
        require((execution.workers_started == 0) == (limit == 1), "pre/mid-compute cancel worker report incorrect");
        CancelState applyCancel{{0}, limit};
        Rgb output(1, 1); std::memset(output.image.pixels, 0x5a, output.image.rowBytes);
        auto* previous = output.image.pixels;
        avifContentLightLevelInformationBox clli{123, 456};
        const auto applied = apply(Engine::awj, low, sdr, reference.get(), headroom, hdr, output, 5, execution, &clli, diag, &cancel_after, &applyCancel);
        require(applied == AVIF_RESULT_UNKNOWN_ERROR && execution.cancelled && std::strstr(diag.error, "cancelled"), "apply cancellation not reported");
        require(output.image.pixels == previous && output.image.width == 1 && output.image.height == 1 &&
                output.image.pixels[0] == 0x5a && clli.maxCLL == 123 && clli.maxPALL == 456, "cancelled apply published partial output/CLLI");
        require((execution.workers_started == 0) == (limit == 1), "pre/mid-apply cancel worker report incorrect");
    }
    awjAvifGainMapTestFaults(-1, 1);
    auto failed = make_map(171, 51, AVIF_PIXEL_FORMAT_YUV444, hdr);
    const avifGainMap before = *failed;
    require(compute(Engine::fault, low, sdr, high, hdr, failed.get(), 5, execution, diag) == AVIF_RESULT_OUT_OF_MEMORY &&
            !execution.cancelled && same_math(before, *failed) && !failed->image->yuvPlanes[0], "compute swallowed allocation failure");
    Rgb output; avifContentLightLevelInformationBox clli{123, 456};
    require(apply(Engine::fault, low, sdr, reference.get(), headroom, hdr, output, 5, execution, &clli, diag) == AVIF_RESULT_OUT_OF_MEMORY &&
            !execution.cancelled && !output.image.pixels && clli.maxCLL == 123 && clli.maxPALL == 456, "apply swallowed allocation failure");
    awjAvifGainMapTestFaults(-1, 0);
    reference->gainMapGamma[0].d = 0;
    require(apply(Engine::awj, low, sdr, reference.get(), headroom, hdr, output, 5, execution, &clli, diag) == AVIF_RESULT_INVALID_ARGUMENT &&
            !execution.cancelled && !output.image.pixels, "invalid metadata was swallowed or published");
    reference->gainMapGamma[0].d = 1;
    require(apply(Engine::awj, low, sdr, reference.get(), std::numeric_limits<float>::quiet_NaN(), hdr,
                  output, 5, execution, &clli, diag) == AVIF_RESULT_INVALID_ARGUMENT, "NaN headroom accepted");
    const auto savedStride = low.image.rowBytes;
    low.image.rowBytes = 1;
    require(compute(Engine::awj, low, sdr, high, hdr, failed.get(), 5, execution, diag) == AVIF_RESULT_INVALID_ARGUMENT,
            "invalid stride accepted");
    low.image.rowBytes = savedStride;
}

// Fingerprints are validation, never timed work; they do not retain a second
// full-size result. All engines run identical inputs/options on this platform.
std::uint64_t hash_bytes(std::uint64_t hash, const void* pointer, std::size_t count) {
    const auto* bytes = static_cast<const std::uint8_t*>(pointer);
    for (std::size_t i = 0; i < count; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}
std::uint64_t rgb_hash(const Rgb& rgb) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint32_t y = 0; y < rgb.image.height; ++y)
        hash = hash_bytes(hash, rgb.image.pixels + std::size_t(y) * rgb.image.rowBytes,
                          std::size_t(rgb.image.width) * avifRGBImagePixelSize(&rgb.image));
    return hash;
}
double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0, "Cannot read process CPU time");
    const auto ticks = [](FILETIME value) { return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000'000.0;
#else
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "Cannot read process CPU time");
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
#endif
}
std::uint64_t process_peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0, "Cannot read process peak RSS");
    return counters.PeakWorkingSetSize;
#else
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "Cannot read process peak RSS");
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}
std::uint64_t gain_map_hash(const avifGainMap& map) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto number = [&](std::uint64_t value) {
        std::array<std::uint8_t, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
        hash = hash_bytes(hash, bytes.data(), bytes.size());
    };
    const auto fraction = [&](const auto& value) { number(value.n); number(value.d); };
    number(map.useBaseColorSpace); fraction(map.baseHdrHeadroom); fraction(map.alternateHdrHeadroom);
    for (int c = 0; c < 3; ++c) {
        fraction(map.gainMapMin[c]); fraction(map.gainMapMax[c]); fraction(map.gainMapGamma[c]);
        fraction(map.baseOffset[c]); fraction(map.alternateOffset[c]);
    }
    number(map.altColorPrimaries); number(map.altTransferCharacteristics); number(map.altDepth); number(map.altPlaneCount);
    const auto& image = *map.image;
    number(image.width); number(image.height); number(image.depth); number(image.yuvFormat);
    number(image.matrixCoefficients); number(image.yuvRange);
    for (int c = 0; c < 3; ++c) for (std::uint32_t y = 0; y < avifImagePlaneHeight(&image, c); ++y)
        hash = hash_bytes(hash, image.yuvPlanes[c] + std::size_t(y) * image.yuvRowBytes[c],
            std::size_t(avifImagePlaneWidth(&image, c)) * (image.depth > 8 ? 2u : 1u));
    return hash;
}
void benchmark(unsigned iterations, std::string_view engine_filter = {}) {
    constexpr std::uint32_t width = 6145, height = 4097, mapWidth = 1537, mapHeight = 1025;
    Rgb low(width, height), high(width, height); fill(low, false); fill(high, true);
    Map reference(nullptr, &avifGainMapDestroy);
    avifDiagnostics diag{}; awjAvifGainMapExecution execution{};
    struct Run { Engine engine; const char* name; std::uint32_t budget; };
    constexpr std::array runs{Run{Engine::official, "official", 1}, Run{Engine::awj, "awj-1", 1}, Run{Engine::awj, "awj-12", 12}};
    std::puts("phase,engine,iteration,width,height,map_width,map_height,seconds,cpu_seconds,workers_started,pixel_hash,max_cll,max_pall");
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        for (const auto& run : runs) {
            if (!engine_filter.empty() && engine_filter != run.name) continue;
            auto map = make_map(mapWidth, mapHeight, AVIF_PIXEL_FORMAT_YUV444, hdr);
            const auto cpu = process_cpu_seconds();
            const auto start = std::chrono::steady_clock::now();
            const auto result = compute(run.engine, low, sdr, high, hdr, map.get(), run.budget, execution, diag);
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const auto cpu_seconds = process_cpu_seconds() - cpu;
            check(result, diag);
            const auto hash = gain_map_hash(*map);
            if (!reference) reference = std::move(map);
            else same_map(*reference, *map);
            std::printf("compute,%s,%u,%u,%u,%u,%u,%.9f,%.9f,%u,%016llx,0,0\n", run.name, iteration + 1,
                        width, height, mapWidth, mapHeight, seconds, cpu_seconds, execution.workers_started,
                        static_cast<unsigned long long>(hash));
            std::fflush(stdout);
        }
        std::uint64_t referenceHash = 0;
        avifContentLightLevelInformationBox referenceCLLI{};
        bool have_reference = false;
        for (const auto& run : runs) {
            if (!engine_filter.empty() && engine_filter != run.name) continue;
            Rgb output; avifContentLightLevelInformationBox clli{};
            const auto cpu = process_cpu_seconds();
            const auto start = std::chrono::steady_clock::now();
            const auto result = apply(run.engine, low, sdr, reference.get(), value(reference->alternateHdrHeadroom), hdr,
                                      output, run.budget, execution, &clli, diag);
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const auto cpu_seconds = process_cpu_seconds() - cpu;
            check(result, diag);
            const auto hash = rgb_hash(output);
            if (!have_reference) { referenceHash = hash; referenceCLLI = clli; have_reference = true; }
            else require(hash == referenceHash && clli.maxCLL == referenceCLLI.maxCLL && clli.maxPALL == referenceCLLI.maxPALL,
                         "25MP apply pixels/CLLI differ from official reference");
            std::printf("apply,%s,%u,%u,%u,%u,%u,%.9f,%.9f,%u,%016llx,%u,%u\n", run.name, iteration + 1,
                        width, height, mapWidth, mapHeight, seconds, cpu_seconds, execution.workers_started,
                        static_cast<unsigned long long>(hash), static_cast<unsigned>(clli.maxCLL), static_cast<unsigned>(clli.maxPALL));
            std::fflush(stdout);
        }
    }
    const auto name = engine_filter.empty() ? std::string_view{"all"} : engine_filter;
    std::printf("#process_resources,{\"engine\":\"%.*s\",\"cpu_seconds_total\":%.9f,\"peak_rss_bytes\":%llu}\n",
        static_cast<int>(name.size()), name.data(), process_cpu_seconds(), static_cast<unsigned long long>(process_peak_rss_bytes()));
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            require(std::string_view(argv[1]) == "--benchmark-only", "Expected --benchmark-only [--iterations 1..5] [--engine official|awj-1|awj-12]");
            unsigned iterations = 1;
            std::string_view engine_filter;
            for (int argument = 2; argument < argc; argument += 2) {
                require(argument + 1 < argc, "Missing benchmark argument value");
                const std::string_view key(argv[argument]), input(argv[argument + 1]);
                if (key == "--iterations") {
                    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), iterations);
                    require(parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && iterations >= 1 && iterations <= 5,
                            "Benchmark iterations must be 1..5");
                } else {
                    require(key == "--engine" && (input == "official" || input == "awj-1" || input == "awj-12"), "Unknown benchmark engine");
                    engine_filter = input;
                }
            }
            benchmark(iterations, engine_filter);
            return 0;
        }
        for (const bool reverse : {false, true}) for (const bool different : {false, true})
            for (const auto format : {AVIF_PIXEL_FORMAT_YUV444, AVIF_PIXEL_FORMAT_YUV400})
                correctness_case(513, 257, reverse, different, format);
        correctness_case(17, 9, false, true, AVIF_PIXEL_FORMAT_YUV420);
        correctness_case(19, 11, true, false, AVIF_PIXEL_FORMAT_YUV444, true);
        correctness_case(1, 1, false, false, AVIF_PIXEL_FORMAT_YUV400, true);
        cancellation_and_faults();
        std::puts("Gain-map parallel tests passed (official/0/1/multi, exact pixels/metadata/CLLI, cancellation, startup/alloc faults).");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gain_map_parallel_tests: %s\n", error.what());
        return 1;
    }
}
