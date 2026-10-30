#define NOMINMAX
#include <windows.h>
#include "awj/awj_shm.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行共享内存测试失败: %s; %s\n", __LINE__, #value, awj_last_error()); return 1; } } while (false)
namespace {
void fill_desc(awj_frame_desc& desc, uint8_t* pixels, uint64_t capacity) {
  awj_frame_desc_init(&desc); desc.width = 2; desc.height = 2; desc.frame_id = 77;
  desc.planes[0] = {pixels, capacity, 0, 8};
  desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
}
int abrupt_peer(const char* name, bool producer) {
  awj_context* context{}; CHECK(awj_context_create(nullptr, &context) == AWJ_OK);
  awj_shm_channel* channel{};
  CHECK((producer ? awj_shm_create(context, name, 2, 4096, &channel) : awj_shm_open(context, name, &channel)) == AWJ_OK);
  if (producer) {
    awj_shm_write slot{sizeof(slot), AWJ_ABI_VERSION};
    CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_OK);
    std::memset(slot.data, 42, 16); awj_frame_desc desc; fill_desc(desc, slot.data, slot.capacity);
    CHECK(awj_shm_commit(channel, slot.token, &desc) == AWJ_OK);
    CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_OK); // 遗留 WRITING 槽位。
  } else {
    awj_frame* frame{}; CHECK(awj_shm_receive(channel, 5000, &frame) == AWJ_OK); // 遗留 READING 槽位。
  }
  const std::string base{name};
  const auto ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, (base + "-ready").c_str());
  const auto finish = OpenEventA(SYNCHRONIZE, FALSE, (base + "-exit").c_str());
  CHECK(ready && finish && SetEvent(ready));
  CHECK(WaitForSingleObject(finish, 10000) == WAIT_OBJECT_0);
  // 只结束本测试创建的子进程，不执行 SDK 清理，以覆盖真实进程退出。
  ExitProcess(0);
}
int abrupt_test(const std::wstring& executable, const std::string& name, bool child_producer, awj_context* context) {
  const auto ready = CreateEventA(nullptr, TRUE, FALSE, (name + "-ready").c_str());
  const auto finish = CreateEventA(nullptr, TRUE, FALSE, (name + "-exit").c_str());
  CHECK(ready && finish);
  awj_shm_channel* channel{};
  awj_shm_write slot{sizeof(slot), AWJ_ABI_VERSION}; awj_frame_desc desc;
  if (!child_producer) {
    CHECK(awj_shm_create(context, name.c_str(), 1, 4096, &channel) == AWJ_OK);
    CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_OK);
    std::memset(slot.data, 42, 16); fill_desc(desc, slot.data, slot.capacity);
    CHECK(awj_shm_commit(channel, slot.token, &desc) == AWJ_OK);
  }
  auto command = L"\"" + executable + (child_producer ? L"\" --abrupt-producer " : L"\" --abrupt-consumer ") + std::wstring(name.begin(), name.end());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
  CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
  awj_frame* held{};
  if (child_producer) {
    CHECK(awj_shm_open(context, name.c_str(), &channel) == AWJ_OK);
    CHECK(awj_shm_receive(channel, 0, &held) == AWJ_OK);
  } else CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_BUSY);
  CHECK(SetEvent(finish)); CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
  DWORD code = 99; CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
  awj_frame* next{};
  CHECK((child_producer ? awj_shm_receive(channel, 5000, &next) : awj_shm_begin_write(channel, 5000, &slot)) == AWJ_IO_ERROR);
  if (held) {
    const uint8_t* row{}; uint64_t bytes{};
    CHECK(awj_frame_row(held, 0, 0, &row, &bytes) == AWJ_OK && row[0] == 42);
    awj_frame_release(held);
  }
  awj_shm_destroy(channel); CloseHandle(ready); CloseHandle(finish); CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return 0;
}
int consumer(const char* name) {
  awj_context* context{}; CHECK(awj_context_create(nullptr, &context) == AWJ_OK);
  awj_shm_channel* channel{}; CHECK(awj_shm_open(context, name, &channel) == AWJ_OK);
  awj_frame* frame{}; CHECK(awj_shm_receive(channel, 5000, &frame) == AWJ_OK);
  awj_shm_destroy(channel); awj_context_destroy(context);
  const uint8_t* row{}; uint64_t size{};
  CHECK(awj_frame_row(frame, 0, 0, &row, &size) == AWJ_OK && size == 8 && row[0] == 42);
  awj_frame_release(frame);
  return 0;
}
}
int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--consumer") == 0) return consumer(argv[2]);
  if (argc == 3 && std::strcmp(argv[1], "--abrupt-producer") == 0) return abrupt_peer(argv[2], true);
  if (argc == 3 && std::strcmp(argv[1], "--abrupt-consumer") == 0) return abrupt_peer(argv[2], false);
  awj_context_options options; awj_context_options_init(&options);
  options.max_frame_bytes = 4096; options.memory_budget_bytes = 65536;
  awj_context* context{}; CHECK(awj_context_create(&options, &context) == AWJ_OK);
  const auto name = "sdk-shm-" + std::to_string(GetCurrentProcessId());
  awj_shm_channel* producer{}; awj_shm_channel* reader{}; awj_shm_channel* duplicate{};
  CHECK(awj_shm_create(context, "../invalid", 1, 4096, &producer) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_create(context, name.c_str(), 1, 4096, &producer) == AWJ_OK);
  CHECK(awj_shm_create(context, name.c_str(), 1, 4096, &duplicate) == AWJ_BUSY && !duplicate);
  CHECK(awj_shm_open(context, name.c_str(), &reader) == AWJ_OK);
  CHECK(awj_shm_open(context, name.c_str(), &duplicate) != AWJ_OK && !duplicate);
  awj_shm_write slot{sizeof(slot), AWJ_ABI_VERSION, 0, nullptr, 0};
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_OK);
  auto* original = slot.data; const auto first_token = slot.token;
  for (uint32_t i = 0; i < 16; ++i) slot.data[i] = static_cast<uint8_t>(i);
  awj_frame_desc desc; fill_desc(desc, slot.data, slot.capacity);
  const uint8_t external[16]{};
  auto invalid = desc; invalid.planes[0].data = external; invalid.planes[0].size = 16;
  CHECK(awj_shm_commit(producer, slot.token, &invalid) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_commit(producer, slot.token, &desc) == AWJ_OK);
  CHECK(awj_shm_commit(producer, slot.token, &desc) == AWJ_INVALID_ARGUMENT);
  awj_frame* frame{}; CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_OK);
  const uint8_t* row{}; uint64_t size{};
  awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION, 0, 0, 0, 0, 0, 0};
  CHECK(awj_frame_diagnostics(frame, &diagnostics) == AWJ_OK && diagnostics.input_copy_bytes == 0);
  CHECK(awj_frame_row(frame, 0, 1, &row, &size) == AWJ_OK && size == 8 && row[0] == 8);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_BUSY);
  awj_frame_release(frame);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_OK && slot.data == original && slot.token != first_token);
  CHECK(awj_shm_abort_write(producer, first_token) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_abort_write(producer, slot.token) == AWJ_OK);
  awj_shm_close(producer);
  CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_END_OF_STREAM);
  awj_shm_destroy(reader); awj_shm_destroy(producer);

  const auto cross = name + "-cross";
  CHECK(awj_shm_create(context, cross.c_str(), 1, 4096, &producer) == AWJ_OK);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_OK);
  std::memset(slot.data, 42, 16); fill_desc(desc, slot.data, slot.capacity);
  CHECK(awj_shm_commit(producer, slot.token, &desc) == AWJ_OK);
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  CHECK(length > 0 && length < executable.size()); executable.resize(length);
  std::wstring command = L"\"" + executable + L"\" --consumer " + std::wstring(cross.begin(), cross.end());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
  const auto waited = WaitForSingleObject(process.hProcess, 10000);
  if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 99);
  DWORD code = 99; GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  CHECK(waited == WAIT_OBJECT_0 && code == 0);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_END_OF_STREAM);
  awj_shm_destroy(producer);
  CHECK(abrupt_test(executable, name + "-dead-producer", true, context) == 0);
  CHECK(abrupt_test(executable, name + "-dead-consumer", false, context) == 0);
  awj_context_destroy(context);
  std::puts("共享槽位背压、归还复用、跨进程传输、生产者/消费者异常退出及已借出帧保活通过。");
  return 0;
}
