/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
 */
#pragma once

#include "velox/vector/BiasVector.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/ConstantVector.h"
#include "velox/vector/DictionaryVector.h"
#include "velox/vector/FlatVector.h"
#include "velox/vector/SequenceVector.h"
#include "velox/vector/SimpleVector.h"

namespace facebook::velox::test {

class SimpleVectorLoader : public VectorLoader {
 public:
  explicit SimpleVectorLoader(std::function<VectorPtr(RowSet)> loader)
      : loader_(loader) {}

  void load(RowSet rows, ValueHook* hook, VectorPtr* result) override {
    VELOX_CHECK(!hook, "SimpleVectorLoader doesn't support ValueHook");
    *result = loader_(rows);
  }

 private:
  std::function<VectorPtr(RowSet)> loader_;
};

class VectorMaker {
 public:
  explicit VectorMaker(memory::MemoryPool* pool) : pool_(pool) {}

  static std::function<bool(vector_size_t /*row*/)> nullEvery(
      int n,
      int startingFrom = 0) {
    return [n, startingFrom](vector_size_t row) {
      return row >= startingFrom && ((row - startingFrom) % n == 0);
    };
  }

  static std::shared_ptr<const RowType> rowType(
      std::vector<std::shared_ptr<const Type>>&& types);

  RowVectorPtr rowVector(const std::vector<VectorPtr>& children);

  RowVectorPtr rowVector(
      const std::shared_ptr<const RowType>& rowType,
      vector_size_t size);

  template <typename T>
  using EvalType = typename CppToType<T>::NativeType;

  template <typename T>
  FlatVectorPtr<T> flatVector(
      vector_size_t size,
      std::function<T(vector_size_t /*row*/)> valueAt,
      std::function<bool(vector_size_t /*row*/)> isNullAt = nullptr) {
    auto flatVector = std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(CppToType<T>::create(), size, pool_));
    for (vector_size_t i = 0; i < size; i++) {
      if (isNullAt && isNullAt(i)) {
        flatVector->setNull(i, true);
      } else {
        flatVector->set(i, valueAt(i));
      }
    }
    return flatVector;
  }

  template <typename T>
  std::shared_ptr<LazyVector> lazyFlatVector(
      vector_size_t size,
      std::function<T(vector_size_t /*row*/)> valueAt,
      std::function<bool(vector_size_t /*row*/)> isNullAt = nullptr) {
    return std::make_shared<LazyVector>(
        pool_,
        CppToType<T>::create(),
        size,
        std::make_unique<SimpleVectorLoader>([=](RowSet rowSet) {
          // Populate requested rows with correct data and fill in gaps with
          // "garbage".
          SelectivityVector rows(rowSet.back() + 1, false);
          for (auto row : rowSet) {
            rows.setValid(row, true);
          }
          rows.updateBounds();

          auto selectiveValueAt = [&](auto row) {
            return rows.isValid(row) ? valueAt(row) : T();
          };

          std::function<bool(vector_size_t)> selectiveIsNullAt = nullptr;
          if (isNullAt) {
            selectiveIsNullAt = [&](auto row) {
              return rows.isValid(row) ? isNullAt(row) : false;
            };
          }

          return flatVector<T>(size, selectiveValueAt, selectiveIsNullAt);
        }));
  }

