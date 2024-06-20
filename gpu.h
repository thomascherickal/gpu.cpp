#ifndef GPU_H
#define GPU_H

#include <array>
#include <cassert>
#include <cstring>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "utils/logging.h"
#include "webgpu/webgpu.h"

namespace gpu {

static Logger kGpuLog = {stdout, "", kInfo};

#ifdef NDEBUG
static constexpr bool kDebug = false;
#else
static constexpr bool kDebug = true;
#endif

/**
 * @brief Represents a buffer of values on the GPU.
 */
struct Array {
  WGPUBuffer buffer;
  WGPUBufferUsageFlags usage;
  size_t size; // in bytes
};

/**
 * @brief Represents the shape of a tensor.
 */
struct Shape {
  static constexpr size_t kMaxRank = 8; // Maximum rank of a tensor, avoids
                                        // dynamic allocation for shape data
  std::array<size_t, kMaxRank> data = {0};
  size_t rank = 0;
  Shape() = default;
  Shape(std::initializer_list<size_t> dims) {
    assert(dims.size() <= kMaxRank);
    std::copy(dims.begin(), dims.end(), data.begin());
    rank = dims.size();
  }
  size_t &operator[](size_t index) {
    assert(index < rank);
    return data[index];
  }
  const size_t &operator[](size_t index) const {
    assert(index < rank);
    return data[index];
  }
};

size_t size(const Shape &shape) {
  size_t numels = 1;
  for (size_t i = 0; i < shape.rank; i++) {
    numels *= shape.data[i];
  }
  return numels;
}

/**
 * @brief Represents a tensor on the GPU, which is a buffer of values with a
 * shape.
 */
struct Tensor {
  Array data;
  Shape shape;
};

/**
 * @brief Represents a collection of tensors.
 *
 * Since Tensor wraps a WGPUBuffer and WGPUBuffer is effectively a reference to
 * a GPU buffer, performing operations on TensorList elements (writing /
 * copying buffers) is tantamount to working with pointers to GPU buffers.
 */
template <std::size_t N> struct TensorList {
  std::array<Tensor, N> data;
  TensorList(std::initializer_list<Tensor> init) {
    std::copy(init.begin(), init.end(), data.begin());
  }
  Tensor &operator[](std::size_t index) { return data[index]; }
  const Tensor &operator[](std::size_t index) const { return data[index]; }
};

/**
 * @brief Deduction guide for TensorList
 */
template <std::size_t N> TensorList(std::array<Tensor, N>) -> TensorList<N>;
template <typename... Args> TensorList(Args...) -> TensorList<sizeof...(Args)>;

struct Context; // Forward declaration so that TensorPool can have a pointer to
                // Context

struct TensorPool {
  TensorPool(Context *ctx) : ctx(ctx), data() {};
  Context *ctx;
  std::unordered_map<WGPUBuffer, Tensor> data;
  ~TensorPool();
};

enum NumType { kf32 };

/**
 * @brief Converts NumType to string.
 */
std::string ToString(NumType type) {
  switch (type) {
  case kf32:
    return "f32";
  default:
    log(kDefLog, kError, "Invalid NumType in string conversion.");
    return "unknown";
  }
}

/**
 * @brief Converts Shape to string. The string formatting isn't arbitrary but
 * is meant to be slotted into shader code (hence no additional parentheses or
 * brackets).
 */
std::string ToString(const Shape &shape) {
  std::string str;
  for (size_t i = 0; i < shape.rank; i++) {
    str += std::to_string(shape.data[i]);
    if (i < shape.rank - 1) {
      str += ", ";
    }
  }
  return str;
}

/**
 * @brief Represents a shader code.
 * workgroup size and precision are stored since they are specified in the
 * shader code and making the values available helps keep parameters
 * consistent.
 */
struct ShaderCode {
  ShaderCode(const std::string &data, size_t workgroupSize = 256,
             NumType precision = kf32)
      : data(data), workgroupSize({workgroupSize, 1, 1}), precision(precision) {
  }

