if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
set(JPEGR_CPP "${SOURCE_DIR}/lib/src/jpegr.cpp")
file(READ "${JPEGR_CPP}" CONTENT)
if(NOT CONTENT MATCHES "awjUhdrThreadLimit")
    set(ANCHOR "unsigned int GetCPUCoreCount() { return (std::max)(1u, std::thread::hardware_concurrency()); }")
    string(FIND "${CONTENT}" "${ANCHOR}" POS)
    if(POS EQUAL -1)
        message(FATAL_ERROR "libultrahdr CPU budget adapter needs upstream review")
    endif()
    set(REPLACEMENT [=[
// AWJ: honor the calling file worker's CPU budget. Decode calls are synchronous;
// the limit is thread-local so concurrent files never change each other's budget.
static thread_local unsigned int awjUhdrCpuLimit = 0;
extern "C" unsigned int awjUhdrThreadLimit(unsigned int limit) {
  const auto previous = awjUhdrCpuLimit;
  awjUhdrCpuLimit = limit;
  return previous;
}
unsigned int GetCPUCoreCount() {
  const auto available = (std::max)(1u, std::thread::hardware_concurrency());
  return awjUhdrCpuLimit ? (std::min)(available, awjUhdrCpuLimit) : available;
}
]=])
    string(REPLACE "${ANCHOR}" "${REPLACEMENT}" CONTENT "${CONTENT}")
    file(WRITE "${JPEGR_CPP}" "${CONTENT}")
endif()