  template <typename T>
  FlatVectorPtr<T> flatVector(
      size_t size,
      const TypePtr& type = CppToType<T>::create()) {
    return std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(type, size, pool_));
  }

  /// Create a FlatVector<T>
  /// creates a FlatVector based on elements from the input std::vector.
  ///
  /// Elements are non-nullable.
  ///
  /// Examples:
  ///   auto flatVector = flatVector({1, 2, 3, 4});
  template <typename T>
  FlatVectorPtr<EvalType<T>> flatVector(const std::vector<T>& data);

  /// Create a FlatVector<StringView>
  /// convenience function to create a FlatVector based on a vector of
  /// std::string. Note that the lifetime of the StringViews on the the
  /// returned FlatVector are bound to the lifetime of the vector input
  /// strings, so be careful with temporaries.
  ///
  /// Elements are non-nullable.
  ///
  /// Examples:
  ///   std::vector<std::string> data({"hello", "world"});
  ///   auto flatVector = flatVector(data);
  ///
  /// but not:
  ///
  ///   auto flatVector2 = flatVector({"hello", "world"});
  FlatVectorPtr<StringView> flatVector(const std::vector<std::string>& data) {
    std::vector<StringView> stringViews;
    stringViews.reserve(data.size());
    for (const auto& str : data) {
      stringViews.emplace_back(str);
    }
    return flatVector(stringViews);
  }

  /// Create a FlatVector<T>
  /// creates a FlatVector based on elements from the input std::vector.
  /// Works for primitive types and StringViews.
  ///
  /// Elements are nullable.
  ///
  /// Examples:
  ///   auto flatVector = flatVectorNullable({1, std::nullopt, 3});
  ///   auto flatVectorStr = flatVectorNullable<StringView>({
  ///       StringView("hello"), std::nullopt, StringView("world")});
  template <typename T>
  FlatVectorPtr<T> flatVectorNullable(
      const std::vector<std::optional<T>>& values,
      const TypePtr& type = CppToType<T>::create());

  /// Create a FlatVector<T>
  /// convenience function to create a FlatVector based on a vector of
  /// std::string. Note that the lifetime of the StringViews on the the
  /// returned FlatVector are bound to the lifetime of the vector input
  /// strings.
  ///
  /// Elements are nullable.
  ///
  /// Examples:
  ///   auto flatVector = flatVectorNullable(
  ///       {std::string("hello"), std::nullopt, std::string("world")});
  //
  /// or simply:
  ///
  ///   auto flatVector2 = flatVectorNullable({"hello", std::nullopt, "world"});
  FlatVectorPtr<StringView> flatVectorNullable(
      const std::vector<std::optional<std::string>>& data,
      const TypePtr& type = ScalarType<TypeKind::VARCHAR>::create()) {
    std::vector<std::optional<StringView>> stringViews;
    stringViews.reserve(data.size());
    for (const auto& str : data) {
      if (str == std::nullopt) {
        stringViews.emplace_back(std::nullopt);
      } else {
        stringViews.emplace_back(*str);
      }
    }
    return flatVectorNullable(stringViews, type);
  }

  template <typename T, int TupleIndex, typename TupleType>
  FlatVectorPtr<T> flatVector(const std::vector<TupleType>& data) {
    auto vector = std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(CppToType<T>::create(), data.size(), pool_));
    for (vector_size_t i = 0; i < data.size(); ++i) {
      vector->set(i, std::get<TupleIndex>(data[i]));
    }
    return vector;
  }

  template <typename T>
  FlatVectorPtr<T> allNullFlatVector(vector_size_t size) {
    auto flatVector = std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(CppToType<T>::create(), size, pool_));
    for (vector_size_t i = 0; i < size; i++) {
      flatVector->setNull(i, true);
    }
    return flatVector;
  }

  /// Create a BiasVector<T>
  /// creates a BiasVector (vector encoded using bias encoding) based on a flat
  /// input from an std::vector.
  ///
  /// Elements are nullable.
  ///
  /// Example:
  ///   auto biasVector = maker.biasVector<int64_t>({10, 15, 13, 11, 12, 14});
  template <typename T>
  BiasVectorPtr<T> biasVector(const std::vector<std::optional<T>>& data);

  /// Create a SequenceVector<T>
  /// creates a SequenceVector (vector encoded using RLE) based on a flat
  /// input from an std::vector.
  ///
  /// Elements are nullable.
  ///
  /// Example:
  ///   auto sequenceVector = maker.sequenceVector<int64_t>({
  ///       10, 10, 10, std::nullopt, 15, 15, std::nullopt, std::nullopt});
  template <typename T>
  SequenceVectorPtr<T> sequenceVector(
      const std::vector<std::optional<T>>& data);

  /// Create a ConstantVector<T>
  /// creates a ConstantVector (vector that represents a single constant value)
  /// based on a flat input from an std::vector. The input vector may contain
  /// several elements, but if the input vector contains more than one distinct
  /// element, it fails.
  ///
  /// Elements are nullable.
  ///
  /// Examples:
  ///   auto constantVector = maker.constantVector<int64_t>({11, 11, 11});
  ///   auto constantVector = maker.constantVector<int64_t>(
  ///        {std::nullopt, std::nullopt});
  template <typename T>
  ConstantVectorPtr<T> constantVector(
      const std::vector<std::optional<T>>& data);

  /// Create a DictionaryVector<T>
  /// creates a dictionary encoded vector based on a flat input from an
  /// std::vector.
  ///
  /// Elements are nullable.
  ///
  /// Example:
  ///   auto dictionaryVector = maker.dictionaryVector<int64_t>({
  ///       10, 10, 10, std::nullopt, 15, 15, std::nullopt, std::nullopt});
  template <typename T>
  DictionaryVectorPtr<T> dictionaryVector(
      const std::vector<std::optional<T>>& data);

  /// Convenience function that creates an vector based on input std::vector
  /// data, encoded with given `vecType`.
  template <typename T>
  SimpleVectorPtr<T> encodedVector(
      VectorEncoding::Simple vecType,
      const std::vector<std::optional<T>>& data) {
    switch (vecType) {
      case VectorEncoding::Simple::FLAT:
        return flatVectorNullable(data);
      case VectorEncoding::Simple::CONSTANT:
        return constantVector(data);
      case VectorEncoding::Simple::DICTIONARY:
        return dictionaryVector(data);
      case VectorEncoding::Simple::SEQUENCE:
        return sequenceVector(data);
      case VectorEncoding::Simple::BIASED:
        return biasVector(data);
      default:
        VELOX_UNSUPPORTED("Unsupported encoding type for VectorMaker.");
    }
    return nullptr;
  }

  /// Create a ArrayVector<T>
  /// size and null for individual array is determined by sizeAt and isNullAt
  /// value for individual array is determined by valueAt.
  template <typename T>
  ArrayVectorPtr arrayVector(
      vector_size_t size,
      std::function<vector_size_t(vector_size_t /* row */)> sizeAt,
      std::function<T(vector_size_t /* idx */)> valueAt,
      std::function<bool(vector_size_t /*row */)> isNullAt = nullptr) {
    BufferPtr nulls;
    BufferPtr offsets;
    BufferPtr sizes;
    auto numElements =
        createOffsetsAndSizes(size, sizeAt, isNullAt, &nulls, &offsets, &sizes);

    return std::make_shared<ArrayVector>(
        pool_,
        ARRAY(CppToType<T>::create()),
        nulls,
        size,
        offsets,
        sizes,
        flatVector<T>(numElements, valueAt),
        BaseVector::countNulls(nulls, 0, size));
  }

  template <typename T>
  ArrayVectorPtr arrayVectorImpl(
      std::shared_ptr<const Type> type,
      vector_size_t size,
      std::function<vector_size_t(vector_size_t /* row */)> sizeAt,
      std::function<T(vector_size_t /* row */, vector_size_t /* idx */)>
          valueAt,
      std::function<bool(vector_size_t /*row */)> isNullAt = nullptr) {
    BufferPtr nulls;
    BufferPtr offsets;
    BufferPtr sizes;
    auto numElements =
        createOffsetsAndSizes(size, sizeAt, isNullAt, &nulls, &offsets, &sizes);

    auto flatVector = std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(CppToType<T>::create(), numElements, pool_));
    vector_size_t currentIndex = 0;
    for (vector_size_t i = 0; i < size; ++i) {
      if (isNullAt && isNullAt(i)) {
        continue;
      }
      for (vector_size_t j = 0; j < sizeAt(i); ++j) {
        auto ret = valueAt(i, j);
        flatVector->set(currentIndex, valueAt(i, j));
        currentIndex++;
      }
    }

    return std::make_shared<ArrayVector>(
        pool_,
        type,
        nulls,
        size,
        offsets,
        sizes,
        flatVector,
        BaseVector::countNulls(nulls, 0, size));
  }

  /// Create a ArrayVector<T>
  /// size and null for individual array is determined by sizeAt and isNullAt
  /// value for elements of each array in a given row is determined by valueAt.
  template <typename T>
  ArrayVectorPtr arrayVector(
      vector_size_t size,
      std::function<vector_size_t(vector_size_t /* row */)> sizeAt,
      std::function<T(vector_size_t /* row */, vector_size_t /* idx */)>
          valueAt,
      std::function<bool(vector_size_t /*row */)> isNullAt = nullptr) {
    return arrayVectorImpl(
        ARRAY(CppToType<T>::create()), size, sizeAt, valueAt, isNullAt);
  }

  /// Create a FixedArrayVector<T>
  /// size and null for individual array is determined by sizeAt and isNullAt
  /// value for elements of each array in a given row is determined by valueAt.
  template <typename T>
  ArrayVectorPtr fixedSizeArrayVector(
      int len,
      vector_size_t size,
      std::function<T(vector_size_t /* row */, vector_size_t /* idx */)>
          valueAt,
      std::function<bool(vector_size_t /*row */)> isNullAt) {
    return arrayVectorImpl(
        FIXED_SIZE_ARRAY(len, CppToType<T>::create()),
        size,
        [=](vector_size_t i) -> vector_size_t {
          // All entries are the same fixed size, _except_ null entries are size
          // 0.
          return isNullAt(i) ? 0 : len;
        }, // sizeAt
        valueAt,
        isNullAt);
  }

  template <typename T>
  ArrayVectorPtr arrayVectorImpl(
      std::shared_ptr<const Type> type,
      const std::vector<std::vector<T>>& data) {
    vector_size_t size = data.size();
    BufferPtr offsets = AlignedBuffer::allocate<vector_size_t>(size, pool_);
    BufferPtr sizes = AlignedBuffer::allocate<vector_size_t>(size, pool_);

    auto rawOffsets = offsets->asMutable<vector_size_t>();
    auto rawSizes = sizes->asMutable<vector_size_t>();

    // Count number of elements.
    vector_size_t numElements = 0;
    for (const auto& array : data) {
      numElements += array.size();
    }

    // Create the underlying flat vector.
    auto flatVector = std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(CppToType<T>::create(), numElements, pool_));

    vector_size_t currentIdx = 0;
    for (const auto& arrayValue : data) {
      *rawSizes++ = arrayValue.size();
      *rawOffsets++ = currentIdx;

      for (auto arrayElement : arrayValue) {
        flatVector->set(currentIdx++, arrayElement);
      }
    }

    return std::make_shared<ArrayVector>(
        pool_, type, nullptr, size, offsets, sizes, flatVector, 0);
  }

  /// Create a ArrayVector<T>
  /// array elements are created based on input std::vectors and are
  /// non-nullable.
  template <typename T>
  ArrayVectorPtr arrayVector(const std::vector<std::vector<T>>& data) {
    return arrayVectorImpl(ARRAY(CppToType<T>::create()), data);
  }

  /// Create a FixedSizeArrayVector<T>
  /// array elements are created based on input std::vectors and are
  /// non-nullable.  All vectors should be the same size.
  template <typename T>
  ArrayVectorPtr fixedSizeArrayVector(
      int len,
      const std::vector<std::vector<T>>& data) {
    return arrayVectorImpl(FIXED_SIZE_ARRAY(len, CppToType<T>::create()), data);
  }

  /// Create an ArrayVector<ROW> from nested std::vectors of Variants.
  ArrayVectorPtr arrayOfRowVector(
      const RowTypePtr& rowType,
      const std::vector<std::vector<variant>>& data);

  template <typename T>
  ArrayVectorPtr arrayVectorNullableImpl(
      std::shared_ptr<const Type> type,
      const std::vector<std::optional<std::vector<std::optional<T>>>>& data) {
    vector_size_t size = data.size();
    BufferPtr offsets = AlignedBuffer::allocate<vector_size_t>(size, pool_);
    BufferPtr sizes = AlignedBuffer::allocate<vector_size_t>(size, pool_);
    BufferPtr nulls = AlignedBuffer::allocate<uint64_t>(size, pool_);

    auto rawOffsets = offsets->asMutable<vector_size_t>();
    auto rawSizes = sizes->asMutable<vector_size_t>();
    auto rawNulls = nulls->asMutable<uint64_t>();
    bits::fillBits(rawNulls, 0, size, pool_);

    // Count number of elements.
    vector_size_t numElements = 0;
    vector_size_t indexPtr = 0;
    vector_size_t nullCount = 0;
    for (const auto& array : data) {
      numElements += array.has_value() ? array.value().size() : 0;
      if (!array.has_value()) {
        bits::setNull(rawNulls, indexPtr, true);
        nullCount++;
      }
      indexPtr++;
    }

    // Create the underlying flat vector.
    auto flatVector = std::dynamic_pointer_cast<FlatVector<T>>(
        BaseVector::create(CppToType<T>::create(), numElements, pool_));
    auto elementRawNulls = flatVector->mutableRawNulls();

    vector_size_t currentIdx = 0;
    vector_size_t elementNullCount = 0;

    for (const auto& arrayValue : data) {
      *rawSizes++ = arrayValue.has_value() ? arrayValue.value().size() : 0;
      *rawOffsets++ = currentIdx;

      if (arrayValue.has_value()) {
        for (auto arrayElement : arrayValue.value()) {
          if (arrayElement == std::nullopt) {
            bits::setNull(elementRawNulls, currentIdx, true);
            ++elementNullCount;
          } else {
            flatVector->set(currentIdx, *arrayElement);
          }
          ++currentIdx;
        }
      }
    }
    flatVector->setNullCount(elementNullCount);

    return std::make_shared<ArrayVector>(
        pool_, type, nulls, size, offsets, sizes, flatVector, nullCount);
  }

  /// Create a ArrayVector<T>
  /// array elements are created based on input std::vectors and are
  /// nullable. Only null array elements are supported; not null arrays.
  template <typename T>
  ArrayVectorPtr arrayVectorNullable(
      const std::vector<std::optional<std::vector<std::optional<T>>>>& data) {
    return arrayVectorNullableImpl(ARRAY(CppToType<T>::create()), data);
  }

  /// Create a FixedSizeArrayVector<T> array elements are created
  /// based on input std::vectors and are nullable.  Only null array
  /// elements are supported; not null arrays.  All vectors should be
  /// the same size.
  template <typename T>
  ArrayVectorPtr fixedSizeArrayVectorNullable(
      int len,
      const std::vector<std::optional<std::vector<std::optional<T>>>>& data) {
    return arrayVectorNullableImpl(
        FIXED_SIZE_ARRAY(len, CppToType<T>::create()), data);
  }

  ArrayVectorPtr allNullArrayVector(
      vector_size_t size,
      const std::shared_ptr<const Type>& elementType);

  /// Create a Map<TKey, TValue>
  /// size and null for individual map is determined by sizeAt and isNullAt
  /// key and value for individual map is determined by keyAt and valueAt
  template <typename TKey, typename TValue>
  MapVectorPtr mapVector(
      vector_size_t size,
      std::function<vector_size_t(vector_size_t /* row */)> sizeAt,
      std::function<TKey(vector_size_t /* idx */)> keyAt,
      std::function<TValue(vector_size_t /* idx */)> valueAt,
      std::function<bool(vector_size_t /*row */)> isNullAt = nullptr,
      std::function<bool(vector_size_t /*row */)> valueIsNullAt = nullptr) {
    BufferPtr nulls;
    BufferPtr offsets;
    BufferPtr sizes;
    auto numElements =
        createOffsetsAndSizes(size, sizeAt, isNullAt, &nulls, &offsets, &sizes);

    return std::make_shared<MapVector>(
        pool_,
        MAP(CppToType<TKey>::create(), CppToType<TValue>::create()),
        nulls,
        size,
        offsets,
        sizes,
        flatVector<TKey>(numElements, keyAt),
        flatVector<TValue>(numElements, valueAt, valueIsNullAt),
        BaseVector::countNulls(nulls, 0, size));
  }

  template <typename TKey, typename TValue>
  MapVectorPtr mapVector(
      vector_size_t size,
      std::function<vector_size_t(vector_size_t /* mapRow */)> sizeAt,
      std::function<TKey(vector_size_t /* mapRow */, vector_size_t /*row*/)>
          keyAt,
      std::function<TValue(vector_size_t /* mapRow */, vector_size_t /*row*/)>
          valueAt,
      std::function<bool(vector_size_t /*mapRow */)> isNullAt = nullptr) {
    BufferPtr nulls;
    BufferPtr offsets;
    BufferPtr sizes;
    auto numElements =
        createOffsetsAndSizes(size, sizeAt, isNullAt, &nulls, &offsets, &sizes);

    auto rawNulls = nulls ? nulls->asMutable<uint64_t>() : nullptr;
    auto rawSizes = sizes->asMutable<vector_size_t>();

    std::vector<TKey> keys;
    keys.reserve(numElements);
    std::vector<TValue> values;
    values.reserve(numElements);
    for (vector_size_t mapRow = 0; mapRow < size; mapRow++) {
      if (rawNulls && bits::isBitNull(rawNulls, mapRow)) {
        continue;
      }

      auto mapSize = rawSizes[mapRow];
      for (vector_size_t row = 0; row < mapSize; row++) {
        keys.push_back(keyAt(mapRow, row));
        values.push_back(valueAt(mapRow, row));
      }
    }

    return std::make_shared<MapVector>(
        pool_,
        MAP(CppToType<TKey>::create(), CppToType<TValue>::create()),
        nulls,
        size,
        offsets,
        sizes,
        flatVector(keys),
        flatVector(values),
        BaseVector::countNulls(nulls, 0, size));
  }

  MapVectorPtr allNullMapVector(
      vector_size_t size,
      const std::shared_ptr<const Type>& keyType,
      const std::shared_ptr<const Type>& valueType);

  /// Create a FlatVector from a variant containing a scalar value.
  template <TypeKind kind>
  VectorPtr toFlatVector(variant value) {
    using T = typename TypeTraits<kind>::NativeType;
    if constexpr (std::is_same_v<T, StringView>) {
      return flatVector<StringView>({StringView(value.value<const char*>())});
    } else {
      return flatVector(std::vector<T>(1, value.value<T>()));
    }
  }

  /// Create constant vector of type ROW from a variant.
  VectorPtr
  constantRow(const RowTypePtr& rowType, variant value, vector_size_t size) {
    VELOX_CHECK_EQ(value.kind(), TypeKind::ROW);

    std::vector<VectorPtr> fields(rowType->size());
    for (auto i = 0; i < rowType->size(); i++) {
      fields[i] = VELOX_DYNAMIC_SCALAR_TYPE_DISPATCH(
          toFlatVector, rowType->childAt(i)->kind(), value.row()[i]);
    }

    return BaseVector::wrapInConstant(
        size,
        0,
        std::make_shared<RowVector>(
            pool_, rowType, nullptr, 1, std::move(fields)));
  }

  static VectorPtr flatten(const VectorPtr& vector);

 private:
  vector_size_t createOffsetsAndSizes(
      vector_size_t size,
      std::function<vector_size_t(vector_size_t /* row */)> sizeAt,
      std::function<bool(vector_size_t /*row */)> isNullAt,
      BufferPtr* nulls,
      BufferPtr* offsets,
      BufferPtr* sizes);

  memory::MemoryPool* pool_;
};

} // namespace facebook::velox::test

#include "velox/vector/tests/VectorMaker-inl.h"
