module;

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "awj_visual_metric_shaders.hpp"

export module awj.visual_metrics_gpu;

import awj.encoding_defaults;
import awj.image;
import awj.visual_metrics;

namespace awj::visual_metrics_gpu_detail {

using Clock = std::chrono::steady_clock;

PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers{};
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets{};
PFN_vkAllocateMemory vkAllocateMemory{};
PFN_vkBeginCommandBuffer vkBeginCommandBuffer{};
PFN_vkBindBufferMemory vkBindBufferMemory{};
PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets{};
PFN_vkCmdBindPipeline vkCmdBindPipeline{};
PFN_vkCmdDispatch vkCmdDispatch{};
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier{};
PFN_vkCreateBuffer vkCreateBuffer{};
PFN_vkCreateCommandPool vkCreateCommandPool{};
PFN_vkCreateComputePipelines vkCreateComputePipelines{};
PFN_vkCreateDescriptorPool vkCreateDescriptorPool{};
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout{};
PFN_vkCreateDevice vkCreateDevice{};
PFN_vkCreateFence vkCreateFence{};
PFN_vkCreateInstance vkCreateInstance{};
PFN_vkCreatePipelineLayout vkCreatePipelineLayout{};
PFN_vkCreateShaderModule vkCreateShaderModule{};
PFN_vkDestroyBuffer vkDestroyBuffer{};
PFN_vkDestroyCommandPool vkDestroyCommandPool{};
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool{};
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout{};
PFN_vkDestroyDevice vkDestroyDevice{};
PFN_vkDestroyFence vkDestroyFence{};
PFN_vkDestroyInstance vkDestroyInstance{};
PFN_vkDestroyPipeline vkDestroyPipeline{};
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout{};
PFN_vkDestroyShaderModule vkDestroyShaderModule{};
PFN_vkDeviceWaitIdle vkDeviceWaitIdle{};
PFN_vkEndCommandBuffer vkEndCommandBuffer{};
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices{};
PFN_vkFreeCommandBuffers vkFreeCommandBuffers{};
PFN_vkFreeMemory vkFreeMemory{};
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements{};
PFN_vkGetDeviceQueue vkGetDeviceQueue{};
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties{};
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties{};
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties{};
PFN_vkMapMemory vkMapMemory{};
PFN_vkQueueSubmit vkQueueSubmit{};
PFN_vkResetDescriptorPool vkResetDescriptorPool{};
PFN_vkUnmapMemory vkUnmapMemory{};
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets{};
PFN_vkWaitForFences vkWaitForFences{};

std::expected<void, std::string> load_vulkan() {
  // The cached device and this dispatch table share the process lifetime.
  static void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) return std::unexpected{"Vulkan loader 不可用，使用 CPU 指标。"};
#define AWJ_LOAD_VK(name) \
  name = reinterpret_cast<PFN_##name>(dlsym(library, #name)); \
  if (!name) return std::unexpected{"Vulkan 入口 " #name " 不可用，使用 CPU 指标。"}
  AWJ_LOAD_VK(vkAllocateCommandBuffers);
  AWJ_LOAD_VK(vkAllocateDescriptorSets);
  AWJ_LOAD_VK(vkAllocateMemory);
  AWJ_LOAD_VK(vkBeginCommandBuffer);
  AWJ_LOAD_VK(vkBindBufferMemory);
  AWJ_LOAD_VK(vkCmdBindDescriptorSets);
  AWJ_LOAD_VK(vkCmdBindPipeline);
  AWJ_LOAD_VK(vkCmdDispatch);
  AWJ_LOAD_VK(vkCmdPipelineBarrier);
  AWJ_LOAD_VK(vkCreateBuffer);
  AWJ_LOAD_VK(vkCreateCommandPool);
  AWJ_LOAD_VK(vkCreateComputePipelines);
  AWJ_LOAD_VK(vkCreateDescriptorPool);
  AWJ_LOAD_VK(vkCreateDescriptorSetLayout);
  AWJ_LOAD_VK(vkCreateDevice);
  AWJ_LOAD_VK(vkCreateFence);
  AWJ_LOAD_VK(vkCreateInstance);
  AWJ_LOAD_VK(vkCreatePipelineLayout);
  AWJ_LOAD_VK(vkCreateShaderModule);
  AWJ_LOAD_VK(vkDestroyBuffer);
  AWJ_LOAD_VK(vkDestroyCommandPool);
  AWJ_LOAD_VK(vkDestroyDescriptorPool);
  AWJ_LOAD_VK(vkDestroyDescriptorSetLayout);
  AWJ_LOAD_VK(vkDestroyDevice);
  AWJ_LOAD_VK(vkDestroyFence);
  AWJ_LOAD_VK(vkDestroyInstance);
  AWJ_LOAD_VK(vkDestroyPipeline);
  AWJ_LOAD_VK(vkDestroyPipelineLayout);
  AWJ_LOAD_VK(vkDestroyShaderModule);
  AWJ_LOAD_VK(vkDeviceWaitIdle);
  AWJ_LOAD_VK(vkEndCommandBuffer);
  AWJ_LOAD_VK(vkEnumeratePhysicalDevices);
  AWJ_LOAD_VK(vkFreeCommandBuffers);
  AWJ_LOAD_VK(vkFreeMemory);
  AWJ_LOAD_VK(vkGetBufferMemoryRequirements);
  AWJ_LOAD_VK(vkGetDeviceQueue);
  AWJ_LOAD_VK(vkGetPhysicalDeviceProperties);
  AWJ_LOAD_VK(vkGetPhysicalDeviceMemoryProperties);
  AWJ_LOAD_VK(vkGetPhysicalDeviceQueueFamilyProperties);
  AWJ_LOAD_VK(vkMapMemory);
  AWJ_LOAD_VK(vkQueueSubmit);
  AWJ_LOAD_VK(vkResetDescriptorPool);
  AWJ_LOAD_VK(vkUnmapMemory);
  AWJ_LOAD_VK(vkUpdateDescriptorSets);
  AWJ_LOAD_VK(vkWaitForFences);
#undef AWJ_LOAD_VK
  return {};
}


constexpr std::size_t kMinimumGpuSessionPixelCount = 1ull * 1000ull * 1000ull;
constexpr std::size_t kMinimumGpuOneShotPixelCount = 2ull * 1000ull * 1000ull;
constexpr std::array<double, 5> kMsSsimWeights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
constexpr std::uint32_t kDescriptorUniform = 0;
constexpr std::uint32_t kDescriptorRead0 = 8;
constexpr std::uint32_t kDescriptorRead1 = 9;
constexpr std::uint32_t kDescriptorWrite0 = 16;
constexpr std::uint32_t kMaxDispatchGroups = 65535;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

std::uint32_t dispatch_group_count(std::uint32_t dimension) noexcept {
  return dimension / 16u + (dimension % 16u == 0 ? 0u : 1u);
}

bool should_use_gpu_session(std::size_t width, std::size_t height) noexcept {
  return width != 0 && height != 0 && width <= std::numeric_limits<std::size_t>::max() / height &&
         width * height >= kMinimumGpuSessionPixelCount;
}

std::string vk_error(std::string_view action, VkResult result) {
  return std::format("Vulkan {} 失败 (VkResult={})", action, static_cast<int>(result));
}

std::expected<void, std::string> check_vk(VkResult result, std::string_view action) {
  if (result != VK_SUCCESS) return std::unexpected{vk_error(action, result)};
  return {};
}

struct LumaConstants { std::uint32_t width{}, height{}, stride{}, channels{}; };
struct GmsdConstants { std::uint32_t width{}, height{}, group_count_x{}, reserved0{}; };
struct GmsdPartial { float sum{}, sum_sq{}, count{}, padding{}; };
struct DownsampleConstants { std::uint32_t source_width{}, source_height{}, output_width{}, output_height{}; };
struct SsimConstants { std::uint32_t width{}, height{}, group_count_x{}, reserved0{}; };
struct SsimPartial { float sum_ref{}, sum_candidate{}, sum_ref_sq{}, sum_candidate_sq{}, sum_cross{}, count{}, padding0{}, padding1{}; };
struct MetricLevelInfo { std::uint32_t width{}, height{}, groups_x{}, groups_y{}; std::size_t pixel_count{}; };

struct VulkanBuffer {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkDeviceSize byte_count{};
  void destroy(VkDevice device) noexcept {
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
    buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; byte_count = 0;
  }
};

struct VulkanMetricContext {
  VkInstance instance{};
  VkPhysicalDevice physical_device{};
  VkDevice device{};
  VkQueue queue{};
  std::uint32_t queue_family{};
  VkCommandPool command_pool{};
  VkDescriptorSetLayout descriptor_set_layout{};
  VkPipelineLayout pipeline_layout{};
  VkDescriptorPool descriptor_pool{};
  VkPipeline luma_pipeline{}, gmsd_pipeline{}, downsample_pipeline{}, ssim_pipeline{};
  VulkanBuffer uniform_buffer{}, dummy_buffer{};
  VkPhysicalDeviceMemoryProperties memory_properties{};
  std::mutex mutex{};
  ~VulkanMetricContext() { destroy(); }
  void destroy() noexcept {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
      uniform_buffer.destroy(device); dummy_buffer.destroy(device);
      for (VkPipeline pipeline : {luma_pipeline, gmsd_pipeline, downsample_pipeline, ssim_pipeline}) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
      }
      if (descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
      if (pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
      if (descriptor_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
      if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
  }
};

struct GpuSessionState {
  VulkanMetricContext* gpu{};
  std::size_t width{}, height{};
  VulkanBuffer luma_input{}, reference_luma{}, candidate_luma{};
  std::array<VulkanBuffer, 4> reference_ms_ssim_levels{}, candidate_ms_ssim_levels{};
  std::array<MetricLevelInfo, 5> ms_ssim_levels{};
  VulkanBuffer gmsd_partial{}, ssim_partial{};
  std::vector<std::uint32_t> input_words{};
  bool candidate_luma_ready{}, reference_ms_ssim_ready{};
  ~GpuSessionState() {
    if (gpu != nullptr && gpu->device != VK_NULL_HANDLE) {
      luma_input.destroy(gpu->device); reference_luma.destroy(gpu->device); candidate_luma.destroy(gpu->device);
      for (auto& b : reference_ms_ssim_levels) b.destroy(gpu->device);
      for (auto& b : candidate_ms_ssim_levels) b.destroy(gpu->device);
      gmsd_partial.destroy(gpu->device); ssim_partial.destroy(gpu->device);
    }
  }
};

std::uint32_t find_memory_type(const VulkanMetricContext& gpu, std::uint32_t type_bits, VkMemoryPropertyFlags flags) {
  for (std::uint32_t i = 0; i < gpu.memory_properties.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) != 0 && (gpu.memory_properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  return std::numeric_limits<std::uint32_t>::max();
}

std::expected<void, std::string> ensure_buffer(VulkanMetricContext& gpu, VulkanBuffer& slot, VkDeviceSize byte_count,
                                               VkBufferUsageFlags usage, std::string_view label) {
  if (byte_count == 0 || byte_count > encoding_defaults::effective_max_input_file_bytes()) return std::unexpected{std::format("Vulkan {} buffer 尺寸超过运行时限制。", label)};
  if (slot.buffer != VK_NULL_HANDLE && slot.byte_count >= byte_count) return {};
  slot.destroy(gpu.device);
  VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = byte_count, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  if (auto ok = check_vk(vkCreateBuffer(gpu.device, &buffer_info, nullptr, &slot.buffer), std::format("创建 {} buffer", label)); !ok) return ok;
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(gpu.device, slot.buffer, &requirements);
  const auto memory_type = find_memory_type(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) return std::unexpected{std::format("Vulkan {} 找不到 host-visible buffer 内存。", label)};
  VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = memory_type};
  if (auto ok = check_vk(vkAllocateMemory(gpu.device, &alloc, nullptr, &slot.memory), std::format("分配 {} buffer", label)); !ok) return ok;
  if (auto ok = check_vk(vkBindBufferMemory(gpu.device, slot.buffer, slot.memory, 0), std::format("绑定 {} buffer", label)); !ok) return ok;
  slot.byte_count = byte_count;
  return {};
}

std::expected<void, std::string> write_buffer(VulkanMetricContext& gpu, VulkanBuffer& buffer, const void* data, std::size_t byte_count, std::string_view label) {
  if (byte_count > buffer.byte_count) return std::unexpected{std::format("Vulkan {} 写入超过 buffer 容量。", label)};
  void* mapped{};
  if (auto ok = check_vk(vkMapMemory(gpu.device, buffer.memory, 0, byte_count, 0, &mapped), std::format("映射 {} buffer", label)); !ok) return ok;
  std::memcpy(mapped, data, byte_count);
  vkUnmapMemory(gpu.device, buffer.memory);
  return {};
}

template <class T>
std::expected<std::vector<T>, std::string> read_buffer(VulkanMetricContext& gpu, VulkanBuffer& buffer, std::size_t count, std::string_view label) {
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(T) || count * sizeof(T) > buffer.byte_count) return std::unexpected{std::format("Vulkan {} 回读超过 buffer 容量。", label)};
  std::vector<T> out(count);
  void* mapped{};
  const auto byte_count = count * sizeof(T);
  if (auto ok = check_vk(vkMapMemory(gpu.device, buffer.memory, 0, byte_count, 0, &mapped), std::format("映射 {} 回读 buffer", label)); !ok) return std::unexpected{ok.error()};
  std::memcpy(out.data(), mapped, byte_count);
  vkUnmapMemory(gpu.device, buffer.memory);
  return out;
}

std::expected<VkShaderModule, std::string> create_shader_module(VulkanMetricContext& gpu, const unsigned char* bytes, std::size_t byte_count, std::string_view label) {
  if (byte_count == 0 || byte_count % 4 != 0) return std::unexpected{std::format("Vulkan {} SPIR-V bytecode 无效。", label)};
  std::vector<std::uint32_t> words(byte_count / 4);
  std::memcpy(words.data(), bytes, byte_count);
  if (words.front() != 0x07230203u) return std::unexpected{std::format("Vulkan {} shader 不是有效 SPIR-V。", label)};
  VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = byte_count, .pCode = words.data()};
  VkShaderModule module{};
  if (auto ok = check_vk(vkCreateShaderModule(gpu.device, &info, nullptr, &module), std::format("创建 {} shader", label)); !ok) return std::unexpected{ok.error()};
  return module;
}

std::expected<VkPipeline, std::string> create_compute_pipeline(VulkanMetricContext& gpu, const unsigned char* bytes, std::size_t byte_count, std::string_view label) {
  auto module = create_shader_module(gpu, bytes, byte_count, label);
  if (!module) return std::unexpected{module.error()};
  VkPipelineShaderStageCreateInfo stage{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = *module, .pName = "main"};
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = stage, .layout = gpu.pipeline_layout};
  VkPipeline pipeline{};
  auto result = vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  vkDestroyShaderModule(gpu.device, *module, nullptr);
  if (result != VK_SUCCESS) return std::unexpected{vk_error(std::format("创建 {} pipeline", label), result)};
  return pipeline;
}

