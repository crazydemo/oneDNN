/*******************************************************************************
* Copyright 2023 Intel Corporation
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

#include "gpu/jit/pooling/ir_builder.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>
#include <unordered_map>

#include "common/c_types_map.hpp"
#include "gpu/jit/ir/epilogue.hpp"
#include "gpu/jit/ir/gemm_schedule.hpp"
#include "gpu/jit/ir/ir.hpp"
#include "gpu/jit/ir/message.hpp"
#include "gpu/jit/ir/post_ops.hpp"
#include "gpu/jit/ir/tensor.hpp"
#include "gpu/jit/pass/pass.hpp"
#include "gpu/jit/utils/iterator.hpp"
#include "gpu/jit/utils/range.hpp"
#include "gpu/jit/utils/trace.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

class pooling_post_op_view_mapper_t : public post_op_view_mapper_t {
public:
    pooling_post_op_view_mapper_t(const view_t &cp_view, const int ndims)
        : post_op_view_mapper_t(cp_view), ndims_(ndims) {}

    view_t create_view(const type_t &type, uint32_t mask) const override {
        return post_op_view_mapper_t::create_view(type, normalize_mask(mask));
    }

    view_t create_view(const memory_desc_t &md) const override {
        int cp_ndims = cp_view().nvdims();
        ir_assert(cp_ndims >= 3);
        layout_t layout(md, /*do_normalize=*/false);
        std::vector<dim_t> dims(md.dims, md.dims + md.ndims);
        std::vector<dim_t> pad_dims(md.padded_dims, md.padded_dims + md.ndims);
        maybe_reshape_dims(ndims_, layout, dims, pad_dims);
        layout = spatials_to_3d(layout, false, 0);
        dims = dims_to_3d(dims);
        pad_dims = dims_to_3d(pad_dims);
        ir_assert(layout.ndims() == cp_ndims) << "Incompatible dimensions.";
        uint32_t bound_check_mask = 0;
        for (int i = 0; i < cp_ndims; i++) {
            if (dims[i] == 1) continue; // Broadcast, no bound check needed.
            if (pad_dims[i] != cp_view().tlayout().dim(i)) {
                bound_check_mask |= (1 << i);
            } else if (cp_view().has_tmask(i)) {
                bound_check_mask |= (1 << i);
            }
        }
        return view_t(layout, cp_view().vvars(), dims, bound_check_mask);
    }

    bool need_to_restore_zero_padding() const override { return true; }

private:
    static void maybe_reshape_dims(int ndims, layout_t &layout,
            std::vector<dim_t> &dims, std::vector<dim_t> &padded_dims) {
        ir_assert(layout.ndims() == int(dims.size()));
        if (layout.ndims() < ndims) {
            layout = layout_t(layout.type(), ndims, layout.offset(),
                    layout.blocks(), /*do_normalize=*/false);
            dims.resize(ndims, 1);
            padded_dims.resize(ndims, 1);
        }
    }

    static std::vector<dim_t> dims_to_3d(const std::vector<dim_t> &dims) {
        layout_t dummy_layout(type_t::u8(), 0, dims);
        return spatials_to_3d(dummy_layout, false, 0).dims();
    }

    uint32_t normalize_mask(uint32_t orig_mask) const {
        int cp_ndims = cp_view().nvdims();
        ir_assert(cp_ndims >= 3);
        // Number of dimensions before normalization.
        int orig_ndims = 2 + ndims_;
        std::vector<dim_t> dummy_dims(orig_ndims, 1);
        dim_t mask_set_value = 2;
        for (int i = 0; i < orig_ndims; i++) {
            if ((orig_mask & (1 << i)) != 0) dummy_dims[i] = mask_set_value;
        }
        auto cvt_dims = dims_to_3d(dummy_dims);
        ir_assert(int(cvt_dims.size()) == cp_ndims);

        uint32_t mask = 0;
        for (int i = 0; i < cp_ndims; i++) {
            if (cvt_dims[i] == mask_set_value) mask = mask | (1 << i);
        }
        return mask;
    }

    const int ndims_;
};

void reduce_dim(int &dn, int &up, int scale) {
    for (auto p : std::array<int, 11> {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31})
        if (dn % (p * scale) == 0) {
            up *= p;
            dn /= p;
            return;
        }
    up *= dn / scale;
    dn = scale;
}

