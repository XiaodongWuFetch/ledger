#pragma once
//------------------------------------------------------------------------------
//
//   Copyright 2018-2019 Fetch.AI Limited
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//------------------------------------------------------------------------------

#include "core/assert.hpp"
#include "ml/ops/ops.hpp"

#include <cassert>
#include <memory>
#include <vector>

namespace fetch {
namespace ml {
namespace ops {

template <class T>
class MaskFill : public fetch::ml::Ops<T>
{
public:
  using ArrayType     = T;
  using DataType      = typename ArrayType::Type;
  using SizeType      = typename ArrayType::SizeType;
  using ArrayPtrType  = std::shared_ptr<ArrayType>;
  using VecTensorType = typename Ops<T>::VecTensorType;

  MaskFill(DataType fill_value)
    : fill_value_(fill_value)
  {}
  ~MaskFill() override = default;

  /**
   * based on boolean condition, decide if we need to fill the element with fill_value.
   * @param inputs - two inputs, first is mask, second is the array to be masked
   * array
   * @return
   */
  void Forward(VecTensorType const &inputs, ArrayType &output) override
  {
    assert(inputs.size() == 2);
    assert(output.shape() == this->ComputeOutputShape(inputs));
    assert(inputs.at(0)->shape() == inputs.at(1)->shape());

    if (fill_array_.shape() != inputs.front()->shape())
    {
      fill_array_.Reshape(inputs.front()->shape());
      fill_array_.Fill(fill_value_);
    }
    fetch::math::Switch(*(inputs.at(0)), *(inputs.at(1)), fill_array_, output);
  }

  /**
   * elementwise gradient for second input (the then input) is:
   * error' = mask * error_signal
   */
  std::vector<ArrayType> Backward(VecTensorType const &inputs,
                                  ArrayType const &    error_signal) override
  {
    assert(inputs.size() == 2);
    assert(inputs.at(0)->shape() == inputs.at(1)->shape());
    assert(error_signal.size() == inputs.at(0)->size());

    ArrayType return_signal(inputs.at(0)->shape());
    ArrayType mask_return_signal(inputs.at(0)->shape());

    fetch::math::Multiply(*(inputs.front()), error_signal, return_signal);

    // be adivsed, it is not reasonable to return gradient for mask, so the mask gradient is set to
    // zero here
    return {mask_return_signal, return_signal};
  }

  std::vector<SizeType> ComputeOutputShape(VecTensorType const &inputs) const override
  {
    return inputs.front()->shape();
  }

  static constexpr char const *DESCRIPTOR = "MaskFill";

private:
  DataType  fill_value_;
  ArrayType fill_array_;
};

}  // namespace ops
}  // namespace ml
}  // namespace fetch
