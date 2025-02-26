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

#include "meta/log2.hpp"
#include "vectorise/memory/iterator.hpp"
#include "vectorise/memory/parallel_dispatcher.hpp"
#include "vectorise/memory/vector_slice.hpp"

#include <mm_malloc.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace fetch {
namespace memory {

template <typename T, std::size_t type_size = sizeof(T)>
class SharedArray : public VectorSlice<T, type_size>
{
public:
  static_assert(sizeof(T) >= type_size, "Invalid object size");

  // TODO(check IfIsPodOrFixedPoint memory safe)
  //  static_assert(std::is_pod<T>::value, "Can only be used with POD types");
  //  static_assert(meta::IfIsPodOrFixedPoint<T>::value, "can only be used with POD or FixedPoint");
  using size_type  = std::size_t;
  using data_type  = std::shared_ptr<T>;
  using super_type = VectorSlice<T, type_size>;
  using self_type  = SharedArray<T, type_size>;
  using type       = T;

  explicit SharedArray(std::size_t const &n)
    : super_type()
  {
    this->size_ = n;

    if (n > 0)
    {
      data_ = std::shared_ptr<T>(
          static_cast<T *>(_mm_malloc(this->padded_size() * sizeof(type), 64)), _mm_free);

      this->pointer_ = data_.get();
    }
  }

  constexpr SharedArray() = default;

  SharedArray(SharedArray const &other) noexcept
    : super_type(other.pointer_, other.size())
    , data_(other.data_)
  {}

  SharedArray(SharedArray const &other, uint64_t offset, uint64_t size) noexcept
    : super_type(other.data_.get() + offset, size)
    , data_(other.data_)
  {}

  SharedArray(SharedArray &&other) noexcept
  {
    std::swap(this->size_, other.size_);
    std::swap(this->data_, other.data_);
    std::swap(this->pointer_, other.pointer_);
  }

  SharedArray &operator=(SharedArray &&other) noexcept
  {
    std::swap(this->size_, other.size_);
    std::swap(this->data_, other.data_);
    std::swap(this->pointer_, other.pointer_);
    return *this;
  }

  self_type &operator=(SharedArray const &other) noexcept
  {
    if (&other == this)
    {
      return *this;
    }

    this->size_ = other.size_;

    if (other.data_)
    {
      this->data_    = other.data_;
      this->pointer_ = other.pointer_;
    }
    else
    {
      this->data_.reset();
      this->pointer_ = nullptr;
    }

    return *this;
  }

  ~SharedArray() = default;

  self_type Copy() const
  {
    // TODO(issue 2): Use memcopy
    self_type ret(this->size_);
    for (std::size_t i = 0; i < this->size_; ++i)
    {
      ret[i] = this->At(i);
    }

    return ret;
  }

  bool IsUnique() const noexcept
  {
    return data_.use_count() < 2;
  }

  uint64_t UseCount() const noexcept
  {
    long const use_count = data_.use_count();
    return use_count < 0 ? 0 : static_cast<uint64_t>(use_count);
  }

private:
  data_type data_ = nullptr;
};

}  // namespace memory
}  // namespace fetch
