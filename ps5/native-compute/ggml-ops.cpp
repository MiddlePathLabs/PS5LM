/* SPDX-License-Identifier: GPL-3.0-or-later
 * One causal prefill graph; compare every output, bound failure logging.
 */
#include "ggml.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include "ggml-vulkan.h"
#include "llama.h"
#include "ggml-ops.h"
#include <cmath>
#include <cstdio>
#include <exception>
#include <cstdint>
#include <cstring>

extern "C" [[noreturn]] void ps5lm_ops_fatal(const char *stage);
extern "C" void ps5lm_ops_checked(unsigned count, unsigned errors);

static constexpr unsigned query_count = 4, value_count = 512, failure_log_limit = 16;
static constexpr float absolute_tolerance = 1e-4f, relative_tolerance = 1e-3f;
static_assert(value_count == PS5LM_GGML_VALUE_COUNT, "Result-count contract");

static float sample(int64_t i, unsigned seed)
{
    return (float(int((i * 17 + i / 97 * 13 + seed) % 97)) - 48.0f) / 49.0f;
}

static void set_input(ggml_tensor *tensor, unsigned seed, bool round_to_half = false)
{
    float data[2048];
    ggml_fp16_t encoded[2048];
    const int64_t count = ggml_nelements(tensor);
    if (count > 2048) ps5lm_ops_fatal("ggml_input_bound");
    for (int64_t i = 0; i < count; ++i) {
        data[i] = sample(i, seed);
        if (round_to_half) data[i] = ggml_fp16_to_fp32(ggml_fp32_to_fp16(data[i]));
    }
    if (tensor->type == GGML_TYPE_F32) {
        ggml_backend_tensor_set(tensor, data, 0, ggml_nbytes(tensor));
    } else if (tensor->type == GGML_TYPE_F16) {
        ggml_fp32_to_fp16_row(data, encoded, count);
        ggml_backend_tensor_set(tensor, encoded, 0, ggml_nbytes(tensor));
    } else {
        ps5lm_ops_fatal("ggml_input_type");
    }
}

static bool evaluate(ggml_backend_t backend, float *out, bool cpu_reference)
{
    ggml_context *ctx = ggml_init({65536, nullptr, true});
    if (!ctx) return false;
    // Four causal queries, two Q heads, one KV head, D=64, KV=32.
    ggml_tensor *q = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, 64, query_count, 2);
    // Reference avoids the CPU's FP16 V accumulator but preserves cache values.
    const ggml_type cache_type = cpu_reference ? GGML_TYPE_F32 : GGML_TYPE_F16;
    ggml_tensor *k = ggml_new_tensor_3d(ctx, cache_type, 64, 32, 1);
    ggml_tensor *v = ggml_new_tensor_3d(ctx, cache_type, 64, 32, 1);
    ggml_tensor *mask = ggml_new_tensor_2d(ctx, GGML_TYPE_F16, 32, query_count);
    ggml_tensor *result = ggml_flash_attn_ext(ctx, q, k, v, mask, 0.125f, 0.0f, 0.0f);
    ggml_prec_set_acc(result, GGML_PREC_F32);
    if (ggml_nelements(result) != value_count) ps5lm_ops_fatal("ggml_output_bound");
    ggml_cgraph *graph = ggml_new_graph_custom(ctx, 16, false);
    ggml_build_forward_expand(graph, result);
    for (int i = 0; i < ggml_graph_n_nodes(graph); ++i) {
        if (!ggml_backend_supports_op(backend, ggml_graph_node(graph, i))) {
            std::printf("PS5LM_GGML_UNSUPPORTED {\"case\":\"ATTN_PREFILL\",\"backend\":\"%s\"}\n",
                        ggml_backend_name(backend));
            ggml_free(ctx);
            return false;
        }
    }
    ggml_backend_buffer_t buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
    if (!buffer) { ggml_free(ctx); return false; }
    set_input(q, 3);
    set_input(k, 23, cpu_reference);
    set_input(v, 37, cpu_reference);
    ggml_fp16_t causal[32 * query_count];
    for (unsigned query = 0; query < query_count; ++query)
        for (unsigned key = 0; key < 32; ++key)
            causal[query * 32 + key] = ggml_fp32_to_fp16(key <= 32 - query_count + query ? 0.0f : -INFINITY);
    ggml_backend_tensor_set(mask, causal, 0, sizeof(causal));
    float sentinel[value_count];
    for (float &value : sentinel) value = NAN;
    ggml_backend_tensor_set(result, sentinel, 0, sizeof(sentinel));
    if (ggml_backend_graph_compute(backend, graph) != GGML_STATUS_SUCCESS)
        ps5lm_ops_fatal("ggml_graph_failed");
    ggml_backend_synchronize(backend);
    ggml_backend_tensor_get(result, out, 0, value_count * sizeof(float));
    ggml_backend_buffer_free(buffer);
    ggml_free(ctx);
    return true;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "IEEE binary32 storage");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct comparison {
    unsigned errors = 0;
    float maximum = 0;
    int first = -1;
};

