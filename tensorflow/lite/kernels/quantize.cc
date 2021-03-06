/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/c_api_internal.h"
#include "tensorflow/lite/kernels/internal/optimized/optimized_ops.h"
#include "tensorflow/lite/kernels/internal/tensor.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/op_macros.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace quantize {

struct OpData {
  int32_t output_multiplier;
  int output_shift;
};

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  auto* data = new OpData;
  return data;
}

void Free(TfLiteContext* context, void* buffer) {
  delete reinterpret_cast<OpData*>(buffer);
}

struct OpContext {
  OpContext(TfLiteContext* context, TfLiteNode* node) {
    input = GetInput(context, node, 0);
    output = GetOutput(context, node, 0);
  }
  const TfLiteTensor* input;
  TfLiteTensor* output;
};

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  OpData* data = reinterpret_cast<OpData*>(node->user_data);
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 1);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  OpContext op_context(context, node);

  TF_LITE_ENSURE(context, op_context.output->type == kTfLiteUInt8 ||
                              op_context.output->type == kTfLiteInt8);

  // TODO(b/128934713): Add support for fixed-point per-channel quantization.
  // Currently this only support affine per-layer quantization.
  TF_LITE_ENSURE_EQ(context, op_context.output->quantization.type,
                    kTfLiteAffineQuantization);
  const auto* affine_quantization = reinterpret_cast<TfLiteAffineQuantization*>(
      op_context.output->quantization.params);
  TF_LITE_ENSURE(context, affine_quantization);
  TF_LITE_ENSURE(context, affine_quantization->scale);
  TF_LITE_ENSURE(context, affine_quantization->scale->size == 1);

  // For requantize use case.
  const bool is_requantize = (op_context.input->type == kTfLiteUInt8 ||
                              op_context.input->type == kTfLiteInt8) &&
                             (op_context.output->type == kTfLiteUInt8 ||
                              op_context.output->type == kTfLiteInt8);
  if (is_requantize) {
    const double effective_output_scale =
        static_cast<double>(op_context.input->params.scale) /
        static_cast<double>(op_context.output->params.scale);
    QuantizeMultiplier(effective_output_scale, &data->output_multiplier,
                       &data->output_shift);
  }

  return context->ResizeTensor(context, op_context.output,
                               TfLiteIntArrayCopy(op_context.input->dims));
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  OpData* data = reinterpret_cast<OpData*>(node->user_data);

  TfLiteTensor* input = &context->tensors[node->inputs->data[0]];
  TfLiteTensor* output = &context->tensors[node->outputs->data[0]];

  switch (input->type) {
    case kTfLiteFloat32: {
      tflite::QuantizationParams op_params;
      op_params.zero_point = output->params.zero_point;
      op_params.scale = output->params.scale;
      if (output->type == kTfLiteInt8) {
        optimized_ops::AffineQuantize(
            op_params, GetTensorShape(input), GetTensorData<float>(input),
            GetTensorShape(output), GetTensorData<int8_t>(output));
      } else if (output->type == kTfLiteUInt8) {
        optimized_ops::AffineQuantize(
            op_params, GetTensorShape(input), GetTensorData<float>(input),
            GetTensorShape(output), GetTensorData<uint8_t>(output));
      } else {
        context->ReportError(
            context,
            "Input type %d with Output type %d is not currently supported.",
            input->type, output->type);
        return kTfLiteError;
      }
    } break;
    case kTfLiteInt8: {
      const int32_t size =
          MatchingFlatSize(GetTensorShape(input), GetTensorShape(output));
      if (output->type == kTfLiteInt8) {
        reference_ops::Requantize<int8_t, int8_t>(
            GetTensorData<int8_t>(input), size, data->output_multiplier,
            data->output_shift, input->params.zero_point,
            output->params.zero_point, GetTensorData<int8_t>(output));
      } else if (output->type == kTfLiteUInt8) {
        reference_ops::Requantize<int8_t, uint8_t>(
            GetTensorData<int8_t>(input), size, data->output_multiplier,
            data->output_shift, input->params.zero_point,
            output->params.zero_point, GetTensorData<uint8_t>(output));
      } else {
        context->ReportError(
            context,
            "Input type %d with Output type %d is not currently supported.",
            input->type, output->type);
        return kTfLiteError;
      }
    } break;
    case kTfLiteUInt8: {
      const int32_t size =
          MatchingFlatSize(GetTensorShape(input), GetTensorShape(output));
      if (output->type == kTfLiteInt8) {
        reference_ops::Requantize<uint8_t, int8_t>(
            GetTensorData<uint8_t>(input), size, data->output_multiplier,
            data->output_shift, input->params.zero_point,
            output->params.zero_point, GetTensorData<int8_t>(output));
      } else if (output->type == kTfLiteUInt8) {
        reference_ops::Requantize<uint8_t, uint8_t>(
            GetTensorData<uint8_t>(input), size, data->output_multiplier,
            data->output_shift, input->params.zero_point,
            output->params.zero_point, GetTensorData<uint8_t>(output));
      } else {
        context->ReportError(
            context,
            "Input type %d with Output type %d is not currently supported.",
            input->type, output->type);
        return kTfLiteError;
      }
    } break;
    default:
      context->ReportError(
          context,
          "Input type %d with Output type %d is not currently supported.",
          input->type, output->type);
      return kTfLiteError;
  }

  return kTfLiteOk;
}

}  // namespace quantize

TfLiteRegistration* Register_QUANTIZE_OPT() {
  static TfLiteRegistration r = {quantize::Init, quantize::Free,
                                 quantize::Prepare, quantize::Eval};
  return &r;
}

TfLiteRegistration* Register_QUANTIZE() { return Register_QUANTIZE_OPT(); }

}  // namespace builtin
}  // namespace ops
}  // namespace tflite
