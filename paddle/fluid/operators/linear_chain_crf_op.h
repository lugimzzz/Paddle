/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once
#include "paddle/fluid/framework/eigen.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/phi/kernels/funcs/math_function.h"

namespace paddle {
namespace operators {

template <typename T>
static inline T NormalizeL1(T* x, size_t len) {
  T sum = 0.;
  for (size_t i = 0; i < len; ++i) sum += x[i];
  // (This comment is from the old LinearChainCRFLayer.)
  // Right now, we just bet that sum won't be zero. If this really happens, we
  // will figure out what should be done then.
  PADDLE_ENFORCE_GT(
      sum,
      0.,
      platform::errors::InvalidArgument(
          "The unnormalized probabilities of all possible unfinished "
          "sequences must be greater than 0."));
  T s = 1. / sum;
  for (size_t i = 0; i < len; ++i) x[i] *= s;
  return sum;
}

template <typename T>
struct ScalarMul {
  explicit ScalarMul(const T& scalar) : scalar(scalar) {}
  T operator()(const T& val) const { return val * scalar; }

  T scalar;
};

using framework::LoD;

template <typename T, typename DeviceContext>
class LinearChainCRFOpKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    const phi::DenseTensor* emission_weights =
        ctx.Input<phi::DenseTensor>("Emission");
    const phi::DenseTensor* transition_weights =
        ctx.Input<phi::DenseTensor>("Transition");

    phi::DenseTensor* emission_exps =
        ctx.Output<phi::DenseTensor>("EmissionExps");
    phi::DenseTensor* transition_exps =
        ctx.Output<phi::DenseTensor>("TransitionExps");
    phi::DenseTensor* alpha = ctx.Output<phi::DenseTensor>("Alpha");
    phi::DenseTensor* ll = ctx.Output<phi::DenseTensor>("LogLikelihood");

    // Because the computation codes only runs on CPU, here the memory for all
    // the outputs is FIXED to be allocated on the CPU memory.
    emission_exps->mutable_data<T>(platform::CPUPlace());
    alpha->mutable_data<T>(platform::CPUPlace());
    transition_exps->mutable_data<T>(platform::CPUPlace());
    auto emission_dims = emission_weights->dims();

    const phi::DenseTensor* label = ctx.Input<phi::DenseTensor>("Label");
    phi::DenseTensor emission_weights_tmp = *emission_weights;
    phi::DenseTensor label_tmp = *label;
    phi::DenseTensor emission_exps_tmp = *emission_exps;
    phi::DenseTensor alpha_tmp = *alpha;
    int64_t seq_num = 0;
    int64_t batch_size;
    int64_t tag_num;
    const int64_t* length_data = nullptr;
    framework::LoD in_lod;
    if (ctx.HasInput("Length")) {
      const phi::DenseTensor* label_length =
          ctx.Input<phi::DenseTensor>("Length");
      length_data = label_length->data<int64_t>();
      seq_num = label_length->numel();
      PADDLE_ENFORCE_EQ(
          seq_num,
          emission_dims[0],
          platform::errors::InvalidArgument(
              "the size of Input(length) must be equal to "
              "emission_dims[0]. But input_size = %d, emission_dims[0] = %d.",
              seq_num,
              emission_dims[0]));
      auto label_dims = label->dims();
      PADDLE_ENFORCE_EQ(
          seq_num,
          label_dims[0],
          platform::errors::InvalidArgument(
              "the size of Input(length) must be equal to "
              "label_dims[0]. But input_size = %d, label_dims[0] = %d.",
              seq_num,
              label_dims[0]));

      batch_size = emission_dims[0] * emission_dims[1];
      tag_num = emission_dims[2];
      emission_weights_tmp.Resize({batch_size, tag_num});
      label_tmp.Resize({batch_size, 1});
      alpha_tmp.Resize({batch_size, tag_num});
      emission_exps_tmp.Resize({batch_size, tag_num});
      phi::funcs::set_constant(
          ctx.device_context(), emission_exps, static_cast<T>(0.0));
      phi::funcs::set_constant(
          ctx.device_context(), alpha, static_cast<T>(0.0));
    } else {
      in_lod = ctx.Input<phi::DenseTensor>("Label")->lod();
      PADDLE_ENFORCE_NE(in_lod.size(),
                        0,
                        platform::errors::InvalidArgument(
                            "Input(Label) must be a sequence."));
      seq_num = in_lod[0].size() - 1;
      batch_size = emission_dims[0];
      tag_num = emission_dims[1];
    }

    // Resize the output tensor to its correct dimension.
    ll->Resize({seq_num, 1});
    ll->mutable_data<T>(platform::CPUPlace());
    // Now, all the inputs and outputs should be on the CPU memory.
    phi::DenseTensor emission_row_max;
    emission_row_max.mutable_data<T>(
        common::make_ddim({static_cast<int64_t>(batch_size), 1}),
        platform::CPUPlace());
    auto& place =
        *ctx.template device_context<phi::CPUContext>().eigen_device();
    auto x = framework::EigenMatrix<T>::From(emission_weights_tmp);
    auto x_row_max = framework::EigenMatrix<T>::From(emission_row_max);
    x_row_max.device(place) =
        x.maximum(Eigen::DSizes<int, 1>(1))
            .reshape(Eigen::DSizes<int, 2>(static_cast<int>(batch_size), 1));
    auto x_exps = framework::EigenMatrix<T>::From(emission_exps_tmp);
    x_exps.device(place) =
        (x - x_row_max.broadcast(Eigen::DSizes<int, 2>(1, tag_num))).exp();
    auto w = framework::EigenMatrix<T>::From(*transition_weights);
    auto w_exps = framework::EigenMatrix<T>::From(*transition_exps);
    w_exps.device(place) = w.exp();
    T* log_likelihood = ll->data<T>();
    for (int64_t i = 0; i < seq_num; ++i) {
      int64_t start_pos = 0;
      int64_t end_pos = 0;
      if (ctx.HasInput("Length")) {
        start_pos = i * emission_dims[1];
        end_pos = start_pos + length_data[i];
      } else {
        start_pos = static_cast<int64_t>(in_lod[0][i]);
        end_pos = static_cast<int64_t>(in_lod[0][i + 1]);
      }
      if (end_pos == start_pos) {
        // If an empty input sequence is given, pad 0 for its cost.
        log_likelihood[i] = 0.;
        continue;
      }
      const phi::DenseTensor one_seq =
          emission_weights_tmp.Slice(start_pos, end_pos);
      phi::DenseTensor one_seq_row_max =
          emission_row_max.Slice(start_pos, end_pos);
      phi::DenseTensor one_seq_exps =
          emission_exps_tmp.Slice(start_pos, end_pos);
      const phi::DenseTensor one_seq_label =
          label_tmp.Slice(start_pos, end_pos);
      phi::DenseTensor one_seq_alpha = alpha_tmp.Slice(start_pos, end_pos);
      log_likelihood[i] = ForwardOneSequence(one_seq,
                                             one_seq_row_max,
                                             one_seq_exps,
                                             *transition_weights,
                                             *transition_exps,
                                             one_seq_label,
                                             &one_seq_alpha);
    }
  };