void pooling_ir_builder_t::build() {
    while ((stmt_ = try_build(*this, ki_mutable_, cfg_mutable_, pd_))
                    .is_empty()) {
        ir_warning() << "loop_grid too large, reduce and retry" << std::endl;
        auto kg(cfg_mutable_.kernel_grid());
        auto lg(cfg_mutable_.loop_grid());
        if (lg[0] > 1)
            reduce_dim(lg[0], kg[1], 1);
        else if (lg[1] / cfg_mutable_.exec_cfg().simd() > 1)
            reduce_dim(lg[1], kg[0], cfg_mutable_.exec_cfg().simd());
        else
            ir_error_not_expected() << "minimal loop_grid too large!";

        cfg_mutable_.set_kernel_grid(kg);
        cfg_mutable_.set_loop_grid(lg);
        ki_mutable_.set_nd_range(nd_range(cfg_mutable_));
    }
}

class loop_bound_counter_t : public ir_mutator_t {
public:
    int count(const expr_t &e) {
        const auto retn = simplify(mutate(e));
        ir_assert(retn.is<int_imm_t>());
        return to_cpp<int>(retn);
    }
    loop_bound_counter_t(const gemm_schedule_t &s) : schedule_(s) {}

private:
    object_t _mutate(const var_t &v) override {
        return expr_t(schedule_.var_bound(v) - 1);
    }
    const gemm_schedule_t &schedule_;
};