  ShaderCode(const std::string &data, const Shape &workgroupSize,
             NumType precision = kf32)
      : data(data), workgroupSize(workgroupSize), precision(precision) {}
  std::string data;
  Shape workgroupSize;
  NumType precision;
};

/**
 * @brief Used for on-done callback data for asynchronous operations sduch as
 * kernel launching.
 */
struct CallbackDataDyn {
  WGPUBuffer buffer; // managed by owning Kernel
  size_t bufferSize;
  float *output; // non-owning, only for target memory in ToCPU, not used for
                 // kernel invocations
  std::promise<void>* promise;
  std::future<void>* future;
};

/**
 * @brief Represents handles + metadata for a reusable kernel on the GPU.
 * The struct members can be divided into "consumed upon dispatch"
 * (commandBuffer) and reusable ahead-of-time setup (all other members).
 */
struct Kernel {
  std::unique_ptr<WGPUBuffer[]> buffers; // non-owning
  std::unique_ptr<size_t[]> bufferSizes;
  size_t numBindings;
  Shape nWorkgroups;
  WGPUBindGroup bindGroup;             // persists between submission
  WGPUComputePipeline computePipeline; // persists between submission
  WGPUCommandBuffer commandBuffer;     // destroyed upon submission
};

/**
 * @brief Operator implementation to make the Kernel type hashable.
 * @param[in] lhs First Kernel instance to compare
 * @param[in] rhs Second Kernel instance to compare
 * @return True if lhs < rhs, false otherwise
 */
bool operator<(const Kernel &lhs, const Kernel &rhs) {
  return lhs.commandBuffer < rhs.commandBuffer;
}

/**
 * @brief A pool of kernels to manage GPU resources. For simple use cases this
 * is instantiated as a member in the Context struct although it's possible to
 * have multiple resource pools of kernels in more complex scenarios.
 */
struct KernelPool {
  KernelPool(Context *ctx) : ctx(ctx), data() {}
  Context *ctx;
  std::set<Kernel *> data;
  ~KernelPool() {
    // Note : Some kernel resources such as commandBuffer are harvested by
    // queue submission, explicitly destroying readback and callback buffers
    // produces runtime errors.
    data.clear();
  }
};

/**
 * @brief Represents a GPU context, aggregates WebGPU API handles to interact
 * with the GPU including the instance, adapter, device, and queue.
 *
 * Additionally contains a TensorPool and KernelPool for managing GPU resources
 * to simplify lifetime management of GPU resources.
 */
struct Context {
  WGPUInstance instance;
  WGPUAdapter adapter;
  WGPUDevice device;
  WGPUQueue queue;
  TensorPool pool = TensorPool(this);
  KernelPool kernelPool = KernelPool(this);
  ~Context() {
    log(kDefLog, kInfo, "Destroying context");
    if (queue) {
      wgpuQueueRelease(queue);
      wgpuInstanceProcessEvents(instance);
    } else {
      log(kDefLog, kWarn, "Queue is null");
    }
    if (device) {
      wgpuDeviceRelease(device);
      wgpuInstanceProcessEvents(instance);
    } else {
      log(kDefLog, kWarn, "Device is null");
    }
    if (adapter) {
      wgpuAdapterRelease(adapter);
      wgpuInstanceProcessEvents(instance);
    } else {
      log(kDefLog, kWarn, "Adapter is null");
    }
    if (instance) {
      wgpuInstanceRelease(instance);
    } else {
      log(kDefLog, kWarn, "Instance is null");
    }
    log(kDefLog, kInfo, "Destroyed context");
  }
};

/**
 * @brief Tensor factory function to create a tensor (a Tensor type is simply
 * an Array with an N-dimensional  Shape specification) on the GPU. The tensor
 * is created with the given shape, data type, and usage flags, added to the
 * TensorPool, and returned.
 *
 * This is the core implementation which takes the minimal set of parameters in
 * terms of the raw WebGPU API, and is used by the other CreateTensor overloads
 * which provide more ergonomic interfaces.
 *
 * @param[in] pool TensorPool instance to manage the tensor
 * @param[in] device WGPUDevice instance to create the tensor on
 * @param[in] shape Shape of the tensor
 * @param[in] dtype Data type of the tensor (e.g. kf32)
 * @param[in] usage Usage flags for the tensor buffer
 * @return Tensor instance representing the created tensor
 * @example Tensor tensor = CreateTensor(pool, device, {256, 256}, kf32);
 */
Tensor CreateTensor(TensorPool &pool, WGPUDevice &device, const Shape &shape,
                    NumType dtype,
                    WGPUBufferUsageFlags usage = WGPUBufferUsage_Storage |
                                                 WGPUBufferUsage_CopyDst |
                                                 WGPUBufferUsage_CopySrc) {
  log(kDefLog, kTrace, "Creating tensor");
  size_t numElements = 1;
  for (size_t dim = 0; dim < shape.rank; dim++) {
    numElements *= shape.data[dim];
  }
  size_t size = dtype == kf32 ? sizeof(float) * numElements : 0;
  WGPUBufferDescriptor bufferDesc = {
      .usage = usage,
      .size = size,
  };
  WGPUBuffer buffer = wgpuDeviceCreateBuffer(device, &bufferDesc);
  pool.data[buffer] = Tensor{
      .data = Array{.buffer = buffer, .usage = usage, .size = size},
      .shape = shape,
  };
  wgpuDeviceCreateBuffer(device, &bufferDesc);
  return pool.data[buffer];
}

/**
 * @brief Overload of the tensor factory function to instantiate a tensor on
 * the GPU with a given shape and data type.
 *
 * Instead of taking the TensoPool and raw WebGPU API WGPUDevice and
 * WGPUBufferUsageFlags arguments, this is a convenience wrapper around the
 * core CreateTensor function which has default usage flags for a storage
 * buffer, and also takes in the Context object.
 *
 * instance instead of the narrower TensorPool object.
 * @param[in] ctx Context instance to manage the tensor
 * @param[in] shape Shape of the tensor
 * @param[in] dtype Data type of the tensor (e.g. kf32)
 * @return Tensor instance representing the created tensor
 * @example Tensor tensor = CreateTensor(ctx, {256, 256}, kf32);
 */
Tensor CreateTensor(Context &ctx, const Shape &shape, NumType dtype) {
  return CreateTensor(ctx.pool, ctx.device, shape, dtype);
}

/**
 * @brief Overload of the tensor factory function to instantiate a tensor on
 * the GPU with a given shape, data type. Unlike the other overloads, this
 * overload also takes initial data to populate the tensor with.
 *
 * The data is assumed to be of size equal to the product of the dimensions in
 * the shape, and is copied to the GPU buffer.
 *
 * @param[in] ctx Context instance to manage the tensor
 * @param[in] shape Shape of the tensor
 * @param[in] dtype Data type of the tensor (e.g. kf32)
 * @param[in] data Initial data to populate the tensor with
 * @return Tensor instance representing the created tensor
 * @example Tensor tensor = CreateTensor(ctx, {256, 256}, kf32, data);
 */
Tensor CreateTensor(Context &ctx, const Shape &shape, NumType dtype,
                    float *data) {
  Tensor tensor =
      CreateTensor(ctx.pool, ctx.device, shape, dtype,
                   WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                       WGPUBufferUsage_CopySrc);
  wgpuQueueWriteBuffer(ctx.queue, tensor.data.buffer, 0, data,
                       tensor.data.size);
  return tensor;
}

/**
 * @brief Frees a tensor resource and updates the tensor pool.
 *
 * Only needed if the use case requires manually managing resource lifetimes of
 * GPU tensors. For simple use cases, the TensorPool destructor will
 * automatically free all tensors.
 *
 * @param[in] pool TensorPool instance to manage the tensor
 * @param[in] tensor Tensor instance to free
 * @example FreeTensor(pool, tensor);
 */
void FreeTensor(TensorPool &pool, Tensor tensor) {
  if (tensor.data.buffer) {
    wgpuBufferRelease(tensor.data.buffer);
  } else {
    log(kDefLog, kWarn, "Tried to free tensor with null buffer");
  }
  if (pool.data.find(tensor.data.buffer) != pool.data.end()) {
    pool.data.erase(tensor.data.buffer);
  } else {
    log(kDefLog, kWarn, "Tried to free tensor that was not in pool");
  }
}

/**
 * @brief Destructor for TensorPool which frees all tensors in the pool.
 */
TensorPool::~TensorPool() {
  // Need to get keys in a separate iteration, otherwise iterator is getting
  // invalidated during erase.
  std::vector<WGPUBuffer> keys;
  for (auto &pair : data) {
    keys.push_back(pair.first);
  }
  for (auto &key : keys) {
    FreeTensor(*this, data[key]);
    log(kDefLog, kTrace, "Freed tensor");
  }
}

/**
 * @brief simple string replacement helper function for substituting
 * placeholders in a shader string template.
 *
 * Note this is not meant to be used in performance-critical code paths and
 * should be used ahead-of-time before any performance-critical codepath to
 * preprocess shader code strings.
 *
 * @param[in] str String to mutate with substitution replacements.
 * @param[in] from Substring to replace
 * @param[in] to Substring to replace with
 * @example ReplaceAll(str, "{{workgroupSize}}", "256");
 */
void ReplaceAll(std::string &str, const std::string &from,
                const std::string &to) {
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
}

/**
 * @brief Factory function to create a shader code object from a shader template
 * string and optional workgroup size and precision.
 *
 * This function replaces placeholders in the shader template string with the
 * provided workgroup size and precision, and returns a ShaderCode object.
 *
 * @param[in] shaderTemplate Shader template string with placeholders
 * @param[in] workgroupSize Shape of the workgroup. Unlike tensor shapes which
 * can be of arbitrary rank, workgroup size is always of rank 3 corresponding
 * to x y and z. workgroupSize is stored as a field in the ShaderCode instance
 * that is returned by CreateShader().
 * @param[in] precision Data type precision for the shader. As with
 * workgroupSize, precision is stored as a field in the ShaderCode instance
 * that is returned by CreateShader().
 * @example ShaderCode code = CreateShader(kPuzzle1, {256, 1, 1}, kf32);
 */
ShaderCode CreateShader(const char *shaderTemplate,
                        const Shape &workgroupSize = {256, 1, 1},
                        NumType precision = kf32) {
  std::string codeString(shaderTemplate);
  ReplaceAll(codeString, "{{workgroupSize}}", ToString(workgroupSize));
  ReplaceAll(codeString, "{{precision}}", ToString(precision));
  log(kDefLog, kInfo, "Shader code:\n%s", codeString.c_str());
  return ShaderCode{codeString, workgroupSize};
}

/**
 * @brief Overload of the factory function to create a shader code object from a
 * shader template string and workgroup size. Unlike the main factory function,
 * this overload takes a single size_t workgroupSize parameter instead of a
 * 3D shape for the workgroup size and instantiates a 3D shape with the
 * workgroupSize in the x dimension and 1 in the y and z dimensions.
 *
 * @param[in] shaderTemplate Shader template string with placeholders
 * @param[in] workgroupSize Workgroup size in the x dimension
 * @param[in] precision Data type precision for the shader
 * @example ShaderCode code = CreateShader(kPuzzle1, 256, kf32);
 */
ShaderCode CreateShader(const char *shaderTemplate, size_t workgroupSize,
                        NumType precision = kf32) {
  return CreateShader(shaderTemplate, Shape{workgroupSize, 1, 1}, precision);
}

inline void check(bool condition, const char *message,
                  const char *file = "unkown", int line = -1) {
  if constexpr (kDebug) {
    if (!condition) {
      log(kDefLog, kError, "Error in file %s line %d:\n%s", file, line,
          message);
      exit(1);
    } else {
      log(kDefLog, kTrace, "Success in file %s line %d:\n%s", file, line,
          message);
    }
  }
}

/**
 * @brief Factory function to create a GPU context, which aggregates WebGPU API
 * handles to interact with the GPU including the instance, adapter, device, and
 * queue.
 *
 * The function takes optional descriptor parameters for the instance
 * descriptor, adapter request options, and device descriptor, which are passed
 * through to the WebGPU API calls to create the instance, adapter, and device.
 *
 * If dawn is used, it also sets up an error callback for device loss.
 *
 * @param[in] desc Instance descriptor for the WebGPU instance (optional)
 * @param[in] adapterOpts Adapter request options for the WebGPU adapter
 * (optional)
 * @param[in] devDescriptor Device descriptor for the WebGPU device (optional)
 * @return Context instance representing the created GPU context
 * @example Context ctx = CreateContext();
 */
Context CreateContext(const WGPUInstanceDescriptor &desc = {},
                      const WGPURequestAdapterOptions &adapterOpts = {},
                      WGPUDeviceDescriptor devDescriptor = {}) {
  Context context;
  {
    context.instance = wgpuCreateInstance(&desc);
    check(context.instance, "Initialize WebGPU", __FILE__, __LINE__);
  }
  log(kDefLog, kInfo, "Requesting adapter");
  {
    struct AdapterData {
      WGPUAdapter adapter = nullptr;
      bool requestEnded = false;
    };
    AdapterData adapterData;
    auto onAdapterRequestEnded = [](WGPURequestAdapterStatus status,
                                    WGPUAdapter adapter, char const *message,
                                    void *pUserData) {
      AdapterData &adapterData = *reinterpret_cast<AdapterData *>(pUserData);
      check(status == WGPURequestAdapterStatus_Success,
            "Request WebGPU adapter", __FILE__, __LINE__);
      adapterData.adapter = adapter;
      adapterData.requestEnded = true;
    };
    wgpuInstanceRequestAdapter(context.instance, &adapterOpts,
                               onAdapterRequestEnded, (void *)&adapterData);
    assert(adapterData.requestEnded);
    context.adapter = adapterData.adapter;
  }
  log(kDefLog, kInfo, "Requesting device");
  {
    struct DeviceData {
      WGPUDevice device = nullptr;
      bool requestEnded = false;
    };
    DeviceData devData;
    auto onDeviceRequestEnded = [](WGPURequestDeviceStatus status,
                                   WGPUDevice device, char const *message,
                                   void *pUserData) {
      DeviceData &devData = *reinterpret_cast<DeviceData *>(pUserData);
      check(status == WGPURequestDeviceStatus_Success,
            "Could not get WebGPU device.", __FILE__, __LINE__);
      log(kDefLog, kInfo, "Device Request succeeded %s",
          static_cast<void *>(device));
      devData.device = device;
      devData.requestEnded = true;
    };

#ifdef WEBGPU_BACKEND_DAWN
    devDescriptor.deviceLostCallbackInfo = {
        .callback =
            [](WGPUDevice const *device, WGPUDeviceLostReason reason,
               char const *message, void *userdata) {
              if (reason != WGPUDeviceLostReason_Destroyed) {
                log(kDefLog, kError, "Device lost (code %d):\n%s", reason,
                    message);
              } else {
                log(kDefLog, kInfo, "Device destroyed: %s", message);
              }
            },
    };
#endif

    wgpuAdapterRequestDevice(context.adapter, &devDescriptor,
                             onDeviceRequestEnded, (void *)&devData);
    assert(devData.requestEnded);
    context.device = devData.device;
    wgpuDeviceSetUncapturedErrorCallback(
        context.device,
        [](WGPUErrorType type, char const *message, void *devData) {
          log(kDefLog, kError, "Device uncaptured error: %s", message);
          throw std::runtime_error("Device uncaptured exception.");
        },
        nullptr);
  }
  context.queue = wgpuDeviceGetQueue(context.device);
  return context;
}

void Wait(Context &ctx, std::future<void> &future) {
  while (future.wait_for(std::chrono::seconds(0)) !=
         std::future_status::ready) {
    wgpuInstanceProcessEvents(ctx.instance);
  }
}

/* Copy from GPU to CPU.
  A more performant version of this would prepare the command buffer once and
  reuse it for multiple readbacks. This version is a convenience implementation
  for non-hot paths.
*/
void ToCPU(Context &ctx, Tensor &tensor, float *data, size_t bufferSize) {
  WGPUDevice device = ctx.device;
  struct CopyOp {
    WGPUCommandBuffer commandBuffer;
    WGPUBuffer readbackBuffer;
    std::promise<void> promise;
    std::future<void> future;
    CallbackDataDyn callbackData;
  };
  CopyOp op;
  op.future = op.promise.get_future();
  {
    WGPUBufferDescriptor readbackBufferDescriptor = {
        .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
        .size = bufferSize,
    };
    op.readbackBuffer =
        wgpuDeviceCreateBuffer(device, &readbackBufferDescriptor);
  }
  {
    WGPUCommandEncoder commandEncoder;
    WGPUComputePassEncoder computePassEncoder;
    commandEncoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(commandEncoder, tensor.data.buffer, 0,
                                         op.readbackBuffer, 0, bufferSize);
    op.commandBuffer = wgpuCommandEncoderFinish(commandEncoder, nullptr);
    check(op.commandBuffer, "Create command buffer", __FILE__, __LINE__);
  }
  wgpuQueueSubmit(ctx.queue, 1, &op.commandBuffer);
  CallbackDataDyn callbackData = {op.readbackBuffer, bufferSize, data,
                                  &op.promise, &op.future};
  wgpuQueueOnSubmittedWorkDone(
      ctx.queue,
      [](WGPUQueueWorkDoneStatus status, void *callbackData) {
        check(status == WGPUQueueWorkDoneStatus_Success, "Queue work done",
              __FILE__, __LINE__);
        const auto *data = static_cast<CallbackDataDyn *>(callbackData);
        wgpuBufferMapAsync(
            data->buffer, WGPUMapMode_Read, 0, data->bufferSize,
            [](WGPUBufferMapAsyncStatus status, void *captureData) {
              const auto *data = static_cast<CallbackDataDyn *>(captureData);
              check(status == WGPUBufferMapAsyncStatus_Success,
                    "Map readbackBuffer", __FILE__, __LINE__);
              const void *mappedData = wgpuBufferGetConstMappedRange(
                  data->buffer, /*offset=*/0, data->bufferSize);
              check(mappedData, "Get mapped range", __FILE__, __LINE__);
              memcpy(data->output, mappedData, data->bufferSize);
              wgpuBufferUnmap(data->buffer);
              data->promise->set_value();
            },
            callbackData);
      },
      &callbackData);
  Wait(ctx, op.future);
}
// Convenience wrapper for array outputs
template <size_t N>
void ToCPU(Context &ctx, Tensor &tensor, std::array<float, N> data) {
  ToCPU(ctx, tensor, data.data(), sizeof(data));
}
void ToGPU(Context &ctx, const void *data, WGPUBuffer buffer, size_t size) {
  wgpuQueueWriteBuffer(ctx.queue, buffer, 0, data, size);
}
void ToGPU(Context &ctx, const float *data, Tensor &tensor) {
  wgpuQueueWriteBuffer(ctx.queue, tensor.data.buffer, 0, data,
                       tensor.data.size);
}
// Separate this out since WGPUCommandBuffer is destroyed upon submission
void ResetCommandBuffer(WGPUDevice &device, const Shape &nThreads, Kernel &op) {
  log(kDefLog, kTrace, "Create command buffer 0x%x", op.commandBuffer);
  {
    WGPUCommandEncoder commandEncoder =
        wgpuDeviceCreateCommandEncoder(device, nullptr);
    WGPUComputePassEncoder computePassEncoder =
        wgpuCommandEncoderBeginComputePass(commandEncoder, nullptr);
    log(kDefLog, kTrace, "Set pipeline %x", op.computePipeline);
    wgpuComputePassEncoderSetPipeline(computePassEncoder, op.computePipeline);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, op.bindGroup, 0,
                                       nullptr);
    log(kDefLog, kTrace, "Dispatching workgroups for number of threads = %s",
        ToString(nThreads).c_str());
    wgpuComputePassEncoderDispatchWorkgroups(
        computePassEncoder, op.nWorkgroups[0], op.nWorkgroups[1],
        op.nWorkgroups[2]);
    wgpuComputePassEncoderEnd(computePassEncoder);
    op.commandBuffer = wgpuCommandEncoderFinish(commandEncoder, nullptr);
    check(op.commandBuffer, "Create command buffer", __FILE__, __LINE__);
  }
}

