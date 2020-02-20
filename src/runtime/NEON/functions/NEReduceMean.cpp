/*
 * Copyright (c) 2018-2019 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/NEON/functions/NEReduceMean.h"

#include "arm_compute/core/CPP/Validate.h"
#include "arm_compute/core/Error.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/runtime/NEON/NEScheduler.h"

namespace arm_compute
{
namespace
{
inline TensorShape calculate_reduce_mean_shape(ITensor *input, const Coordinates &reduction_axis, bool keep_dims)
{
    const int   reduction_ops = reduction_axis.num_dimensions();
    Coordinates axis_local    = reduction_axis;
    const int   input_dims    = input->info()->num_dimensions();
    convert_negative_axis(axis_local, input_dims);
    TensorShape out_shape = input->info()->tensor_shape();
    // Configure reshape layer if we want to drop the dimensions
    if(!keep_dims)
    {
        // We have to sort the reduction axis vectors in order for remove_dimension
        // to work properly
        std::sort(axis_local.begin(), axis_local.begin() + reduction_ops);
        for(int i = 0; i < reduction_ops; ++i)
        {
            out_shape.remove_dimension(axis_local[i] - i);
        }
        return out_shape;
    }
    else
    {
        for(int i = 0; i < reduction_ops; ++i)
        {
            out_shape.set(axis_local[i], 1);
        }
        return out_shape;
    }
}
} // namespace

NEReduceMean::NEReduceMean(std::shared_ptr<IMemoryManager> memory_manager)
    : _memory_group(std::move(memory_manager)), _reduction_kernels(), _reduced_outs(), _reshape(), _reduction_ops(), _keep_dims()
{
}

Status validate_config(const ITensorInfo *input, const Coordinates &reduction_axis, bool keep_dims, const ITensorInfo *output)
{
    ARM_COMPUTE_UNUSED(keep_dims);
    ARM_COMPUTE_RETURN_ERROR_ON_NULLPTR(input, output);
    ARM_COMPUTE_RETURN_ERROR_ON_CPU_F16_UNSUPPORTED(input);
    ARM_COMPUTE_RETURN_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::QASYMM8, DataType::F16, DataType::F32);
    ARM_COMPUTE_RETURN_ERROR_ON(reduction_axis.num_dimensions() < 1);
    ARM_COMPUTE_RETURN_ERROR_ON(reduction_axis.num_dimensions() > input->num_dimensions());

    const unsigned int reduction_ops = reduction_axis.num_dimensions();
    const int          input_dims    = input->num_dimensions();
    Coordinates        axis_local    = reduction_axis;

    for(unsigned int i = 0; i < axis_local.num_dimensions(); ++i)
    {
        //axis: The dimensions to reduce. Must be in the range [-rank(input_tensor), rank(input_tensor)).
        ARM_COMPUTE_RETURN_ERROR_ON(axis_local[i] < (-static_cast<int>(input->num_dimensions())));
        ARM_COMPUTE_RETURN_ERROR_ON(axis_local[i] >= static_cast<int>(input->num_dimensions()));
    }

    if(output->tensor_shape().total_size() != 0)
    {
        // Only validate if not using auto_init for the output tensor
        TensorShape out_shape = input->tensor_shape();
        // Validate output_shape only if not using auto_init
        convert_negative_axis(axis_local, input_dims);
        std::sort(axis_local.begin(), axis_local.begin() + reduction_ops);
        for(unsigned int i = 0; i < reduction_ops; ++i)
        {
            ARM_COMPUTE_RETURN_ERROR_ON(axis_local[i] > 3);
            ARM_COMPUTE_RETURN_ERROR_ON(static_cast<unsigned int>(axis_local[i]) > input->num_dimensions() - 1);
            if(output->total_size() > 0 && keep_dims)
            {
                ARM_COMPUTE_RETURN_ERROR_ON(output->dimension(axis_local[i]) != 1);
            }
            if(keep_dims)
            {
                out_shape.set(axis_local[i], 1);
            }
            else
            {
                ARM_COMPUTE_RETURN_ERROR_ON(i > static_cast<unsigned int>(axis_local[i]));
                const unsigned int remove_index = axis_local[i] - i;
                ARM_COMPUTE_RETURN_ERROR_ON(remove_index >= out_shape.num_dimensions());
                out_shape.remove_dimension(remove_index);
            }
        }
        const TensorInfo out_info = input->clone()->set_tensor_shape(out_shape);
        ARM_COMPUTE_RETURN_ERROR_ON_MISMATCHING_SHAPES(output, &out_info);
    }
    return Status{};
}

Status NEReduceMean::validate(const ITensorInfo *input, const Coordinates &reduction_axis, bool keep_dims, const ITensorInfo *output)
{
    return validate_config(input, reduction_axis, keep_dims, output);
}

void NEReduceMean::configure(ITensor *input, const Coordinates &reduction_axis, bool keep_dims, ITensor *output)
{
    // Perform validate step
    ARM_COMPUTE_ERROR_THROW_ON(NEReduceMean::validate(input->info(), reduction_axis, keep_dims, output->info()));
    // Output auto inizialitation if not yet initialized
    const TensorShape output_shape = calculate_reduce_mean_shape(input, reduction_axis, keep_dims);
    auto_init_if_empty(*output->info(), input->info()->clone()->set_tensor_shape(output_shape));

    _reduction_ops = reduction_axis.num_dimensions();
    _reduction_kernels.resize(_reduction_ops);
    _reduced_outs.resize(_reduction_ops - (keep_dims ? 1 : 0));
    _keep_dims = keep_dims;

    Coordinates axis_local = reduction_axis;
    const int   input_dims = input->info()->num_dimensions();

    convert_negative_axis(axis_local, input_dims);

    // Perform reduction for every axis
    for(int i = 0; i < _reduction_ops; ++i)
    {
        TensorShape out_shape = i == 0 ? input->info()->tensor_shape() : (&_reduced_outs[i - 1])->info()->tensor_shape();
        out_shape.set(axis_local[i], 1);
        auto in = (i == 0) ? input : (&_reduced_outs[i - 1]);

        if(i == _reduction_ops - 1 && keep_dims)
        {
            _reduction_kernels[i].configure(in, output, axis_local[i], ReductionOperation::MEAN_SUM);
        }
        else
        {
            _reduced_outs[i].allocator()->init(TensorInfo(out_shape, input->info()->num_channels(), input->info()->data_type(), input->info()->quantization_info()));
            _memory_group.manage(&_reduced_outs[i]);
            _reduction_kernels[i].configure(in, &_reduced_outs[i], axis_local[i], ReductionOperation::MEAN_SUM);
        }
    }

    // Allocate intermediate tensors
    for(int i = 0; i < _reduction_ops - (keep_dims ? 1 : 0); ++i)
    {
        _reduced_outs[i].allocator()->allocate();
    }

    // Configure reshape layer if we want to drop the dimensions
    if(!keep_dims)
    {
        TensorShape out_shape = input->info()->tensor_shape();
        // We have to sort the reduction axis vectors in order for remove_dimension
        // to work properly
        std::sort(axis_local.begin(), axis_local.begin() + _reduction_ops);
        for(int i = 0; i < _reduction_ops; ++i)
        {
            out_shape.remove_dimension(axis_local[i] - i);
        }
        auto_init_if_empty(*output->info(), input->info()->clone()->set_tensor_shape(out_shape));
        _reshape.configure(&_reduced_outs[_reduction_ops - 1], output);
    }

    m_tmp_output = output;
    m_tmp_input = input;
}

void NEReduceMean::run()
{
    IOFormatInfo iofmt = IOFormatInfo(IOFormatInfo::PrintRegion::ValidRegion, IOFormatInfo::PrecisionType::Full);
    MemoryGroupResourceScope scope_mg(_memory_group);
    for(auto &kernel : _reduction_kernels)
    {
        kernel.run();
    }

    if(!_keep_dims)
    {
        _reshape.run();
    }

    /*
    std::cout << "ReduceMean output:" << std::endl;
    m_tmp_output->print(std::cout, iofmt);
    */
