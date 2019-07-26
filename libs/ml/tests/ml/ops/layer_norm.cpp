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

#include "math/tensor.hpp"
#include "ml/ops/layer_norm.hpp"
#include "vectorise/fixed_point/fixed_point.hpp"

#include "gtest/gtest.h"

#include <vector>

template <typename T>
class LayerNormTest : public ::testing::Test
{
};

using MyTypes = ::testing::Types<fetch::math::Tensor<float>>;

TYPED_TEST_CASE(LayerNormTest, MyTypes);

TYPED_TEST(LayerNormTest, forward_test_2d)
{
  using ArrayType = TypeParam;
  using DataType  = typename TypeParam::Type;

  ArrayType data = ArrayType::FromString(
      "1, 2;"
      "2, 3;"
			"3, 6");

  ArrayType gt = ArrayType::FromString(
		   "-1.2247448, -0.98058067;"
		   "0, -0.39223227;"
		   "1.22474487, 1.372812945");

  fetch::ml::ops::LayerNorm<ArrayType> op(data.shape());

  ArrayType prediction(op.ComputeOutputShape({std::make_shared<ArrayType>(data)}));
  op.Forward({std::make_shared<ArrayType>(data)}, prediction);

  // test correct values
  ASSERT_TRUE(prediction.AllClose(gt, fetch::math::function_tolerance<DataType>(),
                                  fetch::math::function_tolerance<DataType>()));
}

TYPED_TEST(LayerNormTest, forward_test_3d)
{
	using ArrayType = TypeParam;
	using DataType  = typename TypeParam::Type;
	
	ArrayType data = ArrayType::FromString(
	 "1, 2, 3, 0;"
	 "2, 3, 2, 1;"
	 "3, 6, 4, 13");
	
	data.Reshape({3, 2, 2});
	auto s1 = data.View(0).Copy().ToString();
	auto s2 = data.View(1).Copy().ToString();
	
	ArrayType gt = ArrayType::FromString(
	"-1.22474487, -0.98058068, 0, -0.79006571;"
 "0, -0.39223227, -1.22474487,  -0.62076591;"
	"1.22474487,  1.37281295, 1.22474487, 1.41083162");
	
	fetch::ml::ops::LayerNorm<ArrayType> op(data.shape());
	
	ArrayType prediction(op.ComputeOutputShape({std::make_shared<ArrayType>(data)}));
	op.Forward({std::make_shared<ArrayType>(data)}, prediction);
	
	// test correct values
	ASSERT_TRUE(prediction.AllClose(gt, fetch::math::function_tolerance<DataType>(),
	                                fetch::math::function_tolerance<DataType>()));
}