 private:
  T ForwardOneSequence(const phi::DenseTensor& emission,
                       const phi::DenseTensor& emission_row_max,
                       const phi::DenseTensor& emission_exps,
                       const phi::DenseTensor& trans_weights,
                       const phi::DenseTensor& trans_weight_exps,
                       const phi::DenseTensor& label,
                       phi::DenseTensor* alpha) const {
    const T* x = emission.data<T>();
    const T* x_row_max = emission_row_max.data<T>();
    const T* x_exps = emission_exps.data<T>();
    const T* w = trans_weights.data<T>();
    const T* w_exps = trans_weight_exps.data<T>();
    T* alpha_value = alpha->data<T>();

    auto x_dims = emission.dims();
    const size_t seq_length = x_dims[0];
    const size_t tag_num = x_dims[1];
    // The 1st row of w are transition weights for start mask.
    // The 2nd row of w are transition weights for end mask.
    // Transition weights between other tags begin from the 3rd row of w.
    const size_t state_trans_base_idx = 2;

    for (size_t i = 0; i < tag_num; ++i) {
      alpha_value[i] = w_exps[i] * x_exps[i];
    }
    T ll = -x_row_max[0] - std::log(NormalizeL1<T>(alpha_value, tag_num));

    for (size_t k = 1; k < seq_length; ++k) {
      for (size_t i = 0; i < tag_num; ++i) {
        T sum = 0.;
        for (size_t j = 0; j < tag_num; ++j) {
          sum += alpha_value[(k - 1) * tag_num + j] *  // (*)
                 w_exps[(j + state_trans_base_idx) * tag_num + i];
        }
        alpha_value[k * tag_num + i] = x_exps[k * tag_num + i] * sum;
      }
      // NormalizeL1 is to avoid underflow or overflow at (*).
      ll -= x_row_max[k] +
            std::log(NormalizeL1<T>(alpha_value + k * tag_num, tag_num));
    }
    T sum = 0.;
    for (size_t i = 0; i < tag_num; ++i) {
      sum += alpha_value[(seq_length - 1) * tag_num + i] * w_exps[tag_num + i];
    }
    ll -= std::log(sum);
    // Now ll is equal to -log(Z).

    const int64_t* lbl = label.data<int64_t>();
    PADDLE_ENFORCE_LT(
        static_cast<size_t>(*std::max_element(lbl, lbl + seq_length)),
        tag_num,
        platform::errors::InvalidArgument(
            "An invalid tag label that execesses the largest tag number."));

    // Calculate the nominator part, which depends on the label sequence.
    ll += w[lbl[0]] /*start transition*/ + x[lbl[0]] +
          w[tag_num + lbl[seq_length - 1]] /*end transition*/;
    for (size_t k = 1; k < seq_length; ++k) {
      ll += x[k * tag_num + lbl[k]] +
            w[(lbl[k - 1] + state_trans_base_idx) * tag_num + lbl[k]];
    }
    return -ll;
  }
};

