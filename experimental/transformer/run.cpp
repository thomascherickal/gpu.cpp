#include "gpu.h"
#include "utils/array_utils.h"
#include "utils/logging.h"
#include <array>

using namespace gpu;

static const char *kShaderGelu = R"(
const GELU_SCALING_FACTOR: f32 = 0.7978845608028654; // sqrt(2.0 / PI)
@group(0) @binding(0) var<storage, read_write> inp: array<{{precision}}>;
@group(0) @binding(1) var<storage, read_write> out: array<{{precision}}>;
@compute @workgroup_size({{workgroupSize}})
fn main(
    @builtin(global_invocation_id) GlobalInvocationID: vec3<u32>) {
    let i: u32 = GlobalInvocationID.x;
    if (i < arrayLength(&inp)) {
        let x: f32 = inp[i];
        // select is more stable for larger values of x
        out[i] = select(0.5 * x * (1.0 + tanh(GELU_SCALING_FACTOR 
                  * (x + .044715 * x * x * x))), x, x > 10.0);
    }
}
)";

static const char *kMLPGate = R"(
const GELU_SCALING_FACTOR: f32 = 0.7978845608028654; // sqrt(2.0 / PI)
@group(0) @binding(0) var<storage, read_write> gate: array<{{precision}}>;
@group(0) @binding(0) var<storage, read_write> gated: array<{{precision}}>;
@group(0) @binding(1) var<storage, read_write> out: array<{{precision}}>;
@compute @workgroup_size({{workgroupSize}})
fn main(
    @builtin(global_invocation_id) GlobalInvocationID: vec3<u32>) {
    let i: u32 = GlobalInvocationID.x;
    if (i < arrayLength(&a)) {
        let x: f32 = gate[i];
        out[i] = gated[i] * select(0.5 * x * (1.0 + tanh(GELU_SCALING_FACTOR 
                    * (x + .044715 * x * x * x))), x, x > 10.0);
    }
}
)";

static const char *kShaderMatMul = R"(
@group(0) @binding(0) var<storage, read_write> A: array<f32>;
@group(0) @binding(1) var<storage, read_write> B: array<f32>;
@group(0) @binding(2) var<storage, read_write> C: array<f32>;
var<workgroup> tileA: array<f32, workgroupSizeY * workgroupSizeX>;
var<workgroup> tileB: array<f32, workgroupSizeY * workgroupSizeX>;
@compute @workgroup_size(workgroupSizeX, workgroupSizeY, 1)
fn matmul(
    @builtin(global_invocation_id) global_id : vec3<u32>,
    @builtin(local_invocation_id) local_id : vec3<u32>,
    @builtin(workgroup_id) workgroup_id : vec3<u32>
) {
    let row = global_id.x;
    let col = global_id.y;
    if (row >= {{M}} || col >= {{N}}) {
        return;
    }
    var result: f32 = 0.0;
    for (var i = 0u; i < {{K}}; i = i + workgroupSizeX) {
        // Load tiles into shared memory
        tileA[local_id.y][local_id.x] = A[row][i + local_id.x];
        tileB[local_id.y][local_id.x] = B[i + local_id.y][col];
        // Synchronize to make sure the tile is loaded
        workgroupBarrier();
        // Perform partial dot product for the current tile
        for (var k = 0u; k < workgroupSizeX; k = k + 1u) {
            result = result + tileA[local_id.y][k] * tileB[k][local_id.x];
        }
        // Synchronize before loading the next tile
        workgroupBarrier();
    }
    C[row][col] = result;
}
)";

struct Transformer {
  Tensor qkv;         // modelDim * 3 * qkvDim
  Tensor rmsNormPre;  // modelDim
  Tensor rmsNormPost; // modelDim
  Tensor out;         // 3 * qkvDim * modelDim

  Tensor mlp1; // modelDim * (2 * hidden_width * modelDim)
  Tensor mlp2; // modelDim * (2 * hidden_width * modelDim)
  ShaderCode matmul;
  ShaderCode attention;
  ShaderCode rmsNorm;
};

struct Activations {
  Tensor normPre; // modelDim * seqLen
  Tensor attOut;
};

struct KVCache {
  Tensor key_cache;
  Tensor value_cache;
};

void initTransformer(Context &ctx, size_t modelDim, size_t qkvDim,
                     size_t batchSize, size_t seqLen, size_t hiddenWidth,
                     Transformer &transformer, Activations &activations,
                     KVCache &kvcache) {
  transformer = {
      .qkv = CreateTensor(ctx, Shape{modelDim, 3 * qkvDim}, kf32),
      .rmsNormPre = CreateTensor(ctx, Shape{modelDim}, kf32),
      .rmsNormPost = CreateTensor(ctx, Shape{modelDim}, kf32),
      .out = CreateTensor(ctx, Shape{3 * qkvDim, modelDim}, kf32),
      .mlp1 = CreateTensor(ctx, Shape{modelDim, 2 * hiddenWidth}, kf32),
      .mlp2 = CreateTensor(ctx, Shape{modelDim, 2 * hiddenWidth}, kf32),
  };
  activations = {
      // TODO
  };
  kvcache = {
      .key_cache = CreateTensor(ctx, Shape{seqLen, qkvDim}, kf32),
      .value_cache = CreateTensor(ctx, Shape{seqLen, qkvDim}, kf32),
  };
}

int main() {
  printf("\033[2J\033[1;1H");
  Context ctx = CreateContext();
  // static constexpr N = 3072;
  static constexpr size_t N = 128;
  static constexpr size_t seqLen = 24;
  static constexpr size_t batchSize = 1;
  static constexpr size_t modelDim = 3072;
  static constexpr size_t hiddenWidth = modelDim * 2;
  static constexpr size_t qkvDim = 256;
  std::array<float, modelDim> inputArr, outputArr;
  std::mt19937 gen(314);
  randn(inputArr, gen);

  Tensor input = CreateTensor(ctx, Shape{N}, kf32, inputArr.data());
  Transformer transformer;
  Activations activations;
  KVCache kvcache;
  printf("Initializing transformer, allocating GPU buffers ...\n");
  initTransformer(ctx, modelDim, qkvDim, batchSize, seqLen, hiddenWidth,
                  transformer, activations, kvcache);

  input = CreateTensor(ctx, Shape{modelDim}, kf32, inputArr.data());

  printf("Done\n");
}