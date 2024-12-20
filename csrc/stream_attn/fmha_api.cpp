/******************************************************************************
 * Copyright (c) 2011-2021, NVIDIA CORPORATION.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

#include <torch/extension.h>
#include <ATen/cuda/CUDAContext.h>

#include "fmha.h"

void set_params(Fused_multihead_attention_fprop_params &params,
                // sizes
                const size_t b,
                const size_t s,
                const size_t h,
                const size_t d,
                // device pointers
                void *qkv_packed_d,
                void *cu_seqlens_d,
                void *o_packed_d,
                void *o_tmp_d,
                void *do_packed_d,
                void *s_d,
                void *softmax_lse_d,
                void *dsoftmax_sum_d,
                float p_dropout,
                float softmax_scale,
                bool is_causal,
                void *o2_tmp_d,
                void *o2_packed_d) {

    Data_type acc_type = DATA_TYPE_FP32;
    Data_type data_type = DATA_TYPE_FP16;

    // Reset the parameters
    memset(&params, 0, sizeof(params));

    // Set the pointers and strides.
    params.qkv_ptr = qkv_packed_d;
    params.qkv_stride_in_elts = h * 4 * d;
    params.qkv_stride_in_bytes = get_size_in_bytes(h * 4 * d, data_type);
    params.o_ptr = o_packed_d;
    params.o2_ptr = o2_packed_d;
    params.o_stride_in_elts = h * d;
    params.o_stride_in_bytes = get_size_in_bytes(h * d, data_type);
    params.do_ptr = do_packed_d;
    params.o_tmp_ptr = o_tmp_d;
    params.o2_tmp_ptr = o2_tmp_d;

    params.cu_seqlens = static_cast<int *>(cu_seqlens_d);

    // S = softmax(P)
    params.s_ptr = s_d;
    params.s_stride_in_bytes = get_size_in_bytes(b * h * s, data_type);

    // Softmax sum
    params.softmax_lse_ptr = softmax_lse_d;
    params.dsoftmax_sum = dsoftmax_sum_d;

    // Set the dimensions.
    params.b = b;
    params.h = h;
    params.s = s;
    params.d = d;

    // Set the different scale values.
    // const float scale_bmm1 = 1.f / sqrtf(d);
    const float scale_bmm1 = softmax_scale;
    constexpr float scale_softmax = 1.f;
    constexpr float scale_bmm2 = 1.f;

    params.scale_bmm1f = scale_bmm1;
    set_alpha(params.scale_bmm1, scale_bmm1, data_type);
    set_alpha(params.scale_softmax, scale_softmax, acc_type);
    set_alpha(params.scale_bmm2, scale_bmm2, data_type);

    // Set this to probability of keeping an element to simplify things.
    params.p_dropout = 1.f - p_dropout;
    // Convert p from float to int so we don't have to convert the random uint to float to compare.
    // [Minor] We want to round down since when we do the comparison we use <= instead <
    params.p_dropout_in_uint = uint32_t(std::floor(params.p_dropout * 4294967295.0));
    params.p_dropout_in_uint16_t = uint16_t(std::floor(params.p_dropout * 65535.0));
    params.rp_dropout = 1.f / params.p_dropout;
    TORCH_CHECK(p_dropout < 1.f);
    set_alpha(params.scale_dropout, params.rp_dropout, data_type);

    params.is_causal = is_causal;
}

std::vector<at::Tensor> 
mha_fwd(const at::Tensor &qkvv,         // total x num_heads x 4 x head_size, total := \sum_{i=0}^{b} s_i
        const at::Tensor &cu_seqlens,  // b+1
        const float p_dropout,
        const int max_seq_len,
        const float softmax_scale,
        const bool zero_tensors,
        const bool is_causal,
        const bool return_softmax,
        c10::optional<at::Generator> gen_) {

    auto dprops = at::cuda::getCurrentDeviceProperties();
    TORCH_CHECK(dprops->major == 8 && dprops->minor >= 0);
    auto stream = at::cuda::getCurrentCUDAStream().stream();
    bool is_dropout = p_dropout > 0.0;
    Launch_params<Fused_multihead_attention_fprop_params> launch_params(dprops, stream, is_dropout, return_softmax);

    TORCH_CHECK(qkvv.is_cuda())
    TORCH_CHECK(cu_seqlens.is_cuda())

    TORCH_CHECK(qkvv.is_contiguous())
    TORCH_CHECK(cu_seqlens.is_contiguous())

    TORCH_CHECK(cu_seqlens.dim() == 1);
    TORCH_CHECK(qkvv.dim() == 4);

    const auto sizes = qkvv.sizes();

    TORCH_CHECK(sizes[THREE_DIM] == 4);

    const int batch_size = cu_seqlens.numel() - 1;
    const int total = sizes[TOTAL_DIM];
    const int num_heads = sizes[H_DIM];
    const int head_size = sizes[D_DIM];
    TORCH_CHECK(batch_size > 0);
    TORCH_CHECK(head_size == 16 || head_size == 32 || head_size == 64 || head_size == 128);

    // int base_N = head_size == 16 ? 512 : (head_size == 128 ? 128 : 256);
    int base_N = head_size == 128 ? 128 : 256;
    // int base_N = 256;
    int seq_len = 512;
    if( max_seq_len <= 128 ) {
        seq_len = 128;
    } else if( max_seq_len <= 256 ) {
        seq_len = 256;
    } else {
        seq_len = ((max_seq_len + base_N - 1) / base_N) * base_N;
    }
    bool loop = seq_len > base_N;

    auto opts = qkvv.options();

    auto ctx = torch::empty({ total, num_heads, head_size }, opts);
    auto ctx2 = torch::empty({ total, num_heads, head_size }, opts);

    at::Tensor o_tmp;
    at::Tensor o2_tmp;
    if (loop) {
        o_tmp = torch::empty({total, num_heads, head_size}, opts.dtype(at::kFloat));
        o2_tmp = torch::empty({total, num_heads, head_size}, opts.dtype(at::kFloat));
    }

    auto softmax_lse = torch::empty({batch_size, num_heads, seq_len}, opts.dtype(at::kFloat));
    // auto softmax_lse = torch::full({batch_size, num_heads, seq_len}, -std::numeric_limits<float>::infinity(), opts.dtype(at::kFloat));

    at::Tensor s;
    if (return_softmax) {
        s = torch::empty({ batch_size, num_heads, seq_len, seq_len }, opts);
        // s = torch::ones({ batch_size, num_heads, seq_len, seq_len }, opts) * 10000.0;
    }

    if( zero_tensors ) {
        ctx.zero_();
        ctx2.zero_();
        softmax_lse.fill_(-std::numeric_limits<float>::infinity());
        if (loop) { o_tmp.zero_(); }
        if (loop) { o2_tmp.zero_(); }
        if (return_softmax) {s.zero_();}
    }

    auto gen = at::get_generator_or_default<at::CUDAGeneratorImpl>(
        gen_, at::cuda::detail::getDefaultCUDAGenerator());


    set_params(launch_params.params,
               batch_size,
               seq_len,
               num_heads,
               head_size,
               qkvv.data_ptr(),
               cu_seqlens.data_ptr(),
               ctx.data_ptr(),
               loop ? o_tmp.data_ptr() : nullptr,
               nullptr,
               return_softmax ? s.data_ptr() : nullptr,
               softmax_lse.data_ptr(),
               nullptr,
               p_dropout,
               softmax_scale,
               is_causal,
               loop ? o2_tmp.data_ptr() : nullptr,
               ctx2.data_ptr());

    run_fmha_fp16_sm80(launch_params, /*configure=*/ true);
    // number of times random will be generated per thread, to offset philox counter in thc random
    // state
    int64_t counter_offset = launch_params.elts_per_thread;
    at::PhiloxCudaState rng_engine_inputs;

    if( is_dropout ) {
        // See Note [Acquire lock when using random generators]
        std::lock_guard<std::mutex> lock(gen->mutex_);
        launch_params.params.philox_args = gen->philox_cuda_state(counter_offset);
    }

    run_fmha_fp16_sm80(launch_params, /*configure=*/false);

    std::vector<at::Tensor> result = {ctx, ctx2, softmax_lse};
    if (return_softmax) {result.push_back(s);}
    return result;
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
    m.doc() = "Fused Multi-head Self-attention";
    m.def("fwd", &mha_fwd, "Forward pass");
}