#define NAIVE_CHECK
#ifdef NAIVE_CHECK
// Other naive method:
// get info
ITensorInfo *input_info = m_tmp_input->info();
ITensorInfo *output_info = m_tmp_output->info();

size_t input_dims = input_info->num_dimensions();
size_t input_chs = input_info->num_channels();
size_t output_dims = output_info->num_dimensions();
size_t output_chs = output_info->num_channels();

/*
std::cout << "Input dims: " << input_dims << " Input Chl: " << input_chs << std::endl;
std::cout << "Output dims: " << output_dims << " Output Chl: " << output_chs << std::endl;
*/

Coordinates c;
c.set_num_dimensions(output_dims);
std::fill(c.begin(), c.end(), 0);
TensorShape const &out_shape = output_info->tensor_shape();
TensorShape const &in_shape = input_info->tensor_shape();

/*
printf("InputShape:");
for (int i = 0; i < input_dims; i++) {
    printf(" %d", in_shape[i]);
}
printf("\n");

printf("OutputShape:");
for (int i = 0; i < c.num_dimensions(); i++) {
    printf(" %d", out_shape[i]);
}
printf("\n");
*/

// assume axis is [1, 2]
Coordinates ic;
ic.set_num_dimensions(input_dims);
std::fill(c.begin(), c.end(), 0);
std::vector<uint8_t> voutput;
for (int i = 0; i < in_shape[0]; i++) {
    ssize_t mean = 0;
    ssize_t dim = 0;
    for (int j = 0; j < in_shape[1]; j++) {
        for (int k = 0; k < in_shape[2]; k++) {
            ic[0] = i;
            ic[1] = j;
            ic[2] = k;
            ic[3] = 0;
            uint8_t *p = m_tmp_input->ptr_to_element(ic);
            uint8_t b = *p;
            mean += b;
            dim++;
        }
    }
    //printf("sum: %zu dim: %zu", mean, dim);
    //mean = (ssize_t)(((float)mean / (float)dim) + 0.5);
    //mean /= (dim-1);
    mean /= dim;
    //printf(" mean: %zu\n", mean);
    voutput.push_back(mean);
}