std::expected<void, std::string> create_descriptor_state(VulkanMetricContext& gpu) {
  const std::array bindings{
      VkDescriptorSetLayoutBinding{.binding = kDescriptorUniform, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      VkDescriptorSetLayoutBinding{.binding = kDescriptorRead0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      VkDescriptorSetLayoutBinding{.binding = kDescriptorRead1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      VkDescriptorSetLayoutBinding{.binding = kDescriptorWrite0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
  VkDescriptorSetLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = static_cast<std::uint32_t>(bindings.size()), .pBindings = bindings.data()};
  if (auto ok = check_vk(vkCreateDescriptorSetLayout(gpu.device, &layout_info, nullptr, &gpu.descriptor_set_layout), "创建 descriptor layout"); !ok) return ok;
  VkPipelineLayoutCreateInfo pipeline_layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &gpu.descriptor_set_layout};
  if (auto ok = check_vk(vkCreatePipelineLayout(gpu.device, &pipeline_layout, nullptr, &gpu.pipeline_layout), "创建 pipeline layout"); !ok) return ok;
  const std::array pool_sizes{VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 64}, VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 192}};
  VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64, .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()), .pPoolSizes = pool_sizes.data()};
  return check_vk(vkCreateDescriptorPool(gpu.device, &pool, nullptr, &gpu.descriptor_pool), "创建 descriptor pool");
}