/**
 * @brief NoParam is a no-op type used to indicate that a kernel does not have
 * any parameters.
 */
struct NoParam {};
template <typename T> constexpr bool IsNoParam = std::is_same_v<T, NoParam>;

/**
 * @brief A factory function to create a kernel on the GPU. The kernel is
 * created with the given shader code, input tensors, output tensor, and
 * optional parameters.
 *
 * Note that the values of the input tensors are not used here, only the
 * reference handles to the underlying buffers as well as the size of the
 * buffers.
 *
 * @param[in] ctx Context instance to manage the kernel
 * @param[in] shader Shader code for the kernel
 * @param[in] inputs A span of input tensors as a pointer
 * @param[in] numInputs Number of input tensors, effectively the size of the
 * *inputs span.
 * @param[in] nThreads Shape of the workgroup size for the kernel, must be of
 * rank 3.
 * @param[in] params Optional parameters for the kernel. If the kernel does not
 * have any parameters, use NoParam. This is cast as void* to allow for
 * arbitrary types to be passed as parameters.
 * @param[in] paramsSize Size of the parameters buffer in bytes.
 * @return Kernel instance representing the created kernel
 * @example Kernel kernel = CreateKernel(ctx, shader, inputs, numInputs, output,
 * nThreads, params, paramsSize);
 */