static comparison compare(const float *expected, const float *actual)
{
    comparison result;
    for (unsigned i = 0; i < value_count; ++i) {
        const float delta = std::fabs(actual[i] - expected[i]);
        const bool match = std::isfinite(expected[i]) && std::isfinite(actual[i]) &&
                           delta <= absolute_tolerance + relative_tolerance * std::fabs(expected[i]);
        if (!match) {
            if (!result.errors) result.first = int(i);
            ++result.errors;
        }
        if (std::isfinite(delta) && delta > result.maximum) result.maximum = delta;
        // Every value is compared; only the first bounded failures are logged.
        if (!match && result.errors <= failure_log_limit)
            std::printf("PS5LM_ATTN_VALUE {\"index\":%u,\"reference_bits\":\"%08x\","
                        "\"actual_bits\":\"%08x\",\"match\":false}\n",
                        i, float_bits(expected[i]), float_bits(actual[i]));
    }
    return result;
}

extern "C" int ps5lm_run_ggml_ops(void)
{
    try {
        llama_backend_init();
        ggml_backend_t cpu = ggml_backend_cpu_init();
        if (!cpu) ps5lm_ops_fatal("ggml_cpu_init");
        ggml_backend_cpu_set_n_threads(cpu, 1);
        ggml_backend_t gpu = ggml_backend_vk_init(0);
        if (!gpu) ps5lm_ops_fatal("ggml_vulkan_init");
        std::printf("PS5LM_GGML_CASE_BEGIN {\"case\":\"ATTN_PREFILL\","
                    "\"q_type\":\"f32\",\"cache_type\":\"f16\",\"q_heads\":2,"
                    "\"kv_heads\":1,\"head_size\":64,\"kv_tokens\":32,\"query_tokens\":%u,"
                    "\"scale\":0.125,\"requested_acc\":\"f32\",\"atol\":%.9g,\"rtol\":%.9g}\n",
                    query_count, absolute_tolerance, relative_tolerance);
        float expected[value_count], actual[value_count];
        if (!evaluate(cpu, expected, true) || !evaluate(gpu, actual, false))
            ps5lm_ops_fatal("ggml_operator_unsupported");
        const comparison result = compare(expected, actual);
        std::printf("PS5LM_GGML_OP {\"case\":\"ATTN_PREFILL\",\"type\":\"f16\",\"values\":%u,"
                    "\"mismatches\":%u,\"max_finite_abs_error\":%.9g,\"atol\":%.9g,"
                    "\"rtol\":%.9g,\"first_mismatch\":%d,\"logged_failures\":%u,"
                    "\"failure_log_limit\":%u}\n",
                    value_count, result.errors, result.maximum, absolute_tolerance, relative_tolerance,
                    result.first, result.errors < failure_log_limit ? result.errors : failure_log_limit,
                    failure_log_limit);
        ps5lm_ops_checked(value_count, result.errors);
        ggml_backend_free(gpu);
        ggml_backend_free(cpu);
        llama_backend_free();
        return result.errors ? 6 : 0;
    } catch (const std::exception &) {
        ps5lm_ops_fatal("ggml_exception");
    } catch (...) {
        ps5lm_ops_fatal("ggml_unknown_exception");
    }
}