stmt_t pooling_ir_builder_t::try_build(pooling_ir_builder_t &pb,
        const kernel_info_t &ki, const pooling_config_t &cfg,
        const primitive_desc_t &pd) {
    const auto &exec = cfg.exec_cfg();
    const auto &conf = cfg.pool_conf();
    const auto &src_layout = cfg.src_layout().user();
    const auto &dst_layout = cfg.dst_layout().user();

    ir_assert(src_layout.ndims() == dst_layout.ndims());

    // Create loop variables.
    auto mb = var_t::make(type_t::s32(), "mb");
    auto oc = var_t::make(type_t::s32(), "oc");

    auto od = var_t::make(type_t::s32(), "od");
    auto oh = var_t::make(type_t::s32(), "oh");
    auto ow = var_t::make(type_t::s32(), "ow");

    auto kd = var_t::make(type_t::s32(), "kd");
    auto kh = var_t::make(type_t::s32(), "kh");
    auto kw = var_t::make(type_t::s32(), "kw");

    // Initialize masks.
    const bool check_iw = utils::need_src_or_dst_check(!conf.is_backward,
            conf.ow, conf.iw, conf.kw, conf.l_pad, conf.stride_w, conf.dw);
    const bool check_ih = utils::need_src_or_dst_check(!conf.is_backward,
            conf.oh, conf.ih, conf.kh, conf.t_pad, conf.stride_h, conf.dh);
    const bool check_id = utils::need_src_or_dst_check(!conf.is_backward,
            conf.od, conf.id, conf.kd, conf.f_pad, conf.stride_d, conf.dd);
    const bool check_idhw = check_id || check_ih || check_iw;

    auto &x = view_t::placeholder_var();

    expr_t id_mask, ih_mask, iw_mask;
    if (check_id) id_mask = (x >= 0) & (x < conf.id);
    if (check_ih) ih_mask = (x >= 0) & (x < conf.ih);
    if (check_iw) iw_mask = (x >= 0) & (x < conf.iw);

    const int simd = exec.simd();
    const auto &lg = cfg.loop_grid();
    const auto &kg = cfg.kernel_grid();
    const auto &tg = cfg.thread_group_grid();
    const auto &dims_grid = cfg.dims_padded();
    std::vector<int> dims(dims_grid.ndims());
    for (int i = 0; i < int(dims.size()); i++)
        dims[i] = dims_grid[i];

    // Source.
    auto src_view = view_t({mb, oc, od, oh, ow, kd, kh, kw}, 5);
    src_view.set_vdim(mb, dims[0]);
    src_view.set_vdim(oc, dims[1]);
    src_view.set_vdim(od, dims[2]);
    src_view.set_vdim(oh, dims[3]);
    src_view.set_vdim(ow, dims[4]);
    src_view.set_vdim(kd, conf.kd);
    src_view.set_vdim(kh, conf.kh);
    src_view.set_vdim(kw, conf.kw);
    src_view.set_tdim(0, mb);
    src_view.set_tdim(1, oc);
    src_view.set_tdim(
            2, od * conf.stride_d - conf.f_pad + kd * (1 + conf.dd), id_mask);
    src_view.set_tdim(
            3, oh * conf.stride_h - conf.t_pad + kh * (1 + conf.dh), ih_mask);
    src_view.set_tdim(
            4, ow * conf.stride_w - conf.l_pad + kw * (1 + conf.dw), iw_mask);
    src_view.set_tlayout(src_layout);
    src_view.set_tmasks(dims);

    // Destination.
    auto dst_view = view_t({mb, oc, od, oh, ow}, 5);
    dst_view.set_vdim(mb, dims[0]);
    dst_view.set_vdim(oc, dims[1]);
    dst_view.set_vdim(od, dims[2]);
    dst_view.set_vdim(oh, dims[3]);
    dst_view.set_vdim(ow, dims[4]);
    dst_view.set_tdim(0, mb);
    dst_view.set_tdim(1, oc);
    dst_view.set_tdim(2, od);
    dst_view.set_tdim(3, oh);
    dst_view.set_tdim(4, ow);
    dst_view.set_tlayout(dst_layout);
    dst_view.set_tmasks(dims);

    constraint_set_t init_cset;
    std::vector<stmt_t> init_stmts;
    pb.init_kernel_grid(kg, tg, simd, init_cset, init_stmts);

    gemm_schedule_t schedule(init_cset, kg, tg);
    schedule.set_view(src_view);
    schedule.set_view(dst_view);

    auto kg_bind = [&](const std::vector<expr_t> &fuse, int idx) {
        if (fuse.size() > 1)
            schedule.bind(schedule.fuse(fuse), kg.idx(idx - 2));
        else if (fuse.size() == 1)
            schedule.bind(fuse[0], kg.idx(idx - 2));
    };
    auto odhw_to_schedule = [&](expr_t s1, expr_t ns, expr_t s0) {
        int s0_idx = (s0.is_empty()) ? -1 : src_view.vvar_index(s0);
        int s1_idx = src_view.vvar_index(s1);
        int ns_idx = src_view.vvar_index(ns);
        ir_assert((s0_idx <= 4) && (s1_idx <= 4) && (ns_idx <= 4));

        // s1 and ns may swap sides, which affects their fusing order: it has
        // to strictly replicate that of the arguments passed to this lambda!
        const bool need_swap = (s1_idx >= 0) && (s1_idx <= 1);
        // 2 spatials and 2 non-spatials disallowed; only 1 of each or bust
        ir_assert(need_swap != ((ns_idx >= 0) && (ns_idx <= 1)));
        if (need_swap) {
            std::swap(s1_idx, ns_idx);
            std::swap(s1, ns);
        }

        const int s1_tlg_unroll = lg[s1_idx];
        const int s1_unroll = s1_tlg_unroll * tg[s1_idx - 2];
        const auto ps1 = s1.str();

        std::vector<expr_t> s0_fuse, s1_fuse;

        expr_t s1_kg, s1_tlg, s1_tg, s1_lg;
        schedule.split(s1, s1_unroll, s1_kg, s1_tlg, ps1 + "_kg", ps1 + "_tlg");
        schedule.split(
                s1_tlg, s1_tlg_unroll, s1_tg, s1_lg, ps1 + "_tg", ps1 + "_lg");

        schedule.tensorize(s1_lg);
        schedule.bind(s1_tg, tg.idx(s1_idx - 2));
        s1_fuse.emplace_back(s1_kg);

        if (s0_idx >= 0) {
            ir_assert(s0_idx == s1_idx + 1);
            const int s0_tlg_unroll = lg[s0_idx];
            const int s0_unroll = s0_tlg_unroll * tg[s0_idx - 2];
            const int s0_full = s0_unroll * kg[s0_idx - 2];
            const auto ps0 = s0.str();

            if (dims[s0_idx] > s0_full) {
                expr_t s0_split, s0_ktlg; // part of kg[s0] is in kg[s1]
                schedule.split(s0, s0_full, s0_split, s0_ktlg, ps0 + "_split",
                        ps0 + "_ktlg");
                s1_fuse.emplace_back(s0_split);
                s0 = s0_ktlg;
            } else if (dims[s0_idx] <= utils::div_up(s0_full, 2)) {
                expr_t s1_split, s1_ktlg; // part of kg[s1] is in kg[s0]
                const int s1_ext = utils::div_up(s0_full, dims[s0_idx]);
                schedule.split(s1_fuse[0], s1_ext, s1_ktlg, s1_split,
                        ps1 + "_ktlg", ps1 + "_split");
                s1_fuse[0] = s1_ktlg;
                s0_fuse.emplace_back(s1_split);
            }

            expr_t s0_kg, s0_tlg, s0_tg, s0_lg;
            schedule.split(
                    s0, s0_unroll, s0_kg, s0_tlg, ps0 + "_kg", ps0 + "_tlg");
            schedule.split(s0_tlg, s0_tlg_unroll, s0_tg, s0_lg, ps0 + "_tg",
                    ps0 + "_lg");

            schedule.tensorize(s0_lg);
            schedule.bind(s0_tg, tg.idx(s0_idx - 2));
            s0_fuse.emplace_back(s0_kg);
        }

        const int ns_unroll = lg[ns_idx];
        const auto pns = ns.str();

        expr_t ns_kg, ns_lg;
        schedule.split(ns, ns_unroll, ns_kg, ns_lg, pns + "_kg", pns + "_lg");
        if (need_swap)
            s1_fuse.emplace(s1_fuse.begin(), ns_kg);
        else
            s1_fuse.emplace_back(ns_kg);
        schedule.tensorize(ns_lg);

        kg_bind(s0_fuse, s0_idx);
        kg_bind(s1_fuse, s1_idx);
    };
    odhw_to_schedule(oc, od, expr_t());
    if ((src_layout.blocks()[1].dim_idx == 0) || (dims[0] < dims[1]))
        odhw_to_schedule(oh, mb, ow);
    else
        odhw_to_schedule(mb, oh, ow);

    auto kdhw_to_schedule = [&](const expr_t &k) {
        const int k_idx = src_view.vvar_index(k);
        ir_assert((k_idx >= 5) && (k_idx <= 7));
        const int k_dim = lg[k_idx];
        if (k_dim == schedule.var_bound(k)) {
            schedule.tensorize(k);
        } else if (k_dim < schedule.var_bound(k)) {
            if (k_dim > 1) { // otherwise it'll just waste a variable
                expr_t k_lg, k_tnz;
                schedule.split(k, k_dim, k_lg, k_tnz, k.str() + "_lg",
                        k.str() + "_tnz");
                schedule.tensorize(k_tnz);
            }
        } else {
            ir_error_not_expected() << "k_dim > var_bound; this is wrong";
        }
    };
    kdhw_to_schedule(kd);
    kdhw_to_schedule(kh);
    kdhw_to_schedule(kw);

    schedule.finalize();

    const auto expand_loop_kinds = loop_kind_t::serial
            | loop_kind_t::kernel_grid | loop_kind_t::tg_grid;
    mb = schedule.expand(mb, true, expand_loop_kinds);
    oc = schedule.expand(oc, true, expand_loop_kinds);
    od = schedule.expand(od, true, expand_loop_kinds);
    oh = schedule.expand(oh, true, expand_loop_kinds);
    ow = schedule.expand(ow, true, expand_loop_kinds);

    auto src_thr_tile = schedule.thr_view_tile(src_view, /*is_relative=*/false);
    auto src_thr_view = src_view.create_sub_view(src_thr_tile);

    auto dst_thr_tile = schedule.thr_view_tile(dst_view, /*is_relative=*/false);
    auto dst_thr_view = dst_view.create_sub_view(dst_thr_tile);

    const auto &src_buf = ki.arg_var(0);
    const auto &dst_buf = ki.arg_var(1);

    std::vector<stmt_t> allocs;
    for (int i = 0; i < ki.nargs(); i++) {
        auto &var = ki.arg_var(i);
        if (!var.type().is_ptr()) continue;
        allocs.push_back(alloc_t::make(var, 0, alloc_kind_t::global));
    }

    ir_context_t ir_ctx(exec, init_cset);

    auto read_buf = ir_ctx.create_tmp_var(type_t::byte_ptr(), "read");
    auto read_params = get_send_params(
            exec, send_op_t::load, send_address_t::a64, src_thr_view);
    read_params.try_legacy = false;
    auto read = make_access_builder(ir_ctx, src_thr_view, src_buf, read_buf,
            read_params, /*zero_out=*/false);
    allocs.push_back(
            alloc_t::make(read_buf, read.reg_buf_size(), alloc_kind_t::grf));
    const auto &read_layout = read.reg_layout();

    // shall only get used on empty mb's; for all else there's epilogue builder
    auto write_params = get_send_params(
            exec, send_op_t::store, send_address_t::a64, dst_thr_view);
    write_params.try_legacy = false;
    auto write = make_access_builder(
            ir_ctx, dst_thr_view, dst_buf, read_buf, write_params);
    const auto &write_layout = write.reg_layout();

    tensor_t src_tile(read_layout.split_into_max_tile(simd, true));
    tensor_t dst_tile(write_layout.split_into_max_tile(simd, true));
    ir_assert(src_tile.elems() == simd);
    ir_assert(dst_tile.elems() == simd);

    const bool is_identity(conf.kd * conf.kh * conf.kw <= 1);
    const bool is_max(conf.alg == alg_kind_t::dnnl_pooling_max);
    const bool is_pad(conf.alg == alg_kind_t::dnnl_pooling_avg_include_padding);

    const type_t read_type(read_layout.type().kind(), simd);
    const type_t write_type(write_layout.type().kind(), simd);

    type_t acc_type = (!read_type.is_int())
            ? (is_max) ? read_type : type_t::f32(simd)
            : type_t::s32(simd);
    auto acc_buf = ir_ctx.create_tmp_var(type_t::byte_ptr(), "acc");
    const auto acc_sc_size = acc_type.scalar().size();
    const auto acc_size = acc_sc_size * lg[4] * lg[3] * lg[2] * lg[1] * lg[0];

    stmt_t stmt;

    if (is_identity) {
        acc_buf = read_buf;
        acc_type = read_type;
        stmt = read.stmt();
    } else {
        ir_assert(acc_size % simd == 0);
        allocs.push_back(alloc_t::make(acc_buf, acc_size, alloc_kind_t::grf));

        stmt_t fill_stmt, compute_stmt = read.stmt();

        if (!read_type.is_int()) {
            auto init = shuffle_t::make_broadcast(
                    (is_max) ? -INFINITY : 0.f, simd);
            stmt = stmt_t();
            for (int i = 0; i < acc_size; i += simd * acc_sc_size)
                stmt = stmt.append(store_t::make(
                        acc_buf, i, cast_t::make(acc_type, init)));
            read_layout.for_each_tile(
                    src_tile, [&](const std::vector<dim_t> &s) {
                        int off = read_layout(s) * read_layout.type().size();
                        auto op = cast_t::make(read_type, init);
                        fill_stmt = fill_stmt.append(
                                store_t::make(read_buf, off, op));
                    });
        } else {
            const bool is_neg = is_max && read_type.is_signed();
            const int mult = int(sizeof(int32_t)) / read_layout.type().size();
            stmt = stmt_t();
            for (int i = 0; i < acc_size; i += simd * acc_sc_size)
                stmt = stmt.append(store_t::make(acc_buf, i,
                        shuffle_t::make_broadcast(
                                cast_t::make(type_t::s32(),
                                        (is_neg) ? 0x80000000 : 0x00000000),
                                simd)));
            expr_t v_init;
            switch (mult) {
                case 1: v_init = (is_neg) ? 0x80000000 : 0x00000000; break;
                case 2: v_init = (is_neg) ? 0x80008000 : 0x00000000; break;
                case 4: v_init = (is_neg) ? 0x80808080 : 0x00000000; break;
                default: ir_assert(false) << "Not expected.";
            }
            v_init = cast_t::make(type_t::s32(), v_init);
            auto op = shuffle_t::make_broadcast(v_init, simd / mult);
            read_layout.for_each_tile(
                    src_tile, [&](const std::vector<dim_t> &s) {
                        int off = read_layout(s) * read_layout.type().size();
                        fill_stmt = fill_stmt.append(
                                store_t::make(read_buf, off, op));
                    });
        }

        read_layout.for_each_tile(src_tile, [&](const std::vector<dim_t> &s) {
            const int off_l = read_layout(s) * read_layout.type().size();
            const int off_a = (s[0] * lg[1] + s[1]) * acc_sc_size;

            auto load = cast_t::make(
                    acc_type, load_t::make(read_type, read_buf, off_l));
            auto acc = load_t::make(acc_type, acc_buf, off_a);
            auto op = binary_op_t::make(
                    (is_max) ? op_kind_t::_max : op_kind_t::_add, acc, load);
            compute_stmt
                    = compute_stmt.append(store_t::make(acc_buf, off_a, op));
        });

        stmt = stmt.append(schedule.create_loop_nest(
                (check_idhw) ? fill_stmt.append(compute_stmt) : compute_stmt));

        if (!is_max) {
            expr_t filter(conf.kd * conf.kh * conf.kw);
            if (!is_pad && check_idhw) {
                auto dim = [](const expr_t &o, int s, int p, int k, int i) {
                    if (k <= 1) return expr_t(1);
                    return binary_op_t::make(op_kind_t::_min, o * s - p + k, i)
                            - binary_op_t::make(op_kind_t::_max, o * s - p, 0);
                };
                auto dhw = dim(od, conf.stride_d, conf.f_pad, conf.kd, conf.id)
                        * dim(oh, conf.stride_h, conf.t_pad, conf.kh, conf.ih)
                        * dim(ow, conf.stride_w, conf.l_pad, conf.kw, conf.iw);
                filter = cast_t::make(type_t::f32(), dhw);
            }
            filter = shuffle_t::make_broadcast(filter, simd);
            for (int i = 0; i < acc_size; i += simd * acc_sc_size) {
                auto acc = cast_t::make(
                        type_t::f32(simd), load_t::make(acc_type, acc_buf, i));
                stmt = stmt.append(store_t::make(acc_buf, i, acc / filter));
            }
            acc_type = type_t::f32(simd);
        }
    }

    int buf_size = 0;
    pooling_post_op_view_mapper_t view_mapper(dst_view, conf.ndims);
    post_op_context_t post_op_ctx(*pd.attr(), cfg.zp_cfg(), schedule, ki,
            *pd.invariant_dst_md(), *pd.invariant_dst_md(), view_mapper);
    stmt = stmt.append(create_epilogue_stmt(exec, ir_ctx, schedule,
            /*force_c_reorder=*/false, post_op_ctx, dst_thr_tile,
            write_layout.retype(acc_type.scalar()), dst_buf, acc_buf,
            buf_size));

    if (dims[0] > conf.mb) {
        stmt_t stop;
        auto zero_bcast
                = cast_t::make(read_type, shuffle_t::make_broadcast(0, simd));
        for (int i = 0; i < acc_size / acc_sc_size; i += simd)
            stop = stop.append(store_t::make(
                    read_buf, i * read_type.scalar().size(), zero_bcast));
        auto stop_cond = shuffle_t::make_broadcast(mb >= conf.mb, simd);
        stmt = if_t::make(stop_cond, stop.append(write.stmt()), stmt);
    }

    loop_bound_counter_t lbc(schedule);
    auto exit_cond = (lbc.count(ow) >= conf.ow) ? (ow < conf.ow) : expr_t();
    if (lbc.count(oh) >= conf.oh)
        exit_cond = (!exit_cond.is_empty()) ? (oh < conf.oh) & exit_cond
                                            : (oh < conf.oh);
    if (lbc.count(od) >= conf.od)
        exit_cond = (!exit_cond.is_empty()) ? (od < conf.od) & exit_cond
                                            : (od < conf.od);
    if (!exit_cond.is_empty())
        stmt = if_t::make(shuffle_t::make_broadcast(exit_cond, simd), stmt);

    stmt = schedule.create_bind_stmt(stmt);
    stmt = inject_let_stmts(stmt, init_stmts);
    stmt = inject_alloc_stmts(stmt, allocs);
    stmt = inject_external_var_let(stmt, ir_ctx);

    stmt = simplify(stmt, ir_ctx);
    stmt = lift_buffer_offsets_in_send(stmt, ir_ctx);
    stmt = inject_send(stmt, ir_ctx);
    stmt = split_wide_stores(stmt, ir_ctx);
    stmt = fix_int32_overflow(stmt, ir_ctx);
    stmt = eliminate_common_subexprs(
            stmt, ir_ctx, exec.regs() * exec.grf_size());
    stmt = simplify(stmt, ir_ctx);
    stmt = optimize_alloc_let(stmt, ir_ctx);
    stmt = stmt_group_t::make(stmt_label_t::kernel(), stmt);

    const int regs = get_peak_regs(stmt, exec.grf_size());

    ir_trace() << "Pooling kernel body:\n" << stmt << std::endl;
    ir_trace() << "Pooling cfg (" << regs << " regs):\n" << cfg << std::endl;

    return (regs > exec.regs()) ? stmt_t() : stmt;
}

compute::nd_range_t pooling_ir_builder_t::nd_range(
        const pooling_config_t &cfg) {
    const auto &kg = cfg.kernel_grid();
    const auto &tg = cfg.thread_group_grid();
    std::array<size_t, 3> local {size_t(tg[0] * cfg.exec_cfg().simd()),
            size_t(tg[1]), size_t(tg[2])};
    std::array<size_t, 3> global {size_t(kg[0]) * local[0],
            size_t(kg[1]) * local[1], size_t(kg[2]) * local[2]};

    return compute::nd_range_t(global.data(), local.data());
}

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl
