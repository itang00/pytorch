#define TORCH_ASSERT_ONLY_METHOD_OPERATORS
#include <ATen/native/group_norm.h>
#include <ATen/core/Tensor.h>
#include <ATen/Parallel.h>
#include <c10/util/accumulate.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/empty.h>
#include <ATen/ops/empty_like_native.h>
#include <ATen/ops/group_norm_native.h>
#include <ATen/ops/native_batch_norm.h>
#include <ATen/ops/native_group_norm.h>
#include <ATen/ops/native_group_norm_backward_native.h>
#include <ATen/ops/native_group_norm_native.h>
#endif

#include <array>
#include <functional>
#include <tuple>
#include <vector>

namespace at {

namespace native {

template <typename T>
void check_group_norm_inputs(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias,
    T C,
    int64_t num_groups) {
  TORCH_CHECK(
      num_groups > 0,
      "Expected num groups to be greater than 0, got ", num_groups);
  TORCH_CHECK(
      C % num_groups == 0,
      "Expected number of channels in input to be divisible by ",
      "num_groups, but got input of shape ",
      input.sizes(),
      " and "
      "num_groups=",
      num_groups);
  TORCH_CHECK(
      !weight.defined() || (weight.dim() == 1 && at::symint::numel<T>(weight) == C),
      "Expected weight to be a vector of size equal to the number of ",
      "channels in input, but got weight of shape ",
      weight.sizes(),
      " and input of shape ",
      input.sizes());
  TORCH_CHECK(
      !bias.defined() || (bias.dim() == 1 && at::symint::numel<T>(bias) == C),
      "Expected bias to be a vector of size equal to the number of ",
      "channels in input, but got bias of shape ",
      weight.sizes(),
      " and input of shape ",
      input.sizes());
}

std::tuple<Tensor, Tensor, Tensor> native_group_norm(
    const Tensor& X,
    const c10::optional<Tensor>& gamma_opt /* optional */,
    const c10::optional<Tensor>& beta_opt /* optional */,
    int64_t N,
    int64_t C,
    int64_t HxW,
    int64_t group,
    double eps) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> gamma_maybe_owned =
      at::borrow_from_optional_tensor(gamma_opt);
  const Tensor& gamma = *gamma_maybe_owned;
  const Tensor& beta = c10::value_or_else(beta_opt, [] { return Tensor(); });

  // repeated check so expanded weights can call native_group_norm directly but
  // save mean and variance from forward
  check_group_norm_inputs(X, gamma, beta, C, group);
  auto memory_format = X.device().is_cpu() ?
      X.suggest_memory_format() : at::MemoryFormat::Contiguous;

  TORCH_CHECK(X.is_contiguous(memory_format));

  Tensor Y = at::native::empty_like(
      X,
      c10::nullopt /* dtype */,
      c10::nullopt /* layout */,
      c10::nullopt /* device */,
      c10::nullopt /* pin_memory */,
      memory_format);
  Tensor mean = at::empty({N, group}, X.options());
  Tensor rstd = at::empty({N, group}, X.options());
  GroupNormKernel(
      X.device().type(), X, gamma, beta, N, C, HxW, group, eps, Y, mean, rstd);
  return std::make_tuple(Y, mean, rstd);
}

std::tuple<Tensor, Tensor, Tensor> native_group_norm_backward(
    const Tensor& dY,
    const Tensor& X,
    const Tensor& mean,
    const Tensor& rstd,
    const c10::optional<Tensor>& gamma_opt,
    int64_t N,
    int64_t C,
    int64_t HxW,
    int64_t group,
    std::array<bool, 3> grad_input_mask) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> gamma_maybe_owned =
      at::borrow_from_optional_tensor(gamma_opt);
  const Tensor& gamma = *gamma_maybe_owned;

  Tensor dX;
  Tensor dgamma;
  Tensor dbeta;
  if (grad_input_mask[0]) {
    dX = at::native::empty_like(
        X,
        c10::nullopt /* dtype */,
        c10::nullopt /* layout */,
        c10::nullopt /* device */,
        c10::nullopt /* pin_memory */,
        at::MemoryFormat::Contiguous);
  }
  if (grad_input_mask[1]) {
    dgamma = at::native::empty_like(
        gamma,
        c10::nullopt /* dtype */,
        c10::nullopt /* layout */,
        c10::nullopt /* device */,
        c10::nullopt /* pin_memory */,
        at::MemoryFormat::Contiguous);
  }
  if (grad_input_mask[2]) {
    dbeta = at::native::empty_like(
        gamma,
        c10::nullopt /* dtype */,
        c10::nullopt /* layout */,
        c10::nullopt /* device */,
        c10::nullopt /* pin_memory */,
        at::MemoryFormat::Contiguous);
  }
  GroupNormBackwardKernel(
      X.device().type(),
      dY,
      X,
      mean,
      rstd,
      gamma,
      N,
      C,
      HxW,
      group,
      dX,
      dgamma,
      dbeta);
  return std::make_tuple(dX, dgamma, dbeta);
}

Tensor group_norm(
    const Tensor& input,
    int64_t num_groups,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */,
    double eps,
    bool /* cudnn_enabled, deprecated */) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned =
      at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] { return Tensor(); });

  const auto N = input.sym_size(0);
  const auto C = input.sym_size(1);
  check_group_norm_inputs(input, weight, bias, C, num_groups);

  const auto input_shape = input.sym_sizes();
  const auto HxW =
      c10::multiply_integers(input_shape.slice(2));

  const Tensor kEmpty;
  auto memory_format = input.suggest_memory_format();
  const auto& X = input.device().is_cpu() || input.device().is_xpu() ?
      input.contiguous(memory_format) : input.contiguous();
  const auto& gamma = weight.defined() ? weight.contiguous() : kEmpty;
  const auto& beta = bias.defined() ? bias.contiguous() : kEmpty;
  TORCH_CHECK(!gamma.defined() || gamma.sym_numel() == C);
  TORCH_CHECK(!beta.defined() || beta.sym_numel() == C);
  return std::get<0>(
      at::native_group_norm_symint(X, gamma, beta, N, C, HxW, num_groups, eps));
}

DEFINE_DISPATCH(GroupNormKernel);
DEFINE_DISPATCH(GroupNormBackwardKernel);

// Ported from pytorch/xla repo
std::tuple<at::Tensor, at::Tensor, at::Tensor> math_group_norm(
    const Tensor& input,
    const c10::optional<Tensor>& weight_opt,
    const c10::optional<Tensor>& bias_opt,
    int64_t N,
    int64_t C,
    int64_t HxW,
    int64_t group,
    double eps) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned =
      at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] { return Tensor(); });

  auto input_shape = input.sizes();
  at::Tensor input_reshaped = input.view({1, N * group, N ? -1 : 1});
  auto outputs = at::native_batch_norm(
      input_reshaped,
      /*weight=*/{},
      /*bias=*/{},
      /*running_mean=*/{},
      /*running_var=*/{},
      /*training=*/true,
      /*momentum=*/0,
      eps);
  at::Tensor out = std::get<0>(outputs);
  out = out.view(input_shape);
  std::vector<int64_t> affine_param_shape(input.dim(), 1);
  affine_param_shape[1] = C;
  if (weight.defined() && bias.defined()) {
    out = bias.view(affine_param_shape)
              .addcmul(out, weight.view(affine_param_shape), 1);
  } else if (weight.defined()) {
    out = out.mul(weight.view(affine_param_shape));
  } else if (bias.defined()) {
    out = out.add(bias.view(affine_param_shape));
  }
  // convert mean/std to have the same dtype as input.
  // This follows the same behavior as the CPU and CUDA kernels.
  at::Tensor mean = std::get<1>(outputs).to(c10::TensorOptions().dtype(input.scalar_type())).view({N, group});
  at::Tensor rstd = std::get<2>(outputs).to(c10::TensorOptions().dtype(input.scalar_type())).view({N, group});
  return std::make_tuple(out, mean, rstd);
}
} // namespace native
} // namespace at