Kernel CreateKernel(Context &ctx, const ShaderCode &shader,
                    const Tensor *inputs, size_t numTensors,
                    const Shape &nThreads,
                    const void *params, size_t paramsSize = 0) {
  assert(nThreads.rank == 3);
  WGPUDevice device = ctx.device;
  WGPUQueue queue = ctx.queue;
  Kernel op;
  size_t paramIndex = -1;
  // paramIndex is undefined
  // unless ParamsType != NoParam
  size_t numBindings = numTensors;
  if (paramsSize > 0) {
    numBindings++;            // parameters buffer
    paramIndex = numBindings - 1; // index of the parameters buffer within
                                  // op.buffers, op.bufferSizes and
                                  // bgLayoutEntries
  }
  op.buffers = std::make_unique<WGPUBuffer[]>(numBindings);
  op.bufferSizes = std::make_unique<size_t[]>(numBindings);
  op.numBindings = numBindings;
  log(kDefLog, kInfo, "Create the bind group layout");
  std::vector<WGPUBindGroupLayoutEntry> bgLayoutEntries(numBindings);
  // Create layout entries for input buffers
  for (size_t i = 0; i < numTensors; ++i) {
    bgLayoutEntries[i] = WGPUBindGroupLayoutEntry{
        .binding = static_cast<uint32_t>(i),
        .visibility = WGPUShaderStage_Compute,
        .buffer =
            WGPUBufferBindingLayout{
                .type = WGPUBufferBindingType_Storage,
                .minBindingSize = inputs[i].data.size,
            },
    };
  }
  if (paramsSize > 0) {
    log(kDefLog, kInfo, "Create layout entry for the params buffer");
    // Create layout entry for the params buffer
    bgLayoutEntries[paramIndex] = WGPUBindGroupLayoutEntry{
        .binding = static_cast<uint32_t>(paramIndex),
        .visibility = WGPUShaderStage_Compute,
        .buffer =
            WGPUBufferBindingLayout{
                .type = WGPUBufferBindingType_Uniform,
                .minBindingSize = paramsSize,
            },
    };
  }
  log(kDefLog, kInfo, "Create the bind group layout descriptor");
  WGPUBindGroupLayoutDescriptor bgLayoutDesc = {
      .entryCount = static_cast<uint32_t>(bgLayoutEntries.size()),
      .entries = bgLayoutEntries.data(),
  };
  WGPUBindGroupLayout bgLayout =
      wgpuDeviceCreateBindGroupLayout(device, &bgLayoutDesc);
  for (size_t i = 0; i < numTensors; ++i) {
    op.buffers[i] = inputs[i].data.buffer;
    op.bufferSizes[i] = inputs[i].data.size;
  }
  log(kDefLog, kInfo, "Create the params buffer");
  // Create a buffer for the Params struct
  if (paramsSize > 0) {
    WGPUBufferDescriptor paramsBufferDesc = {
        .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
        .size = paramsSize,
        .mappedAtCreation = false,
    };
    op.buffers[paramIndex] = wgpuDeviceCreateBuffer(device, &paramsBufferDesc);
    op.bufferSizes[paramIndex] = paramsSize;
    wgpuQueueWriteBuffer(queue, op.buffers[paramIndex], 0, params, paramsSize);
    log(kDefLog, kInfo, "Params buffer written");
  } else {
    log(kDefLog, kInfo, "No params buffer needed");
  }
  log(kDefLog, kInfo, "Create the bind group");
  std::vector<WGPUBindGroupEntry> bindGroupEntries(numBindings);
  for (size_t i = 0; i < numTensors; ++i) {
    bindGroupEntries[i] = WGPUBindGroupEntry{
        .binding = static_cast<uint32_t>(i),
        .buffer = op.buffers[i],
        .offset = 0,
        .size = op.bufferSizes[i],
    };
  }
  if (paramsSize > 0) {
    log(kDefLog, kInfo, "Create bind group entry for the params buffer");
    log(kDefLog, kInfo, "paramIndex: %d", paramIndex);
    bindGroupEntries[paramIndex] = WGPUBindGroupEntry{
        .binding = static_cast<uint32_t>(paramIndex),
        .buffer = op.buffers[paramIndex],
        .offset = 0,
        .size = paramsSize,
    };
  }
  log(kDefLog, kInfo, "BG Entries Size: %d", numBindings);
  WGPUBindGroupDescriptor bindGroupDesc = {
      .layout = bgLayout,
      .entryCount = static_cast<uint32_t>(numBindings),
      .entries = bindGroupEntries.data(),
  };
  op.bindGroup = wgpuDeviceCreateBindGroup(device, &bindGroupDesc);
  {
    WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &bgLayout,
    };
    WGPUPipelineLayout pipelineLayout =
        wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDesc);
    WGPUShaderModuleWGSLDescriptor wgslDesc = {
        .code = shader.data.c_str(),
    };
    wgslDesc.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    WGPUShaderModuleDescriptor shaderModuleDesc = {};
    shaderModuleDesc.nextInChain = &wgslDesc.chain;
    shaderModuleDesc.label = "shader";
    WGPUComputePipelineDescriptor computePipelineDesc = {};
    computePipelineDesc.layout = pipelineLayout;
    computePipelineDesc.compute.module =
        wgpuDeviceCreateShaderModule(device, &shaderModuleDesc);
    computePipelineDesc.compute.entryPoint = "main";
    computePipelineDesc.label = "compute pipeline";
    op.computePipeline =
        wgpuDeviceCreateComputePipeline(device, &computePipelineDesc);
  }
  op.nWorkgroups = {
      (nThreads[0] + (shader.workgroupSize[0] - 1)) / shader.workgroupSize[0],
      (nThreads[1] + (shader.workgroupSize[1] - 1)) / shader.workgroupSize[1],
      (nThreads[2] + (shader.workgroupSize[2] - 1)) / shader.workgroupSize[2]};
  ResetCommandBuffer(device, nThreads, op);
  ctx.kernelPool.data.insert(&op);
  log(kDefLog, kInfo, "Exiting CreateKernel");
  return op;
}