std::expected<VkDescriptorSet, std::string> allocate_descriptor_set(VulkanMetricContext& gpu) {
  VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = gpu.descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &gpu.descriptor_set_layout};
  VkDescriptorSet set{};
  if (auto ok = check_vk(vkAllocateDescriptorSets(gpu.device, &alloc, &set), "分配 descriptor set"); !ok) return std::unexpected{ok.error()};
  return set;
}

std::expected<void, std::string> run_compute(VulkanMetricContext& gpu, VkPipeline pipeline, const void* constants, std::size_t constants_size,
                                             VulkanBuffer& read0, VulkanBuffer* read1, VulkanBuffer& write0,
                                             std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z,
                                             std::string_view label) {
  if (groups_x == 0 || groups_y == 0 || groups_z == 0 || groups_x > kMaxDispatchGroups || groups_y > kMaxDispatchGroups || groups_z > kMaxDispatchGroups) return std::unexpected{std::format("Vulkan {} dispatch 尺寸超过限制。", label)};
  if (auto ok = ensure_buffer(gpu, gpu.uniform_buffer, constants_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "uniform"); !ok) return ok;
  if (auto ok = write_buffer(gpu, gpu.uniform_buffer, constants, constants_size, "uniform"); !ok) return ok;
  auto descriptor = allocate_descriptor_set(gpu);
  if (!descriptor) return std::unexpected{descriptor.error()};
  const VkDescriptorBufferInfo uniform_info{.buffer = gpu.uniform_buffer.buffer, .offset = 0, .range = constants_size};
  const VkDescriptorBufferInfo read0_info{.buffer = read0.buffer, .offset = 0, .range = read0.byte_count};
  const VkDescriptorBufferInfo read1_info{.buffer = read1 != nullptr ? read1->buffer : gpu.dummy_buffer.buffer, .offset = 0, .range = read1 != nullptr ? read1->byte_count : gpu.dummy_buffer.byte_count};
  const VkDescriptorBufferInfo write_info{.buffer = write0.buffer, .offset = 0, .range = write0.byte_count};
  const std::array writes{
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = *descriptor, .dstBinding = kDescriptorUniform, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &uniform_info},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = *descriptor, .dstBinding = kDescriptorRead0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &read0_info},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = *descriptor, .dstBinding = kDescriptorRead1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &read1_info},
      VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = *descriptor, .dstBinding = kDescriptorWrite0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &write_info}};
  vkUpdateDescriptorSets(gpu.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
  VkCommandBufferAllocateInfo cmd_alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = gpu.command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer cmd{};
  if (auto ok = check_vk(vkAllocateCommandBuffers(gpu.device, &cmd_alloc, &cmd), std::format("分配 {} command buffer", label)); !ok) return ok;
  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if (auto ok = check_vk(vkBeginCommandBuffer(cmd, &begin), std::format("开始 {} command buffer", label)); !ok) return ok;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline_layout, 0, 1, &*descriptor, 0, nullptr);
  vkCmdDispatch(cmd, groups_x, groups_y, groups_z);
  VkBufferMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = write0.buffer, .offset = 0, .size = VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
  if (auto ok = check_vk(vkEndCommandBuffer(cmd), std::format("结束 {} command buffer", label)); !ok) return ok;
  VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{};
  if (auto ok = check_vk(vkCreateFence(gpu.device, &fence_info, nullptr, &fence), std::format("创建 {} fence", label)); !ok) return ok;
  VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
  VkResult result = vkQueueSubmit(gpu.queue, 1, &submit, fence);
  if (result == VK_SUCCESS) result = vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
  vkDestroyFence(gpu.device, fence, nullptr);
  vkFreeCommandBuffers(gpu.device, gpu.command_pool, 1, &cmd);
  vkResetDescriptorPool(gpu.device, gpu.descriptor_pool, 0);
  if (result != VK_SUCCESS) return std::unexpected{vk_error(std::format("执行 {} dispatch", label), result)};
  return {};
}