//printf("voutput size: %zu\n", voutput.size());

// access buffer
// input->ptr_to_element();
// compute max
// reduce
//
    // Output results... somehow?
/*
    std::cout << "VOutput: "<< std::endl;
    for (auto it = voutput.begin(); it != voutput.end(); ++it) {
        printf("%d ", *it);
    }
    std::cout << std::endl;

    std::cout << "Output: "<< std::endl;
    m_tmp_output->print(std::cout, iofmt);

    for (int i = 0; i < 64; i++) {
        c[0] = i;
        uint8_t *p = m_tmp_output->ptr_to_element(c);
        if (voutput[i] != *p) {
            printf("Differ!: %d %d vs %d\n", i, voutput[i], *p);
            break;
        }
    }
*/
int v = 0;
for (;;) {
    uint8_t *p = m_tmp_output->ptr_to_element(c);
    *p = voutput[v];


    // Iterate dimensions
    c[c.num_dimensions()-1]++;
    for (int i = c.num_dimensions()-1; i >= 1; i--) {
        if (c[i] >= out_shape[i]) {
            c[i] = 0;
            c[i-1]++;
        }
        else {
            break;
        }
    }
    if (c[0] >= out_shape[0]) {
        break;
    }
    v++;
}



#endif
}
} // namespace arm_compute