/**
 * @brief Overload which wraps the CreateKernel factory function to create a
 * kernel on the GPU. This overload uses takes a static collection of input
 * tensors instead of a pointer and a statically determined ParamsType instead
 * of casting params to a void pointer.
 *
 * @param[in] ctx Context instance to manage the kernel
 * @param[in] shader Shader code for the kernel
 * @param[in] inputs A collection of input tensors
 * @param[in] output Output tensor for the kernel
 * @param[in] nThreads Shape of the workgroup size for the kernel, must be of
 * rank 3.
 * @param[in] params Optional parameters for the kernel. If the kernel does not
 * have any parameters, use NoParam.
 * @return Kernel instance representing the created kernel
 * @example Kernel kernel = CreateKernel(ctx, shader, inputs, output, nThreads,
 * params);
 */
template <typename ParamsType = NoParam, size_t numInputs>
Kernel CreateKernel(Context &ctx, const ShaderCode &shader,
                    const TensorList<numInputs> &inputs, 
                    const Shape &nThreads,
                    const ParamsType &params = ParamsType{}) {
  if constexpr (!IsNoParam<ParamsType>) {
    log(kDefLog, kInfo, "Using params of size %d bytes", sizeof(ParamsType));
    return CreateKernel(ctx, shader, inputs.data.data(), numInputs, nThreads,
                        reinterpret_cast<const void *>(&params),
                        sizeof(ParamsType));
  } else {
    log(kDefLog, kInfo, "No params");
    return CreateKernel(ctx, shader, inputs.data.data(), numInputs, nThreads,
                        nullptr, 0);
  }
}

/**
 * @brief Asynchronously submits a kernel to the GPU queue for execution.
 * It also sets up a callback to notify when the kernel has finished executing
 * by setting the value of the promise in the kernel instance argument.
 *
 * DispatchKernel does *not* wait for the kernel to finish executing and returns
 * immediately. The caller can wait for the kernel to finish executing by
 * calling Wait() on the future in the kernel instance.
 *
 * @param[in] ctx Context instance to manage the kernel, from which the queue
 * for the GPU is obtained
 * @param[in] kernel Kernel instance to dispatch
 * @example DispatchKernel(ctx, kernel);
 */
void DispatchKernel(Context &ctx, Kernel &kernel, std::promise<void>& promise) {
  // Submit the command buffer
  wgpuQueueSubmit(ctx.queue, 1, &kernel.commandBuffer);
  wgpuQueueOnSubmittedWorkDone(
      ctx.queue,
      [](WGPUQueueWorkDoneStatus status, void *data) {
        check(status == WGPUQueueWorkDoneStatus_Success, "Queue work done",
              __FILE__, __LINE__);
        auto *promise = static_cast<std::promise<void>*>(data);
        promise->set_value();
      },
      &promise);
}

} // namespace gpu

#endif // GPU_H
