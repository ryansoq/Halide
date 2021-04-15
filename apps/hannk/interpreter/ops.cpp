#include "interpreter/ops.h"
#include "util/error_util.h"

#include <atomic>
#include <cmath>
#include <iostream>

#include "add_uint8_uint8.h"
#include "average_pool_uint8.h"
#include "conv_uint8.h"
#ifdef CONV_R16
#include "conv_r16_uint8.h"
#endif
#include "copy_uint8_uint8.h"
#include "depthwise_conv_broadcast_uint8.h"
#include "depthwise_conv_dm1_uint8.h"
#include "depthwise_conv_uint8.h"
#include "fill_uint8.h"
#include "fully_connected_uint8.h"
#include "l2_normalization_uint8.h"
#include "logistic_uint8.h"
#include "max_pool_uint8.h"
#include "mean_uint8.h"
#include "mul_uint8_uint8_uint8.h"
#include "softmax_uint8.h"
#include "tanh_uint8.h"
#include "tile_conv_filter_uint8.h"

namespace hannk {

namespace {

// Check if dimension 0 and dimension 1 of buf can be fused.
template<typename T>
bool can_fuse(const HalideBuffer<T> &buf, int d0, int d1) {
    assert(d0 != d1);
    return d0 < buf.dimensions() &&
           d1 < buf.dimensions() &&
           buf.dim(d0).min() == 0 &&
           buf.dim(d1).stride() > 0 &&
           buf.dim(d1).stride() == buf.dim(d0).extent() * buf.dim(d0).stride();
}
template<typename T>
bool can_fuse_cx(const HalideBuffer<T> &buf) {
    return can_fuse(buf, 0, 1);
}
template<typename T>
bool can_fuse_xy(const HalideBuffer<T> &buf) {
    return can_fuse(buf, 1, 2);
}

// Fuse the first two dimensions of buf. d1 is deleted from the buffer.
template<typename T>
void fuse(HalideBuffer<T> &buf, int d0, int d1) {
    halide_dimension_t &dim0 = buf.raw_buffer()->dim[d0];
    halide_dimension_t &dim1 = buf.raw_buffer()->dim[d1];
    dim0.extent *= dim1.extent;
    for (int d = d1; d + 1 < buf.dimensions(); d++) {
        buf.raw_buffer()->dim[d] = buf.raw_buffer()->dim[d + 1];
    }
    buf.slice(buf.dimensions() - 1);
}
template<typename T>
void fuse_cx(HalideBuffer<T> &buf) {
    fuse(buf, 0, 1);
}
template<typename T>
void fuse_xy(HalideBuffer<T> &buf) {
    fuse(buf, 1, 2);
}

// Embed extent 1 dimensions until buf has the given rank.
template<typename T>
void pad_to_rank(HalideBuffer<T> &buf, int rank) {
    while (buf.dimensions() < rank) {
        buf.embed(buf.dimensions(), 0);
    }
}

template<typename Ta, typename Tb, typename Tc>
void optimize_elementwise_shapes(HalideBuffer<Ta> &a, HalideBuffer<Tb> &b, HalideBuffer<Tc> &c, int rank) {
    while (can_fuse_cx(a) && can_fuse_cx(b) && can_fuse_cx(c) &&
           a.dim(0).extent() == c.dim(0).extent() &&
           b.dim(0).extent() == c.dim(0).extent()) {
        fuse_cx(a);
        fuse_cx(b);
        fuse_cx(c);
    }
    pad_to_rank(a, rank);
    pad_to_rank(b, rank);
    pad_to_rank(c, rank);
}

template<typename Ta, typename Tb>
void optimize_elementwise_shapes(HalideBuffer<Ta> &a, HalideBuffer<Tb> &b, int rank) {
    while (can_fuse_cx(a) && can_fuse_cx(b) &&
           a.dim(0).extent() == b.dim(0).extent()) {
        fuse_cx(a);
        fuse_cx(b);
    }
    pad_to_rank(a, rank);
    pad_to_rank(b, rank);
}

template<typename Ta, typename Tb>
void broadcast_shapes(HalideBuffer<Ta> &a, HalideBuffer<Tb> &b, int rank) {
    pad_to_rank(a, rank);
    pad_to_rank(b, rank);

    halide_buffer_t *raw_a = a.raw_buffer();
    halide_buffer_t *raw_b = b.raw_buffer();
    for (int d = 0; d < rank; d++) {
        if (raw_a->dim[d].extent == 1) {
            raw_a->dim[d].extent = raw_b->dim[d].extent;
            raw_a->dim[d].stride = 0;
        } else if (b.dim(d).extent() == 1) {
            raw_b->dim[d].extent = raw_a->dim[d].extent;
            raw_b->dim[d].stride = 0;
        } else {
            LOG(FATAL) << "Can't broadcast shapes";
        }
    }
}

bool is_alias(const HalideBuffer<const void> &a, const HalideBuffer<const void> &b) {
    return !(a.begin() >= b.end() || a.end() <= b.begin());
}

template<typename T, typename U>
void crop_to_union(HalideBuffer<T> &a, HalideBuffer<U> &b) {
    assert(a.dimensions() == b.dimensions());
    for (int d = 0; d < a.dimensions(); d++) {
        int min = std::max(a.dim(d).min(), b.dim(d).min());
        int max = std::min(a.dim(d).max(), b.dim(d).max());
        a.crop(d, min, max - min + 1);
        b.crop(d, min, max - min + 1);
    }
}

struct QuantizedMulAndShift {
    int multiplier, shift;
};

QuantizedMulAndShift get_quantized_mul_and_shift(double double_multiplier, int bits = 32) {
    if (double_multiplier == 0.) {
        return {0, 0};
    }

    int shift = 0;
    const double q = std::frexp(double_multiplier, &shift);
    int64_t q_fixed = (int64_t)std::round(q * (1LL << (bits - 1)));
    assert(q_fixed <= (1LL << (bits - 1)));

    if (q_fixed == (1LL << (bits - 1))) {
        q_fixed /= 2;
        ++shift;
    }
    assert(q_fixed <= std::numeric_limits<int32_t>::max());

    if (shift < -(bits - 1)) {
        shift = 0;
        q_fixed = 0;
    }

    return {(int)q_fixed, shift};
}

QuantizedMulAndShift get_quantized_mul_and_shift_smaller_than_one(double double_multiplier, int bits = 32) {
    assert(double_multiplier >= 0.0 && double_multiplier < 1.0);
    auto result = get_quantized_mul_and_shift(double_multiplier, bits);
    assert(result.shift <= 0);
    return result;
}

Interval get_quantized_min_max(ActivationFunction activation, int zero_point, double scale) {
    int min = 0;
    int max = 255;
    if (activation == ActivationFunction::None) {
        // nothing
    } else if (activation == ActivationFunction::Relu) {
        min = zero_point;
    } else if (activation == ActivationFunction::Relu6) {
        min = zero_point;
        max = zero_point + (int)std::round(6.0 / scale);
    } else if (activation == ActivationFunction::ReluN1To1) {
        min = zero_point + (int)std::round(-1.0 / scale);
        max = zero_point + (int)std::round(1.0 / scale);
    } else {
        CHECK(false) << "Unsupported quantized activation function type.";
    }
    return {std::max(min, 0), std::min(max, 255)};
}

Interval get_output_range(ActivationFunction activation, const QuantizationInfo &quantization) {
    const int output_zero = quantization.zero.at(0);
    assert(output_zero >= 0 && output_zero <= 255);

    const float output_scale = quantization.scale.at(0);

    const auto output_range = get_quantized_min_max(activation, output_zero, output_scale);
    assert(output_range.min >= 0 && output_range.min <= 255);
    assert(output_range.max >= 0 && output_range.max <= 255);
    assert(output_range.min <= output_range.max);

    return output_range;
}

struct MultiplyParams {
    int a_zero;
    int b_zero;
    int c_zero;
    QuantizedMulAndShift c;
};

MultiplyParams get_quantized_multiply_params(const QuantizationInfo &a, const QuantizationInfo &b, const QuantizationInfo &c) {
    MultiplyParams result;
    result.a_zero = a.zero.at(0);
    result.b_zero = b.zero.at(0);
    result.c_zero = c.zero.at(0);

    const float a_scale = a.scale.at(0);
    const float b_scale = b.scale.at(0);
    const float c_scale = c.scale.at(0);
    const double ab_scale = a_scale * b_scale;
    result.c = get_quantized_mul_and_shift_smaller_than_one(ab_scale / c_scale);
    result.c.shift = -result.c.shift;

    return result;
}

void add(HalideBuffer<const uint8_t> in1, const QuantizationInfo &in1q,
         HalideBuffer<const uint8_t> in2, const QuantizationInfo &in2q, int in2sign,
         HalideBuffer<uint8_t> out, const QuantizationInfo &outq, ActivationFunction activation) {
    const int in1_zero = in1q.zero.at(0);
    const int in2_zero = in2q.zero.at(0);
    const int out_zero = outq.zero.at(0);

    const float in1_scale = in1q.scale.at(0);
    const float in2_scale = in2q.scale.at(0);
    const float out_scale = outq.scale.at(0);

    const int left_shift = 20;  // 20 for 8-bit, 15 for 16-bit
    const double twice_max_input_scale = 2 * std::max(in1_scale, in2_scale);
    const double real_in1_multiplier = in1_scale / twice_max_input_scale;
    const double real_in2_multiplier = in2_scale / twice_max_input_scale;
    const double real_out_multiplier = twice_max_input_scale / ((1 << left_shift) * out_scale);

    auto in1_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(real_in1_multiplier);
    auto in2_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(real_in2_multiplier);
    auto out_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(real_out_multiplier);
    assert(in1_mul_and_shift.shift <= 0);
    assert(in2_mul_and_shift.shift <= 0);
    assert(out_mul_and_shift.shift <= 0);

    in2_mul_and_shift.multiplier *= in2sign;

    const auto out_range = get_output_range(activation, outq);

    CHECK(0 == add_uint8_uint8(left_shift, in1, in2,
                               in1_zero, in1_mul_and_shift.multiplier, -in1_mul_and_shift.shift,
                               in2_zero, in2_mul_and_shift.multiplier, -in2_mul_and_shift.shift,
                               out_zero, out_mul_and_shift.multiplier, -out_mul_and_shift.shift,
                               out_range.min, out_range.max, out));
}

void mul(HalideBuffer<const uint8_t> in1, const QuantizationInfo &in1q,
         HalideBuffer<const uint8_t> in2, const QuantizationInfo &in2q,
         HalideBuffer<uint8_t> out, const QuantizationInfo &outq, ActivationFunction activation) {
    const int in1_zero = in1q.zero.at(0);
    const int in2_zero = in2q.zero.at(0);
    const int out_zero = outq.zero.at(0);

    const float in1_scale = in1q.scale.at(0);
    const float in2_scale = in2q.scale.at(0);
    const float out_scale = outq.scale.at(0);

    const double multiplier = in1_scale * in2_scale / out_scale;

    auto mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(multiplier);
    assert(mul_and_shift.shift <= 0);

    const auto out_range = get_output_range(activation, outq);

    CHECK(0 == mul_uint8_uint8_uint8(in1, in2, in1_zero, in2_zero,
                                     out_zero, mul_and_shift.multiplier, -mul_and_shift.shift,
                                     out_range.min, out_range.max, out));
}

void requantize(const HalideBuffer<const uint8_t> &in, const QuantizationInfo &inq,
                HalideBuffer<uint8_t> out, const QuantizationInfo &outq) {
    if (inq == outq) {
        // Some of these are just copies, or no-ops.
        if (is_alias(in, out)) {
            return;
        } else {
            out.copy_from(in);
        }
    } else {
        // TODO: Maybe a dedicated pipeline for this would be better. It
        // could be a little faster, and avoid some quantization error.
        add(in, inq, in, inq, 0, out, outq, ActivationFunction::None);
    }
}

}  // namespace

BoundsMap ElementwiseOp::map_bounds(int input_idx, int output_idx) const {
    int rank = output()->rank();
    assert(rank == input(input_idx)->rank());
    return BoundsMap::elementwise(rank);
}

const char *BinaryOp::to_string(BinaryOp::Operator op) {
    switch (op) {
    case Add:
        return "Add";
    case Sub:
        return "Sub";
    case Mul:
        return "Mul";
    default:
        CHECK(false) << "Unsupported binary op\n";
        return nullptr;
    }
}

void BinaryOp::execute() {
    const TensorPtr in1 = input(0);
    const TensorPtr in2 = input(1);
    TensorPtr out = output();

    if (in1->type() == halide_type_of<uint8_t>() &&
        in2->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto in1_buf = in1->buffer<const uint8_t>();
        auto in2_buf = in2->buffer<const uint8_t>();
        auto out_buf = out->buffer<uint8_t>();
        // TODO: We should require the buffers are already broadcasted appropriately before
        // getting here.
        broadcast_shapes(in1_buf, in2_buf, 4);
        optimize_elementwise_shapes(in1_buf, in2_buf, out_buf, 4);
        switch (op_) {
        case Add:
        case Sub:
            add(in1_buf, in1->quantization(), in2_buf, in2->quantization(), op_ == Add ? 1 : -1, out_buf, out->quantization(), activation_);
            break;
        case Mul:
            mul(in1_buf, in1->quantization(), in2_buf, in2->quantization(), out_buf, out->quantization(), activation_);
            break;
        }
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

BoundsMap ConcatenationOp::map_bounds(int input_idx, int output_idx) const {
    int rank = output()->rank();
    assert(rank == input(input_idx)->rank());

    int offset = 0;
    for (int i = 0; i < input_idx; i++) {
        offset += input(i)->extent(axis_);
    }
    BoundsMap result = BoundsMap::elementwise(rank);
    result.at(axis_, axis_).bounds += offset;
    return result;
}

void ConcatenationOp::execute() {
    HalideBuffer<void> output_buf = output()->buffer();

    int concatenated_i = 0;
    for (int i = 0; i < input_count(); i++) {
        HalideBuffer<const void> input_buf = input(i)->buffer();
        assert(input_buf.dim(axis_).min() == 0);
        input_buf.translate(axis_, concatenated_i);
        concatenated_i += input_buf.dim(axis_).extent();

        HalideBuffer<void> output_crop = output_buf;
        crop_to_union(output_crop, input_buf);
        requantize(input_buf, input(i)->quantization(), output_crop, output()->quantization());
    }
}

halide_type_t Conv2DOp::filter_type() const {
    if (input()->type() == halide_type_of<uint8_t>() &&
        output()->type() == halide_type_of<uint8_t>()) {
        const halide_filter_metadata_t *metadata = conv_uint8_metadata();
        return metadata->arguments[1].type;
    } else {
        CHECK(false) << "Unsupported type " << output()->type() << "\n";
    }
}

BoundsMap Conv2DOp::map_bounds(int input_idx, int output_idx) const {
#ifdef CONV_R16
    const int unroll_reduction = filter()->extent(0) >= 16 ? 16 : 4;
#else
    const int unroll_reduction = 4;
#endif
    if (input_idx == 0) {
        return BoundsMap(4, output()->rank())
            .constant(0, align_up(input()->extent(0), unroll_reduction))
            .downsample(1, 1, stride_[0], Interval(0, dilation_[0] * (filter()->extent(1) - 1)))
            .downsample(2, 2, stride_[1], Interval(0, dilation_[1] * (filter()->extent(2) - 1)))
            .elementwise(3, 3);
    } else if (input_idx == 1) {
        // Pass minimal sized buffers to learn about the alignment requirements.
        HalideBuffer<uint8_t> input_buf(nullptr, 1, 1, 1, 1);
        HalideBuffer<int32_t> bias_buf(nullptr, 1);
        HalideBuffer<void> filter_buf(filter_type(), 1, 1, 1, 1, 1, 1);
        // TODO: How to initialize the above buffer without allocating?
        filter_buf.deallocate();
        HalideBuffer<uint8_t> output_buf;
        CHECK(0 == conv_uint8(input_buf, filter_buf, bias_buf, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, output_buf));

        const int vector_reduction = filter_buf.dim(0).extent();
        const int vector_tile = filter_buf.dim(1).extent();
        const int channel_alignment = unroll_reduction / vector_reduction;
        return BoundsMap(6, 4)
            .constant(0, vector_reduction)
            .constant(1, vector_tile)
            .constant(2, align_up(ceil_div(filter()->extent(0), vector_reduction), channel_alignment))
            .upsample(3, 0, vector_tile)
            .constant(4, filter()->bounds(1))
            .constant(5, filter()->bounds(2));
    } else {
        assert(input_idx == 2);
        return BoundsMap(1, 4).elementwise(0, 0);
    }
}

namespace {

void conv_uint8(halide_buffer_t *input, halide_buffer_t *filter, halide_buffer_t *bias,
                const MultiplyParams &params, const std::vector<int> &stride,
                const std::vector<int> &dilation, const Interval &output_range,
                halide_buffer_t *output) {
#ifdef CONV_R16
    if (input->dim[0].extent >= 16) {
        // For large reductions, use the big reduction version.
        // TODO: We really ought to be able to do this with GuardWithIf
        // and/or specialize.
        CHECK(
            0 == conv_r16_uint8(
                     input, filter, bias, (uint8_t)params.a_zero,
                     (uint8_t)params.b_zero, stride[0], stride[1],
                     dilation[0], dilation[1], params.c.multiplier,
                     params.c.shift, (uint8_t)params.c_zero,
                     output_range.min, output_range.max, output));
    } else
#endif
    {
        CHECK(
            0 == ::hannk::conv_uint8(
                     input, filter, bias, (uint8_t)params.a_zero,
                     (uint8_t)params.b_zero, stride[0], stride[1],
                     dilation[0], dilation[1], params.c.multiplier,
                     params.c.shift, (uint8_t)params.c_zero,
                     output_range.min, output_range.max, output));
    }
}

}  // namespace

void Conv2DOp::execute() {
    const TensorPtr in = input();
    const TensorPtr filt = filter();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto input_buf = in->buffer<const uint8_t>();
        auto filter_buf = filt->buffer<const void>();
        auto bias_buf = bias()->buffer<const int32_t>();
        auto output_buf = out->buffer<uint8_t>();

        MultiplyParams params =
            get_quantized_multiply_params(in->quantization(), filt->quantization(), out->quantization());

        const auto output_range = get_output_range(activation_, out->quantization());

        assert(filter_buf.dimensions() == 6);
        const int filter_width = filter_buf.dim(4).extent();
        const int filter_height = filter_buf.dim(5).extent();
        if (filter_width == 1 && filter_height == 1) {
            // For 1x1 filters, we can fuse x and y, which can help avoid overhead for
            // small output sizes.
            while (can_fuse_xy(input_buf) && can_fuse_xy(output_buf) &&
                   input_buf.dim(1).extent() == output_buf.dim(1).extent()) {
                fuse_xy(input_buf);
                fuse_xy(output_buf);
            }
            pad_to_rank(input_buf, 4);
            pad_to_rank(output_buf, 4);
        }

        conv_uint8(input_buf, filter_buf, bias_buf, params, stride_, dilation_, output_range, output_buf);

    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

namespace {

// Wrapper to dispatch to the appropriate variant of depthwise_conv.
void depthwise_conv_uint8(
    halide_buffer_t *input, halide_buffer_t *filter, halide_buffer_t *bias,
    int depth_multiplier, const MultiplyParams &params, const std::vector<int> &stride, const std::vector<int> &dilation,
    const Interval &output_range, halide_buffer_t *output) {
    if (depth_multiplier >= output->dim[0].extent) {
        CHECK(
            0 == depthwise_conv_broadcast_uint8(
                     input, filter, bias, depth_multiplier,
                     (uint8_t)params.a_zero, (uint8_t)params.b_zero, stride[0], stride[1],
                     dilation[0], dilation[1], params.c.multiplier, params.c.shift,
                     (uint8_t)params.c_zero, (uint8_t)output_range.min, (uint8_t)output_range.max, output));
    } else if (depth_multiplier == 1) {
        CHECK(
            0 == depthwise_conv_dm1_uint8(
                     input, filter, bias, depth_multiplier,
                     (uint8_t)params.a_zero, (uint8_t)params.b_zero, stride[0], stride[1],
                     dilation[0], dilation[1], params.c.multiplier, params.c.shift,
                     (uint8_t)params.c_zero, (uint8_t)output_range.min, (uint8_t)output_range.max, output));
    } else {
        CHECK(
            0 == ::hannk::depthwise_conv_uint8(
                     input, filter, bias, depth_multiplier,
                     (uint8_t)params.a_zero, (uint8_t)params.b_zero, stride[0], stride[1],
                     dilation[0], dilation[1], params.c.multiplier, params.c.shift,
                     (uint8_t)params.c_zero, (uint8_t)output_range.min, (uint8_t)output_range.max, output));
    }
}

}  // namespace

BoundsMap DepthwiseConv2DOp::map_bounds(int input_idx, int output_idx) const {
    assert(output_idx == 0);
    if (input_idx == 0) {
        BoundsMap result(4, 4);
        result
            .upsample(0, 0, depth_multiplier_)
            .downsample(1, 1, stride_[0], Interval(0, dilation_[0] * (filter()->extent(1) - 1)))
            .downsample(2, 2, stride_[1], Interval(0, dilation_[1] * (filter()->extent(2) - 1)))
            .elementwise(3, 3);
        if (depth_multiplier_ == 1) {
            // TODO: Handle this padding for SIMD width elsewhere. Either fix depthwise
            // so it doesn't need this, or pass alignment information somewhere else.
#if defined(__arm__) || defined(__aarch64__)
            result.align(0, 16);
#else
            result.align(0, 32);
#endif
        }
        return result;
    } else if (input_idx == 1) {
        return BoundsMap(3, 4)
            .elementwise(0, 0)
            .constant(1, filter()->bounds(1))
            .constant(2, filter()->bounds(2));
    } else if (input_idx == 2) {
        return BoundsMap(1, 4).elementwise(0, 0);
    } else {
        return BoundsMap(0, 4);
    }
}

void DepthwiseConv2DOp::execute() {
    const TensorPtr in = input();
    const TensorPtr filt = filter();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        filt->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto input_buf = in->buffer<const uint8_t>();
        auto filter_buf = filt->buffer<const uint8_t>().sliced(3, 0);
        auto bias_buf = bias()->buffer<const int32_t>();
        auto output_buf = out->buffer<uint8_t>();

        assert(depth_multiplier_ * input_buf.dim(0).extent() == output_buf.dim(0).extent());

        MultiplyParams params =
            get_quantized_multiply_params(in->quantization(), filt->quantization(), out->quantization());

        const auto output_range = get_output_range(activation_, out->quantization());

        depthwise_conv_uint8(input_buf, filter_buf, bias_buf, depth_multiplier_, params,
                             stride_, dilation_, output_range, output_buf);
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

BoundsMap FullyConnectedOp::map_bounds(int input_idx, int output_idx) const {
    assert(output_idx == 0);
    if (input_idx == 0) {
        return BoundsMap(2, 2).constant(0, input()->extent(0)).elementwise(1, 1);
    } else if (input_idx == 1) {
        return BoundsMap(2, 2).constant(0, filter()->extent(0)).elementwise(1, 0);
    } else if (input_idx == 2) {
        return BoundsMap(1, 2).elementwise(0, 0);
    } else {
        return BoundsMap(0, 2);
    }
}

void FullyConnectedOp::execute() {
    const TensorPtr in = input();
    const TensorPtr filt = filter();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        filt->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto input_buf = in->buffer<const uint8_t>();
        auto filter_buf = filt->buffer<const uint8_t>();
        auto bias_buf = bias()->buffer<const int32_t>();
        auto output_buf = out->buffer<uint8_t>();

        // TODO: This should be handled explicitly with a reshape.
        // It's annoying tflite doesn't require this. This means
        // that we can't arbitrarily insert padding of the strides
        // for tensors consumed by this op.
        while (input_buf.dimensions() > 2) {
            CHECK(can_fuse_cx(input_buf)) << "Unfusable fully connected input\n";
            fuse_cx(input_buf);
        }

        MultiplyParams params =
            get_quantized_multiply_params(in->quantization(), filt->quantization(), out->quantization());

        const auto output_range = get_output_range(activation_, out->quantization());

        CHECK(
            0 == fully_connected_uint8(
                     input_buf, filter_buf, bias_buf, (uint8_t)params.a_zero, (uint8_t)params.b_zero,
                     (uint8_t)params.c_zero, params.c.multiplier, params.c.shift, (uint8_t)output_range.min,
                     (uint8_t)output_range.max, output_buf));
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

BoundsMap L2NormalizationOp::map_bounds(int input_idx, int output_idx) const {
    assert(input_idx == 0);
    assert(output_idx == 0);
    return BoundsMap(2, 2)
        .constant(0, input()->bounds(0))
        .elementwise(1, 1);
}

void L2NormalizationOp::execute() {
    const TensorPtr in = input();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto in_buf = in->buffer<const uint8_t>();
        auto output_buf = out->buffer<uint8_t>();

        const int input_zero = in->quantization().zero.at(0);
        assert(input_zero >= 0 && input_zero <= 255);

        assert(out->quantization().scale.at(0) == 1.0f / 128.0f);
        assert(out->quantization().zero.at(0) == 128);

        CHECK(0 == l2_normalization_uint8(in_buf, input_zero, output_buf));
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

BoundsMap PadOp::map_bounds(int input_idx, int output_idx) const {
    assert(output_idx == 0);
    const int rank = output()->rank();
    if (input_idx == 0) {
        if (input(1)) {
            BoundsMap result(rank, rank);
            auto padding = input(1)->buffer<const int32_t>();
            for (int d = 0; d < output()->rank(); d++) {
                result.elementwise(d, d, padding(0, d));
            }
            return result;
        } else {
            return BoundsMap::elementwise(rank);
        }
    } else {
        assert(input_idx == 1);
        return BoundsMap(1, rank).constant(0, rank);
    }
}

void PadOp::execute() {
    const TensorPtr in = input(0);
    TensorPtr out = output();

    if (out->type().bytes() == 1) {
        auto input_buf = in->buffer<const uint8_t>();
        auto output_buf = out->buffer<uint8_t>();

        if (input(1)) {
            auto padding = input(1)->buffer<const int32_t>();
            for (int d = 0; d < output_buf.dimensions(); d++) {
                input_buf.translate(d, padding(0, d));
            }
        }

        uint8_t pad_value = in->quantization().zero.at(0);

        int fill_min_dim = 0;
        if (input_buf.dim(0).extent() == 3 && output_buf.dim(0).extent() == 4) {
            // copy can handle padding dimension 0, which is much faster than
            // filling the extra channel for interleaved 3/4 channel paddings.
            fill_min_dim = 1;
        }
        for (int d = output_buf.dimensions() - 1; d >= fill_min_dim; d--) {
            int input_min = input_buf.dim(d).min();
            int output_min = output_buf.dim(d).min();
            int input_max = input_buf.dim(d).max();
            int output_max = output_buf.dim(d).max();
            if (output_min < input_min) {
                auto before = output_buf.cropped(d, output_min, input_min - output_min);
                CHECK(0 == fill_uint8(pad_value, before));
            } else {
                input_min = output_min;
            }
            if (output_max > input_max) {
                auto after = output_buf.cropped(d, input_max + 1, output_max - input_max);
                CHECK(0 == fill_uint8(pad_value, after));
            } else {
                input_max = output_max;
            }
            output_buf.crop(d, input_min, input_max - input_min + 1);
        }
        if (!is_alias(input_buf, output_buf) ||
            input_buf.dim(0).min() > output_buf.dim(0).min() ||
            input_buf.dim(0).max() < output_buf.dim(0).max()) {
            CHECK(0 == copy_uint8_uint8(input_buf, pad_value, output_buf));
        }
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

namespace {

int compute_padding(int stride, int in_size, int filter_size, int out_size) {
    const int effective_filter_size = (filter_size - 1) + 1;
    const int total_padding = std::max(0, ((out_size - 1) * stride + effective_filter_size - in_size));
    return total_padding / 2;
}

}  // namespace

const char *PoolOp::to_string(PoolOp::Operator op) {
    switch (op) {
    case Average:
        return "Average";
    case Max:
        return "Max";
    default:
        CHECK(false) << "Unsupported pool op\n";
        return nullptr;
    }
}

BoundsMap PoolOp::map_bounds(int input_idx, int output_idx) const {
    assert(output_idx == 0);
    return BoundsMap(4, 4)
        .elementwise(0, 0)
        .downsample(1, 1, stride_[0], Interval(0, filter_size_[0] - 1))
        .downsample(2, 2, stride_[1], Interval(0, filter_size_[1] - 1))
        .elementwise(3, 3);
}

void PoolOp::execute() {
    const TensorPtr in = input();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto input_buf = in->buffer<const uint8_t>();
        auto output_buf = out->buffer<uint8_t>();

        const auto output_range = get_output_range(activation_, out->quantization());

        const int in_width = input_buf.dim(1).extent();
        const int in_height = input_buf.dim(2).extent();
        const int out_width = output_buf.dim(1).extent();
        const int out_height = output_buf.dim(2).extent();
        input_buf.translate(1, compute_padding(stride_[0], in_width, filter_size_[0], out_width));
        input_buf.translate(2, compute_padding(stride_[1], in_height, filter_size_[1], out_height));

        switch (op_) {
        case Average:
            CHECK(
                0 == average_pool_uint8(input_buf, stride_[0], stride_[1],
                                        filter_size_[0], filter_size_[1],
                                        output_range.min, output_range.max, output_buf));
            break;
        case Max:
            CHECK(
                0 == max_pool_uint8(input_buf, stride_[0], stride_[1],
                                    filter_size_[0], filter_size_[1],
                                    output_range.min, output_range.max, output_buf));
            break;
        }
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

const char *ReductionOp::to_string(Operator op) {
    switch (op) {
    case Mean:
        return "Mean";
    default:
        CHECK(false) << "Unsupported reduction operator.\n";
        return nullptr;
    }
}

bool ReductionOp::reducing(int d) const {
    auto indices = input(1)->buffer<const int32_t>();
    for (int i = 0; i < indices.dim(0).extent(); i++) {
        if (indices(i) == d) {
            return true;
        }
    }
    return false;
}

BoundsMap ReductionOp::map_bounds(int input_idx, int output_idx) const {
    assert(output_idx == 0);

    if (input_idx == 0) {
        int output_d = 0;
        BoundsMap result(input()->rank(), output()->rank());
        for (int d = 0; d < input()->rank(); d++) {
            if (reducing(d)) {
                result.constant(d, input()->bounds(d));
            } else {
                result.elementwise(d, output_d++);
            }
        }
        assert(output_d == output()->rank());
        return result;
    } else {
        return BoundsMap(1, output()->rank()).all(input(1)->bounds(), output()->rank());
    }
}

void ReductionOp::execute() {
    auto indices = input(1)->buffer<const int32_t>();

    const TensorPtr in = input();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto input_buf = in->buffer<const uint8_t>();
        auto output_buf = out->buffer<uint8_t>();

        if (op_ == Mean) {
            int mins[4] = { 0, 0, 0, 0 };
            int extents[4] = { 1, 1, 1, 1 };
            for (int d = 0; d < 4; d++) {
                if (reducing(d)) {
                    mins[d] = input_buf.dim(d).min();
                    extents[d] = input_buf.dim(d).extent();
                }
            }
            CHECK(0 == mean_uint8(input_buf, mins[0], extents[0], mins[1], extents[1],
                                  mins[2], extents[2], mins[3], extents[3], output_buf));
        }
    }

}

// TODO: Maybe this is only a reshape in some dimensions, in which case we might be able to split it.
BoundsMap ReshapeOp::map_bounds(int input_idx, int output_idx) const {
    assert(input_idx == 0);
    assert(output_idx == 0);
    return BoundsMap::all(input()->bounds(), output()->rank());
}

void ReshapeOp::execute() {
    const TensorPtr in = input();
    TensorPtr out = output();

    auto input_buf = in->buffer<const void>();
    auto output_buf = out->buffer();

    // TODO: should reality-check that the output buf matches the shape we expect
    // assert((int) new_shape_.size() == output_buf.dimensions());
    // for (int d = 0; d < output_buf.dimensions(); d++) {
    //     assert(new_shape_.at(d) == output_buf.dim(d).extent());
    // }

    assert(input_buf.number_of_elements() == output_buf.number_of_elements());
    size_t output_size = output_buf.number_of_elements() * out->type().bytes();
    if (is_alias(input_buf, output_buf)) {
        assert(input_buf.begin() == output_buf.begin());
        assert(input_buf.end() == output_buf.end());
    } else {
        // TODO: This should also check the strides are dense.
        memcpy(output_buf.data(), input_buf.data(), output_size);
    }
}

BoundsMap SoftmaxOp::map_bounds(int input_idx, int output_idx) const {
    assert(input_idx == 0);
    assert(output_idx == 0);
    return BoundsMap(2, 2)
        .constant(0, input()->bounds(0))
        .elementwise(1, 1);
}

void SoftmaxOp::execute() {
    const TensorPtr in = input();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() &&
        out->type() == halide_type_of<uint8_t>()) {
        auto in_buf = in->buffer<const uint8_t>();
        auto output_buf = out->buffer<uint8_t>();

        // It's a easier to compute 2^(x*(B*log2(e))) than e^(x*B).
        const float beta2 = beta_ * std::log2(std::exp(1.0f));

        // We don't need the input zero point because this op exploits the
        // identity exp(x_i)/sum(exp(x_i)) == exp(x_i + C)/sum(exp(x_i + C))
        const int output_zero = out->quantization().zero.at(0);
        assert(output_zero >= 0 && output_zero <= 255);

        const float in_scale = in->quantization().scale.at(0);
        const float output_scale = out->quantization().scale.at(0);

        const int left_shift = 6;
        const double real_in_multiplier = in_scale * beta2 / (1 << left_shift);

        auto in_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(real_in_multiplier, 16);
        auto output_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(output_scale);
        assert(in_mul_and_shift.shift <= 0);
        assert(output_mul_and_shift.shift <= 0);

        CHECK(0 == softmax_uint8(in_buf, in_mul_and_shift.multiplier, -in_mul_and_shift.shift,
                                 output_zero, output_mul_and_shift.multiplier, -output_mul_and_shift.shift,
                                 output_buf));
    } else {
        CHECK(false) << "Unsupported type " << out->type() << "\n";
    }
}

BoundsMap TileConvFilterOp::map_bounds(int input_idx, int output_idx) const {
    assert(input_idx == 0);
    assert(output_idx == 0);
    // TODO: Maybe we could say more here, but it usually doesn't
    // matter because this op usually gets constant folded.
    return BoundsMap::all(input()->bounds(), output()->rank());
}

void TileConvFilterOp::execute() {
    const TensorPtr in = input();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>()) {
        auto input_buf = in->buffer<const uint8_t>();
        auto output_buf = out->buffer<void>();

        int input_zero = in->quantization().zero.at(0);
        int output_zero = out->quantization().zero.at(0);

        CHECK(0 == tile_conv_filter_uint8(input_buf, input_zero, output_zero, output_buf));
    } else {
        CHECK(false) << "Unsupported type " << in->type() << "\n";
    }
}

const char *UnaryOp::to_string(UnaryOp::Operator op) {
    switch (op) {
    case Logistic:
        return "Logistic";
    case Tanh:
        return "Tanh";
    default:
        CHECK(false) << "Unsupported unary op\n";
        return nullptr;
    }
}

void UnaryOp::execute() {
    const TensorPtr in = input();
    TensorPtr out = output();

    if (in->type() == halide_type_of<uint8_t>() && out->type() == halide_type_of<uint8_t>()) {
        auto in_buf = in->buffer<const uint8_t>();
        auto out_buf = out->buffer<uint8_t>();
        optimize_elementwise_shapes(in_buf, out_buf, 1);

        const int input_zero = in->quantization().zero.at(0);
        assert(input_zero >= 0 && input_zero <= 255);
        const float in_scale = in->quantization().scale.at(0);

        const int left_shift = 6;

        if (op_ == Logistic) {
            // It's a easier to compute 2^(x*(log2(e))) than e^(x).
            const double real_in_multiplier = in_scale * -std::log2(std::exp(1.0f)) / (1 << left_shift);

            auto in_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(real_in_multiplier, 16);
            assert(in_mul_and_shift.shift <= 0);

            assert(out->quantization().scale.at(0) == 1.0f / 256.0f);
            assert(out->quantization().zero.at(0) == 0);

            CHECK(0 == logistic_uint8(in_buf, input_zero, in_mul_and_shift.multiplier, -in_mul_and_shift.shift, out_buf));
        } else if (op_ == Tanh) {
            // It's a easier to compute 2^(2*x*(log2(e))) than e^(2*x).
            const double real_in_multiplier = 2.0f * in_scale * std::log2(std::exp(1.0f)) / (1 << left_shift);

            auto in_mul_and_shift = get_quantized_mul_and_shift_smaller_than_one(real_in_multiplier, 16);
            assert(in_mul_and_shift.shift <= 0);

            assert(out->quantization().scale.at(0) == 1.0f / 128.0f);
            assert(out->quantization().zero.at(0) == 128);

            CHECK(0 == tanh_uint8(in_buf, input_zero, in_mul_and_shift.multiplier, -in_mul_and_shift.shift, out_buf));
        } else {
            CHECK(false) << "Unsupported unary op\n";
        }
    }
}

void BinaryOp::accept(OpVisitor *v) {
    v->visit(this);
}

void ConcatenationOp::accept(OpVisitor *v) {
    v->visit(this);
}

void Conv2DOp::accept(OpVisitor *v) {
    v->visit(this);
}

void DepthwiseConv2DOp::accept(OpVisitor *v) {
    v->visit(this);
}

void FullyConnectedOp::accept(OpVisitor *v) {
    v->visit(this);
}

void L2NormalizationOp::accept(OpVisitor *v) {
    v->visit(this);
}

void PadOp::accept(OpVisitor *v) {
    v->visit(this);
}

void PoolOp::accept(OpVisitor *v) {
    v->visit(this);
}

void SoftmaxOp::accept(OpVisitor *v) {
    v->visit(this);
}

void ReductionOp::accept(OpVisitor *v) {
    v->visit(this);
}

void ReshapeOp::accept(OpVisitor *v) {
    v->visit(this);
}

void TileConvFilterOp::accept(OpVisitor *v) {
    v->visit(this);
}

void UnaryOp::accept(OpVisitor *v) {
    v->visit(this);
}

}  // namespace hannk