std::expected<std::unique_ptr<VulkanMetricContext>, std::string> create_context() {
  if (auto loaded = load_vulkan(); !loaded) return std::unexpected{loaded.error()};
  auto gpu = std::make_unique<VulkanMetricContext>();
  VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "AWJimage", .applicationVersion = 1, .pEngineName = "AWJimage", .engineVersion = 1, .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  if (auto ok = check_vk(vkCreateInstance(&instance_info, nullptr, &gpu->instance), "创建 instance"); !ok) return std::unexpected{ok.error()};
  std::uint32_t physical_count{};
  if (auto ok = check_vk(vkEnumeratePhysicalDevices(gpu->instance, &physical_count, nullptr), "枚举 physical device"); !ok) return std::unexpected{ok.error()};
  if (physical_count == 0) return std::unexpected{"Vulkan 未找到可用 GPU。"};
  std::vector<VkPhysicalDevice> physical_devices(physical_count);
  if (auto ok = check_vk(vkEnumeratePhysicalDevices(gpu->instance, &physical_count, physical_devices.data()), "读取 physical device"); !ok) return std::unexpected{ok.error()};
  for (auto physical : physical_devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
    std::uint32_t queue_count{};
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
    for (std::uint32_t i = 0; i < queue_count; ++i) if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) { gpu->physical_device = physical; gpu->queue_family = i; break; }
    if (gpu->physical_device != VK_NULL_HANDLE) break;
  }
  if (gpu->physical_device == VK_NULL_HANDLE) return std::unexpected{"Vulkan 未找到 compute queue。"};
  vkGetPhysicalDeviceMemoryProperties(gpu->physical_device, &gpu->memory_properties);
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = gpu->queue_family, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
  if (auto ok = check_vk(vkCreateDevice(gpu->physical_device, &device_info, nullptr, &gpu->device), "创建设备"); !ok) return std::unexpected{ok.error()};
  vkGetDeviceQueue(gpu->device, gpu->queue_family, 0, &gpu->queue);
  VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = gpu->queue_family};
  if (auto ok = check_vk(vkCreateCommandPool(gpu->device, &pool, nullptr, &gpu->command_pool), "创建 command pool"); !ok) return std::unexpected{ok.error()};
  if (auto ok = create_descriptor_state(*gpu); !ok) return std::unexpected{ok.error()};
  if (auto ok = ensure_buffer(*gpu, gpu->dummy_buffer, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "dummy"); !ok) return std::unexpected{ok.error()};
  std::uint32_t zero{};
  if (auto ok = write_buffer(*gpu, gpu->dummy_buffer, &zero, sizeof(zero), "dummy"); !ok) return std::unexpected{ok.error()};
  using namespace awj::visual_metrics_gpu_shaders;
  auto luma = create_compute_pipeline(*gpu, kLumaShaderBytecode, kLumaShaderBytecodeSize, "luma"); if (!luma) return std::unexpected{luma.error()}; gpu->luma_pipeline = *luma;
  auto gmsd = create_compute_pipeline(*gpu, kGmsdShaderBytecode, kGmsdShaderBytecodeSize, "GMSD"); if (!gmsd) return std::unexpected{gmsd.error()}; gpu->gmsd_pipeline = *gmsd;
  auto downsample = create_compute_pipeline(*gpu, kDownsampleShaderBytecode, kDownsampleShaderBytecodeSize, "downsample"); if (!downsample) return std::unexpected{downsample.error()}; gpu->downsample_pipeline = *downsample;
  auto ssim = create_compute_pipeline(*gpu, kMsSsimShaderBytecode, kMsSsimShaderBytecodeSize, "MS-SSIM"); if (!ssim) return std::unexpected{ssim.error()}; gpu->ssim_pipeline = *ssim;
  return gpu;
}

