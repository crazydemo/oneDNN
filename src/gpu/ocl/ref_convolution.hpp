/*******************************************************************************
* Copyright 2019-2023 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_OCL_REF_CONVOLUTION_HPP
#define GPU_OCL_REF_CONVOLUTION_HPP

#include "common/c_types_map.hpp"
#include "common/primitive.hpp"
#include "gpu/gpu_primitive.hpp"

#include "gpu/compute/compute.hpp"
#include "gpu/gpu_convolution_pd.hpp"
#include "gpu/gpu_resource.hpp"
#include "gpu/ocl/ocl_stream.hpp"
#include "gpu/primitive_conf.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

struct ref_convolution_fwd_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_convolution_fwd_pd_t {
        using gpu_convolution_fwd_pd_t::gpu_convolution_fwd_pd_t;

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_convolution_fwd_t);

        status_t init(engine_t *engine) {
            using namespace data_type;

            const auto *compute_engine
                    = utils::downcast<compute::compute_engine_t *>(engine);

            const auto attr_skip_mask
                    = primitive_attr_t::skip_mask_t::scales_runtime
                    | primitive_attr_t::skip_mask_t::zero_points_runtime
                    | primitive_attr_t::skip_mask_t::post_ops
                    | primitive_attr_t::skip_mask_t::sum_dt;

            const bool is_int8 = utils::one_of(src_md_.data_type, s8, u8);
            bool ok = set_default_alg_kind(alg_kind::convolution_direct)
                    && utils::one_of(desc()->prop_kind,
                            prop_kind::forward_training,
                            prop_kind::forward_inference)
                    && desc()->alg_kind == alg_kind::convolution_direct
                    && IMPLICATION(
                            utils::one_of(f16, src_md_.data_type,
                                    weights_md_.data_type, dst_md_.data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp16))
                    && IMPLICATION(
                            utils::one_of(f64, src_md_.data_type,
                                    weights_md_.data_type, dst_md_.data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp64)
                                    && attr()->post_ops_.has_default_values())
                    && !memory_desc_ndims_ok(src_md(), weights_md(), dst_md())
                    && this->set_default_formats()
                    && attr()->has_default_values(
                            attr_skip_mask, dst_md_.data_type)
                    && attr()->post_ops_.check_sum_consistency(
                            dst_md_.data_type, is_int8, true)
                    && attr_.set_default_formats(dst_md(0)) == status::success
                    && post_ops_with_binary_ok(
                            attr(), dst_md()->data_type, 5, 0xffff)
                    && attr_scales_ok() && zero_points_ok(attr())
                    && IMPLICATION(
                            !attr()->scales_.has_default_values(), is_int8);
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        conv_conf_t conf;

    private:
        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, nwc, nhwc, ndhwc);
            auto wei_tag = with_groups()
                    ? utils::pick(ndims() - 3, goiw, goihw, goidhw)
                    : utils::pick(ndims() - 3, oiw, oihw, oidhw);
            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    status_t init(engine_t *engine) override {
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        CHECK(create_kernel(
                engine, &kernel_, "ref_convolution_fwd", kernel_ctx));
        if (!kernel_) return status::runtime_error;

        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        return execute_forward(ctx);
    }

private:
    status_t execute_forward(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::kernel_t kernel_;
};

struct ref_convolution_bwd_data_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_convolution_bwd_data_pd_t {
        using gpu_convolution_bwd_data_pd_t::gpu_convolution_bwd_data_pd_t;

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_convolution_bwd_data_t);

        status_t init(engine_t *engine) {
            using sm = primitive_attr_t::skip_mask_t;
            const auto attr_skip_mask = sm::post_ops | sm::scales_runtime
                    | sm::zero_points_runtime;
            using namespace data_type;
            const auto *compute_engine
                    = utils::downcast<compute::compute_engine_t *>(engine);
            bool ok = set_default_alg_kind(alg_kind::convolution_direct)
                    && desc()->prop_kind == prop_kind::backward_data
                    && desc()->alg_kind == alg_kind::convolution_direct
                    && !memory_desc_ndims_ok(diff_src_md(), diff_dst_md())
                    && this->set_default_formats()
                    && attr()->has_default_values(attr_skip_mask)
                    && post_ops_with_binary_ok(
                            attr(), dst_md()->data_type, ndims())
                    && attr_scales_ok() && zero_points_ok(attr())
                    && IMPLICATION(utils::one_of(f64, diff_src_md()->data_type,
                                           dst_md()->data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp64)
                                    && attr()->post_ops_.has_default_values())
                    && attr_.set_default_formats(diff_src_md(0))
                            == status::success;
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        conv_conf_t conf;

    private:
        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, ncw, nchw, ncdhw);
            auto wei_tag = with_groups()
                    ? utils::pick(ndims() - 3, goiw, goihw, goidhw)
                    : utils::pick(ndims() - 3, oiw, oihw, oidhw);
            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    status_t init(engine_t *engine) override {
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        CHECK(create_kernel(
                engine, &kernel_, "ref_convolution_bwd_data", kernel_ctx));
        if (!kernel_) return status::runtime_error;

        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        return execute_backward_data(ctx);
    }

private:
    status_t execute_backward_data(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::kernel_t kernel_;
};

struct ref_convolution_bwd_weights_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_convolution_bwd_weights_pd_t {
        using gpu_convolution_bwd_weights_pd_t::
                gpu_convolution_bwd_weights_pd_t;

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_convolution_bwd_weights_t);

        status_t init(engine_t *engine) {
            using namespace data_type;
            const auto *compute_engine
                    = utils::downcast<compute::compute_engine_t *>(engine);

            bool ok = set_default_alg_kind(alg_kind::convolution_direct)
                    && desc()->prop_kind == prop_kind::backward_weights
                    && desc()->alg_kind == alg_kind::convolution_direct
                    && !memory_desc_ndims_ok(src_md(), diff_dst_md())
                    && utils::one_of(desc()->diff_weights_desc.data_type, f32,
                            bf16, f16, f64, f8_e5m2, f8_e4m3)
                    && utils::one_of(desc()->src_desc.data_type, f32, bf16, f16,
                            f64, f8_e5m2, f8_e4m3)
                    && utils::one_of(desc()->diff_dst_desc.data_type, f32, bf16,
                            f16, f64, f8_e5m2, f8_e4m3)
                    && this->set_default_formats()
                    && attr()->has_default_values()
                    && IMPLICATION(
                            utils::one_of(f16, desc()->src_desc.data_type,
                                    desc()->diff_weights_desc.data_type,
                                    desc()->diff_dst_desc.data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp16))
                    && IMPLICATION(
                            utils::one_of(f64, desc()->src_desc.data_type,
                                    desc()->diff_dst_desc.data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp64)
                                    && attr()->post_ops_.has_default_values());
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        conv_conf_t conf;

    private:
        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, ncw, nchw, ncdhw);
            auto wei_tag = with_groups()
                    ? utils::pick(ndims() - 3, goiw, goihw, goidhw)
                    : utils::pick(ndims() - 3, oiw, oihw, oidhw);
            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    status_t init(engine_t *engine) override {
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        CHECK(create_kernel(
                engine, &kernel_, "ref_convolution_bwd_weights", kernel_ctx));
        if (!kernel_) return status::runtime_error;

        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        return execute_backward_weights(ctx);
    }

private:
    status_t execute_backward_weights(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::kernel_t kernel_;
};

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl
#endif