template <typename T, typename DeviceContext>
class LinearChainCRFGradOpKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    const phi::DenseTensor* label = ctx.Input<phi::DenseTensor>("Label");
    const phi::DenseTensor* emission_exps =
        ctx.Input<phi::DenseTensor>("EmissionExps");
    const phi::DenseTensor* transition_exps =
        ctx.Input<phi::DenseTensor>("TransitionExps");
    const phi::DenseTensor* alpha = ctx.Input<phi::DenseTensor>("Alpha");
    const T* ll_grad =
        ctx.Input<phi::DenseTensor>(framework::GradVarName("LogLikelihood"))
            ->data<T>();
    phi::DenseTensor* emission_grad =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("Emission"));
    auto* emission_grad_data =
        emission_grad->mutable_data<T>(platform::CPUPlace());
    memset(emission_grad_data, 0, emission_grad->numel() * sizeof(T));
    phi::DenseTensor alpha_tmp = *alpha;
    phi::DenseTensor label_tmp = *label;
    phi::DenseTensor emission_exps_tmp = *emission_exps;
    phi::DenseTensor emission_grad_tmp = *emission_grad;
    // getting seq_num  using padding or not
    int64_t seq_num = 0;
    framework::LoD in_lod;
    const int64_t* length_data = nullptr;
    if (ctx.HasInput("Length")) {
      const phi::DenseTensor* label_length =
          ctx.Input<phi::DenseTensor>("Length");
      length_data = label_length->data<int64_t>();
      seq_num = label_length->numel();
      auto emission_dims = emission_grad->dims();
      auto label_dims = label->dims();
      emission_grad_tmp.Resize(
          {emission_dims[0] * emission_dims[1], emission_dims[2]});
      label_tmp.Resize({label_dims[0] * label_dims[1], 1});
      alpha_tmp.Resize({emission_dims[0] * emission_dims[1], emission_dims[2]});
      emission_exps_tmp.Resize(
          {emission_dims[0] * emission_dims[1], emission_dims[2]});
    } else {
      in_lod = ctx.Input<phi::DenseTensor>("Label")->lod();
      PADDLE_ENFORCE_NE(in_lod.size(),
                        0,
                        platform::errors::InvalidArgument(
                            "Input(Label) must be a sequence."));
      seq_num = static_cast<int64_t>(in_lod[0].size() - 1);
    }

    phi::DenseTensor* transition_grad =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("Transition"));

    // TODO(caoying) Fix this constraint. When the Input(Emission) is from the
    // data reader operator, it can have no gradients.
    if (transition_grad) {
      transition_grad->mutable_data<T>(platform::CPUPlace());
      phi::funcs::set_constant(
          ctx.device_context(), transition_grad, static_cast<T>(0.));
    }
    // Now, all the inputs and outputs should be on the CPU memory.
    auto emission_dims = emission_exps->dims();
    // Beta is the memo table used in dynamic programming to calculate the
    // backwark vectors. For a backward vector i (the i-th row of beta), it
    // captures the unnormalized probabilities of partial sequences starting
    // at position i.
    phi::DenseTensor beta;
    beta.mutable_data<T>(emission_dims, platform::CPUPlace());
    if (ctx.HasInput("Length")) {
      beta.Resize({emission_dims[0] * emission_dims[1], emission_dims[2]});
    }

    for (int64_t i = 0; i < seq_num; ++i) {
      int64_t start_pos = 0;
      int64_t end_pos = 0;
      if (ctx.HasInput("Length")) {
        start_pos = i * emission_dims[1];
        end_pos = start_pos + length_data[i];
      } else {
        start_pos = static_cast<int64_t>(in_lod[0][i]);
        end_pos = static_cast<int64_t>(in_lod[0][i + 1]);
      }

      if (end_pos == start_pos) {
        continue;
      }
      const phi::DenseTensor one_seq_emission_exps =
          emission_exps_tmp.Slice(start_pos, end_pos);
      const phi::DenseTensor one_seq_label =
          label_tmp.Slice(start_pos, end_pos);
      const phi::DenseTensor one_seq_alpha =
          alpha_tmp.Slice(start_pos, end_pos);
      phi::DenseTensor one_seq_beta = beta.Slice(start_pos, end_pos);
      phi::DenseTensor one_seq_emission_grad =
          emission_grad_tmp.Slice(start_pos, end_pos);
      BackwardOneSequence(ctx.template device_context<phi::CPUContext>(),
                          ll_grad[i],
                          one_seq_emission_exps,
                          *transition_exps,
                          one_seq_alpha,
                          one_seq_label,
                          &one_seq_beta,
                          transition_grad,
                          &one_seq_emission_grad);
    }
  };

 private:
  void BackwardOneSequence(const phi::CPUContext& ctx,
                           const T ll_grad,
                           const phi::DenseTensor& emission_exps,
                           const phi::DenseTensor& transition_exps,
                           const phi::DenseTensor& alpha,
                           const phi::DenseTensor& label,
                           phi::DenseTensor* beta,
                           phi::DenseTensor* transition_grad,
                           phi::DenseTensor* emission_grad) const {
    const T* w_exps = transition_exps.data<T>();
    const T* x_exps = emission_exps.data<T>();
    const int64_t* label_value = label.data<int64_t>();
    T* beta_value = beta->data<T>();
    auto x_dims = emission_exps.dims();
    const size_t seq_length = x_dims[0];
    const size_t tag_num = x_dims[1];
    const size_t state_trans_base_idx = 2;

    // Calculate the backward vectors: beta.
    // First, calculate the initialition state.
    for (size_t i = 0; i < tag_num; ++i) {
      beta_value[(seq_length - 1) * tag_num + i] = w_exps[tag_num + i];
    }
    NormalizeL1<T>(beta_value + (seq_length - 1) * tag_num, tag_num);
    for (int k = static_cast<int>(seq_length) - 2; k >= 0; --k) {
      for (size_t i = 0; i < tag_num; ++i) {
        T sum = 0.;
        for (size_t j = 0; j < tag_num; ++j) {
          sum += w_exps[(i + state_trans_base_idx) * tag_num + j] *  // (**)
                 x_exps[(k + 1) * tag_num + j] *
                 beta_value[(k + 1) * tag_num + j];
        }
        beta_value[k * tag_num + i] = sum;
      }
      // NormalizeL1 is to avoid underflow or overflow at (**).
      NormalizeL1<T>(beta_value + k * tag_num, tag_num);
    }

    auto x_grad_mat = framework::EigenMatrix<T>::From(*emission_grad);
    auto alpha_mat = framework::EigenMatrix<T>::From(alpha);
    auto beta_mat = framework::EigenMatrix<T>::From(*beta);

    auto* place = ctx.eigen_device();
    auto prob = alpha_mat * beta_mat;
    auto row_sum = prob.sum(Eigen::DSizes<int, 1>(1))
                       .reshape(Eigen::DSizes<int, 2>(seq_length, 1))
                       .broadcast(Eigen::DSizes<int, 2>(1, tag_num));
    x_grad_mat.device(*place) =
        (prob / row_sum).unaryExpr(ScalarMul<T>(ll_grad));

    for (size_t k = 0; k < seq_length; ++k) {
      x_grad_mat(k, label_value[k]) -= static_cast<T>(ll_grad);
    }

    if (transition_grad) {
      T* trans_grad = transition_grad->data<T>();
      for (size_t k = 0; k < tag_num; ++k) {
        // Do not multiply by the output gradient here, because x_grad_mat has
        // alrealy done this.
        trans_grad[k] += x_grad_mat(/*from start state*/ 0, k);
        trans_grad[tag_num + k] +=
            x_grad_mat(/*to end state*/ seq_length - 1, k);
      }

      auto x_exps_mat = framework::EigenMatrix<T>::From(emission_exps);

      // TODO(caoying): Fix this to avoid using this local variable if we can
      // profile the training process.
      phi::DenseTensor tmp;
      tmp.mutable_data<T>(beta->dims(), platform::CPUPlace());
      auto tmp_mat = framework::EigenMatrix<T>::From(tmp);
      auto prob = beta_mat * x_exps_mat;
      auto row_sum = prob.sum(Eigen::DSizes<int, 1>(1))
                         .reshape(Eigen::DSizes<int, 2>(seq_length, 1))
                         .broadcast(Eigen::DSizes<int, 2>(1, tag_num));
      tmp_mat.device(*place) = prob / row_sum;

      for (size_t k = 1; k < seq_length; ++k) {
        T sum = 0.;
        for (size_t i = 0; i < tag_num; ++i) {
          for (size_t j = 0; j < tag_num; ++j) {
            sum += w_exps[(i + state_trans_base_idx) * tag_num + j] *  // (**)
                   alpha_mat(k - 1, i) * tmp_mat(k, j);
          }
        }
        sum = 1. / sum;
        for (size_t i = 0; i < tag_num; ++i) {
          for (size_t j = 0; j < tag_num; ++j) {
            trans_grad[(i + state_trans_base_idx) * tag_num + j] +=
                sum * w_exps[(i + state_trans_base_idx) * tag_num + j] *
                alpha_mat(k - 1, i) * tmp_mat(k, j) * ll_grad;
          }
        }
        trans_grad[(label_value[k - 1] + state_trans_base_idx) * tag_num +
                   label_value[k]] -= static_cast<T>(ll_grad);
      }
    }
  }
};

}  // namespace operators
}  // namespace paddle