std::expected<VulkanMetricContext*, std::string> shared_metric_context() {
  static const auto context = create_context();
  if (!context) return std::unexpected{context.error()};
  return context->get();
}

struct LumaDispatchInfo { std::size_t pixel_count{}, input_byte_count{}, input_word_count{}; std::uint32_t width{}, height{}, stride{}, channels{}, groups_x{}, groups_y{}; };

std::expected<LumaDispatchInfo, std::string> describe_luma_dispatch(const ImageBuffer& image) {
  if (image.bit_depth != 8 || image.planes.empty() || (image.pixel_format != PixelFormat::rgba && image.pixel_format != PixelFormat::rgb)) return std::unexpected{"Vulkan luma 仅支持 8-bit RGB/RGBA 图像。"};
  if (image.width == 0 || image.height == 0 || image.width > std::numeric_limits<std::uint32_t>::max() || image.height > std::numeric_limits<std::uint32_t>::max() || image.width > std::numeric_limits<std::size_t>::max() / image.height) return std::unexpected{"Vulkan luma 输入尺寸无效。"};
  const auto& plane = image.planes.front();
  const std::size_t channels = image.pixel_format == PixelFormat::rgba ? 4 : 3;
  if (image.width > std::numeric_limits<std::size_t>::max() / channels) return std::unexpected{"Vulkan luma 输入宽度过大。"};
  const std::size_t min_stride = image.width * channels;
  if (!plane.contains_rows(image.height, min_stride) || plane.stride > std::numeric_limits<std::size_t>::max() / image.height || plane.stride > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Vulkan luma 输入 buffer 尺寸无效。"};
  const auto pixel_count = image.width * image.height;
  const auto input_byte_count = plane.stride * (image.height - 1) + min_stride;
  const auto input_word_count = input_byte_count / sizeof(std::uint32_t) + (input_byte_count % sizeof(std::uint32_t) == 0 ? 0u : 1u);
  if (input_byte_count > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Vulkan luma 输入 buffer 超过 shader 32-bit 地址限制。"};
  if (pixel_count > std::numeric_limits<std::uint32_t>::max() || input_word_count > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Vulkan luma buffer 超过 GPU 资源限制。"};
  const std::uint32_t width = static_cast<std::uint32_t>(image.width);
  const std::uint32_t height = static_cast<std::uint32_t>(image.height);
  return LumaDispatchInfo{.pixel_count = pixel_count, .input_byte_count = input_byte_count, .input_word_count = input_word_count, .width = width, .height = height, .stride = static_cast<std::uint32_t>(plane.stride), .channels = static_cast<std::uint32_t>(channels), .groups_x = dispatch_group_count(width), .groups_y = dispatch_group_count(height)};
}

std::expected<LumaDispatchInfo, std::string> write_luma_buffer(GpuSessionState& session, const ImageBuffer& image, VulkanBuffer& output) {
  auto info = describe_luma_dispatch(image);
  if (!info) return std::unexpected{info.error()};
  auto& gpu = *session.gpu;
  session.input_words.assign(info->input_word_count, 0u);
  std::memcpy(session.input_words.data(), image.planes.front().read_bytes().data(), info->input_byte_count);
  if (auto ok = ensure_buffer(gpu, session.luma_input, info->input_word_count * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "luma input"); !ok) return std::unexpected{ok.error()};
  if (auto ok = ensure_buffer(gpu, output, info->pixel_count * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "luma output"); !ok) return std::unexpected{ok.error()};
  if (auto ok = write_buffer(gpu, session.luma_input, session.input_words.data(), info->input_word_count * sizeof(std::uint32_t), "luma input"); !ok) return std::unexpected{ok.error()};
  const LumaConstants constants{.width = info->width, .height = info->height, .stride = info->stride, .channels = info->channels};
  if (auto ok = run_compute(gpu, gpu.luma_pipeline, &constants, sizeof(constants), session.luma_input, nullptr, output, info->groups_x, info->groups_y, 1, "luma"); !ok) return std::unexpected{ok.error()};
  return *info;
}

std::expected<void, std::string> prepare_ms_ssim_levels(GpuSessionState& session, VulkanBuffer& base, std::array<VulkanBuffer, 4>& levels) {
  auto& gpu = *session.gpu;
  session.ms_ssim_levels[0] = MetricLevelInfo{.width = static_cast<std::uint32_t>(session.width), .height = static_cast<std::uint32_t>(session.height), .groups_x = dispatch_group_count(static_cast<std::uint32_t>(session.width)), .groups_y = dispatch_group_count(static_cast<std::uint32_t>(session.height)), .pixel_count = session.width * session.height};
  VulkanBuffer* source = &base;
  for (std::size_t level = 1; level < kMsSsimWeights.size(); ++level) {
    const auto& previous = session.ms_ssim_levels[level - 1];
    if (previous.width <= 1 || previous.height <= 1) { session.ms_ssim_levels[level] = previous; continue; }
    const std::uint32_t width = (previous.width + 1u) / 2u;
    const std::uint32_t height = (previous.height + 1u) / 2u;
    const auto pixel_count = static_cast<std::size_t>(width) * height;
    if (auto ok = ensure_buffer(gpu, levels[level - 1], pixel_count * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "MS-SSIM level"); !ok) return std::unexpected{ok.error()};
    const DownsampleConstants constants{.source_width = previous.width, .source_height = previous.height, .output_width = width, .output_height = height};
    if (auto ok = run_compute(gpu, gpu.downsample_pipeline, &constants, sizeof(constants), *source, nullptr, levels[level - 1], dispatch_group_count(width), dispatch_group_count(height), 1, "downsample"); !ok) return std::unexpected{ok.error()};
    session.ms_ssim_levels[level] = MetricLevelInfo{.width = width, .height = height, .groups_x = dispatch_group_count(width), .groups_y = dispatch_group_count(height), .pixel_count = pixel_count};
    source = &levels[level - 1];
  }
  return {};
}

VulkanBuffer& level_buffer(VulkanBuffer& base, std::array<VulkanBuffer, 4>& levels, const std::array<MetricLevelInfo, 5>& infos, std::size_t level) noexcept {
  if (level == 0) return base;
  if (infos[level].width == infos[level - 1].width && infos[level].height == infos[level - 1].height) return level_buffer(base, levels, infos, level - 1);
  return levels[level - 1];
}

double ssim_from_partials(double sum_ref, double sum_candidate, double sum_ref_sq, double sum_candidate_sq, double sum_cross, double count) noexcept {
  if (count <= 0.0) return 1.0;
  const double mean_ref = sum_ref / count;
  const double mean_candidate = sum_candidate / count;
  const double variance_ref = std::max(0.0, sum_ref_sq / count - mean_ref * mean_ref);
  const double variance_candidate = std::max(0.0, sum_candidate_sq / count - mean_candidate * mean_candidate);
  const double covariance = sum_cross / count - mean_ref * mean_candidate;
  constexpr double k1 = 0.01, k2 = 0.03, c1 = k1 * k1, c2 = k2 * k2;
  const double numerator = (2.0 * mean_ref * mean_candidate + c1) * (2.0 * covariance + c2);
  const double denominator = (mean_ref * mean_ref + mean_candidate * mean_candidate + c1) * (variance_ref + variance_candidate + c2);
  return denominator <= 0.0 ? 1.0 : std::clamp(numerator / denominator, 0.0, 1.0);
}

std::expected<double, std::string> compute_gmsd_session(GpuSessionState& session) {
  if (!session.candidate_luma_ready) return std::unexpected{"Vulkan GMSD candidate luma 尚未准备好。"};
  auto& gpu = *session.gpu;
  const std::uint32_t width = static_cast<std::uint32_t>(session.width);
  const std::uint32_t height = static_cast<std::uint32_t>(session.height);
  const auto groups_x = dispatch_group_count(width);
  const auto groups_y = dispatch_group_count(height);
  const auto partial_count = static_cast<std::size_t>(groups_x) * groups_y;
  if (auto ok = ensure_buffer(gpu, session.gmsd_partial, partial_count * sizeof(GmsdPartial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "GMSD partial"); !ok) return std::unexpected{ok.error()};
  const GmsdConstants constants{.width = width, .height = height, .group_count_x = groups_x};
  if (auto ok = run_compute(gpu, gpu.gmsd_pipeline, &constants, sizeof(constants), session.reference_luma, &session.candidate_luma, session.gmsd_partial, groups_x, groups_y, 1, "GMSD"); !ok) return std::unexpected{ok.error()};
  auto partials = read_buffer<GmsdPartial>(gpu, session.gmsd_partial, partial_count, "GMSD");
  if (!partials) return std::unexpected{partials.error()};
  double sum = 0.0, sum_sq = 0.0, count = 0.0;
  for (const auto& partial : *partials) { sum += partial.sum; sum_sq += partial.sum_sq; count += partial.count; }
  if (count <= 0.0) return std::unexpected{"Vulkan GMSD 输入为空。"};
  const double mean = sum / count;
  return std::sqrt(std::max(0.0, sum_sq / count - mean * mean));
}

std::expected<double, std::string> compute_ms_ssim_session(GpuSessionState& session) {
  if (!session.candidate_luma_ready) return std::unexpected{"Vulkan MS-SSIM candidate luma 尚未准备好。"};
  auto& gpu = *session.gpu;
  if (!session.reference_ms_ssim_ready) { if (auto ok = prepare_ms_ssim_levels(session, session.reference_luma, session.reference_ms_ssim_levels); !ok) return std::unexpected{ok.error()}; session.reference_ms_ssim_ready = true; }
  if (auto ok = prepare_ms_ssim_levels(session, session.candidate_luma, session.candidate_ms_ssim_levels); !ok) return std::unexpected{ok.error()};
  double value = 1.0;
  for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
    const auto& info = session.ms_ssim_levels[level];
    const auto partial_count = static_cast<std::size_t>(info.groups_x) * info.groups_y;
    if (auto ok = ensure_buffer(gpu, session.ssim_partial, partial_count * sizeof(SsimPartial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "MS-SSIM partial"); !ok) return std::unexpected{ok.error()};
    const SsimConstants constants{.width = info.width, .height = info.height, .group_count_x = info.groups_x};
    auto& ref = level_buffer(session.reference_luma, session.reference_ms_ssim_levels, session.ms_ssim_levels, level);
    auto& cand = level_buffer(session.candidate_luma, session.candidate_ms_ssim_levels, session.ms_ssim_levels, level);
    if (auto ok = run_compute(gpu, gpu.ssim_pipeline, &constants, sizeof(constants), ref, &cand, session.ssim_partial, info.groups_x, info.groups_y, 1, "MS-SSIM"); !ok) return std::unexpected{ok.error()};
    auto partials = read_buffer<SsimPartial>(gpu, session.ssim_partial, partial_count, "MS-SSIM");
    if (!partials) return std::unexpected{partials.error()};
    double sum_ref = 0.0, sum_candidate = 0.0, sum_ref_sq = 0.0, sum_candidate_sq = 0.0, sum_cross = 0.0, count = 0.0;
    for (const auto& partial : *partials) { sum_ref += partial.sum_ref; sum_candidate += partial.sum_candidate; sum_ref_sq += partial.sum_ref_sq; sum_candidate_sq += partial.sum_candidate_sq; sum_cross += partial.sum_cross; count += partial.count; }
    value *= std::pow(std::clamp(ssim_from_partials(sum_ref, sum_candidate, sum_ref_sq, sum_candidate_sq, sum_cross, count), 1e-9, 1.0), kMsSsimWeights[level]);
  }
  return std::clamp(value, 0.0, 1.0);
}

}  // namespace awj::visual_metrics_gpu_detail

export namespace awj {

struct AcceleratedVisualMetricTiming { double luma_seconds{}, gmsd_seconds{}, ms_ssim_seconds{}; };

class AcceleratedVisualMetricSession {
 public:
  AcceleratedVisualMetricSession() noexcept = default;
  AcceleratedVisualMetricSession(const AcceleratedVisualMetricSession&) = delete;
  AcceleratedVisualMetricSession& operator=(const AcceleratedVisualMetricSession&) = delete;
  AcceleratedVisualMetricSession(AcceleratedVisualMetricSession&&) noexcept = default;
  AcceleratedVisualMetricSession& operator=(AcceleratedVisualMetricSession&&) noexcept = default;
  ~AcceleratedVisualMetricSession() = default;

  static std::expected<AcceleratedVisualMetricSession, std::string> create(const ImageBuffer& reference_image) {
    if (!visual_metrics_gpu_detail::should_use_gpu_session(reference_image.width, reference_image.height)) return std::unexpected{"Vulkan visual metric session 已对小图禁用。"};
    auto shared_context = visual_metrics_gpu_detail::shared_metric_context();
    if (!shared_context) return std::unexpected{shared_context.error()};
    AcceleratedVisualMetricSession session;
    auto state_holder = std::make_unique<visual_metrics_gpu_detail::GpuSessionState>();
    auto& state = *state_holder;
    state.gpu = *shared_context; state.width = reference_image.width; state.height = reference_image.height;
    std::lock_guard lock{state.gpu->mutex};
    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, reference_image, state.reference_luma);
    if (!info) return std::unexpected{info.error()};
    session.state_.reset(state_holder.release());
    return std::move(session);
  }

  std::expected<void, std::string> prepare_candidate_luma(const ImageBuffer& candidate_image) {
    if (state_ == nullptr) return std::unexpected{"Vulkan visual metric session 不可用。"};
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    state.candidate_luma_ready = false;
    if (candidate_image.width != state.width || candidate_image.height != state.height) return std::unexpected{"Vulkan metric 输入尺寸不一致。"};
    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, candidate_image, state.candidate_luma);
    if (!info) return std::unexpected{info.error()};
    state.candidate_luma_ready = true;
    return {};
  }

  std::expected<LumaImage, std::string> make_candidate_luma(const ImageBuffer& candidate_image) { return make_luma_image(candidate_image); }
  std::expected<double, std::string> compute_gmsd() { if (state_ == nullptr) return std::unexpected{"Vulkan visual metric session 不可用。"}; auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get()); std::lock_guard lock{state.gpu->mutex}; return visual_metrics_gpu_detail::compute_gmsd_session(state); }
  std::expected<double, std::string> compute_ms_ssim() { if (state_ == nullptr) return std::unexpected{"Vulkan visual metric session 不可用。"}; auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get()); std::lock_guard lock{state.gpu->mutex}; return visual_metrics_gpu_detail::compute_ms_ssim_session(state); }

  std::expected<VisualMetricResult, std::string> calculate_candidate_metrics(const ImageBuffer& candidate_image, AcceleratedVisualMetricTiming* timing = nullptr) {
    if (state_ == nullptr) return std::unexpected{"Vulkan visual metric session 不可用。"};
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    state.candidate_luma_ready = false;
    if (candidate_image.width != state.width || candidate_image.height != state.height) return std::unexpected{"Vulkan metric 输入尺寸不一致。"};
    auto started = visual_metrics_gpu_detail::Clock::now();
    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, candidate_image, state.candidate_luma);
    if (timing != nullptr) timing->luma_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    if (!info) return std::unexpected{info.error()};
    state.candidate_luma_ready = true;
    started = visual_metrics_gpu_detail::Clock::now();
    auto gmsd = visual_metrics_gpu_detail::compute_gmsd_session(state);
    if (timing != nullptr) timing->gmsd_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    if (!gmsd) return std::unexpected{gmsd.error()};
    started = visual_metrics_gpu_detail::Clock::now();
    auto ms_ssim = visual_metrics_gpu_detail::compute_ms_ssim_session(state);
    if (timing != nullptr) timing->ms_ssim_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    if (!ms_ssim) return std::unexpected{ms_ssim.error()};
    return make_visual_metric_result(*gmsd, *ms_ssim);
  }

  std::expected<std::vector<VisualMetricResult>, std::string> calculate_candidate_metrics_batch(std::span<const ImageBuffer> candidate_images, AcceleratedVisualMetricTiming* timing = nullptr) {
    if (candidate_images.empty()) return std::unexpected{"Vulkan visual metric batch 输入为空。"};
    std::vector<VisualMetricResult> results;
    results.reserve(candidate_images.size());
    for (const auto& image : candidate_images) { auto single = calculate_candidate_metrics(image, timing); if (!single) return std::unexpected{single.error()}; results.push_back(*single); }
    return results;
  }

 private:
  struct StateDeleter { void operator()(void* value) const noexcept; };
  using StatePtr = std::unique_ptr<void, StateDeleter>;
  StatePtr state_{};
};

void AcceleratedVisualMetricSession::StateDeleter::operator()(void* value) const noexcept { delete static_cast<visual_metrics_gpu_detail::GpuSessionState*>(value); }

std::expected<LumaImage, std::string> make_luma_image_accelerated(const ImageBuffer& image) { return make_luma_image(image); }
std::expected<double, std::string> compute_gmsd_accelerated(const LumaImage& reference, const LumaImage& candidate) { return compute_gmsd(reference, candidate); }
std::expected<double, std::string> compute_ms_ssim_accelerated(const LumaImage& reference, const LumaImage& candidate) { return compute_ms_ssim(reference, candidate); }
std::expected<VisualMetricResult, std::string> calculate_visual_metrics_accelerated(const LumaImage& reference, const LumaImage& candidate) { return calculate_visual_metrics_cpu(reference, candidate); }

}  // namespace awj
