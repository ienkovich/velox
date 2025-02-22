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

#include "velox/row/UnsafeRowSerializer.h"
#include <gtest/gtest.h>

#include "velox/row/UnsafeRowDynamicSerializer.h"
#include "velox/type/Type.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/tests/VectorMaker.h"

namespace facebook::velox::row {
using namespace facebook::velox;

class UnsafeRowSerializerTests : public ::testing::Test {
 protected:
  UnsafeRowSerializerTests() {
    clearBuffer();
  }

  void clearBuffer() {
    std::memset(buffer_, 0, 1024);
  }

  std::unique_ptr<memory::ScopedMemoryPool> pool_ =
      memory::getDefaultScopedMemoryPool();
  BufferPtr bufferPtr_ = AlignedBuffer::allocate<char>(1024, pool_.get(), true);
  // Variable pointing to the row pointer held by the smart pointer BufferPtr.
  char* buffer_ = bufferPtr_->asMutable<char>();
  std::unique_ptr<velox::test::VectorMaker> vectorMaker_ =
      std::make_unique<velox::test::VectorMaker>(pool_.get());

  template <typename T>
  testing::AssertionResult checkFixedLength(
      std::optional<size_t> serializedSize,
      size_t expectedSize,
      const T* expectedValue) {
    if (serializedSize != expectedSize) {
      return testing::AssertionFailure()
          << "Expected serializedSize " << expectedSize << " but got "
          << serializedSize.value();
    }
    if (std::memcmp(buffer_, expectedValue, sizeof(T)) != 0) {
      return testing::AssertionFailure()
          << "Buffer is " << reinterpret_cast<T*>(buffer_)[0]
          << " but expected " << expectedValue[0];
    }
    return testing::AssertionSuccess();
  }

  template <typename T>
  testing::AssertionResult checkVariableLength(
      std::optional<size_t> serializedSize,
      size_t expectedSize,
      T* expectedValue) {
    if (serializedSize != expectedSize) {
      return testing::AssertionFailure()
          << "Expected serializedSize " << expectedSize << " but got "
          << serializedSize.value();
    }

    for (int i = 0; i < expectedSize; i++) {
      if (std::memcmp(
              buffer_ + i,
              reinterpret_cast<const uint8_t*>(expectedValue) + i,
              1) != 0) {
        return testing::AssertionFailure()
            << "Buffer and expectedValue differ at index " << i;
      }
    }
    return testing::AssertionSuccess();
  }

  testing::AssertionResult checkIsNull(std::optional<size_t> serializedSize) {
    return !serializedSize.has_value() ? testing::AssertionSuccess()
                                       : testing::AssertionFailure();
  }

  void setNull(uint64_t* nullsPtr, size_t i, bool isNull) {
    bits::setBit(nullsPtr, i, bits::kNull ? isNull : !isNull);
  }

  template <typename T>
  VectorPtr makeFlatVectorPtr(
      size_t flatVectorSize,
      const TypePtr type,
      memory::MemoryPool* pool,
      bool* nullsValue,
      T* elementValue) {
    auto vector = BaseVector::create(type, flatVectorSize, pool);
    auto flatVector = vector->asFlatVector<T>();

    size_t nullCount = 0;
    for (size_t i = 0; i < flatVectorSize; i++) {
      if (nullsValue[i]) {
        vector->setNull(i, true);
      } else {
        vector->setNull(i, false);
        flatVector->set(i, elementValue[i]);
        nullCount++;
      }
    }
    vector->setNullCount(nullCount);
    return vector;
  }

  ArrayVectorPtr makeArrayVectorPtr(
      size_t arrayVectorSize,
      memory::MemoryPool* pool,
      int32_t* offsetsValue,
      vector_size_t* lengthsValue,
      bool* nullsValue,
      const TypePtr type,
      VectorPtr elements) {
    BufferPtr offsets = AlignedBuffer::allocate<int32_t>(arrayVectorSize, pool);
    auto* offsetsPtr = offsets->asMutable<int32_t>();
    BufferPtr lengths =
        AlignedBuffer::allocate<vector_size_t>(arrayVectorSize, pool);
    auto* lengthsPtr = lengths->asMutable<vector_size_t>();
    BufferPtr nulls =
        AlignedBuffer::allocate<char>(bits::nbytes(arrayVectorSize), pool);
    auto* nullsPtr = nulls->asMutable<uint64_t>();

    size_t nullCount = 0;
    for (size_t i = 0; i < arrayVectorSize; i++) {
      offsetsPtr[i] = offsetsValue[i];
      lengthsPtr[i] = lengthsValue[i];
      setNull(nullsPtr, i, nullsValue[i]);
      if (nullsValue[i]) {
        nullCount++;
      }
    }

    return std::make_shared<ArrayVector>(
        pool,
        type,
        nulls,
        arrayVectorSize,
        offsets,
        lengths,
        elements,
        nullCount);
  }

  MapVectorPtr makeMapVectorPtr(
      size_t mapVectorSize,
      memory::MemoryPool* pool,
      int32_t* offsetsValue,
      vector_size_t* lengthsValue,
      bool* nullsValue,
      const TypePtr type,
      VectorPtr keys,
      VectorPtr values) {
    BufferPtr offsets = AlignedBuffer::allocate<int32_t>(mapVectorSize, pool);
    auto* offsetsPtr = offsets->asMutable<int32_t>();
    BufferPtr lengths =
        AlignedBuffer::allocate<vector_size_t>(mapVectorSize, pool);
    auto* lengthsPtr = lengths->asMutable<vector_size_t>();
    BufferPtr nulls =
        AlignedBuffer::allocate<char>(bits::nbytes(mapVectorSize), pool);
    auto* nullsPtr = nulls->asMutable<uint64_t>();

    size_t nullCount = 0;
    for (size_t i = 0; i < mapVectorSize; i++) {
      offsetsPtr[i] = offsetsValue[i];
      lengthsPtr[i] = lengthsValue[i];
      setNull(nullsPtr, i, nullsValue[i]);
      if (nullsValue[i]) {
        nullCount++;
      }
    }

    return std::make_shared<MapVector>(
        pool,
        type,
        nulls,
        mapVectorSize,
        offsets,
        lengths,
        keys,
        values,
        nullCount);
  }
};

TEST_F(UnsafeRowSerializerTests, fixedLengthPrimitive) {
  int16_t smallint = 0x1234;
  auto smallintSerialized = UnsafeRowDynamicSerializer::serialize(
      createScalarType(TypeKind::SMALLINT), smallint, buffer_);
  ASSERT_TRUE(checkFixedLength(smallintSerialized, 0, &smallint));

  float real = 3.4;
  auto realSerialized = UnsafeRowDynamicSerializer::serialize(
      createScalarType(TypeKind::REAL), real, buffer_);
  EXPECT_TRUE(checkFixedLength(realSerialized, 0, &real));

  bool boolean = true;
  auto boolSerialized =
      UnsafeRowSerializer::serialize<BooleanType>(boolean, buffer_);
  EXPECT_TRUE(checkFixedLength(boolSerialized, 0, &boolean));
}

TEST_F(UnsafeRowSerializerTests, fixedLengthVectorPtr) {
  bool nulls[5] = {false, false, false, false, false};
  int32_t elements[5] = {
      0x01010101, 0x01010101, 0x01010101, 0x01234567, 0x01010101};
  auto intVector =
      makeFlatVectorPtr<int32_t>(5, INTEGER(), pool_.get(), nulls, elements);

  auto intSerialized0 =
      UnsafeRowSerializer::serialize<IntegerType>(intVector, buffer_, 0);
  int intVal0 = 0x01010101;
  EXPECT_TRUE(checkFixedLength(intSerialized0, 0, &intVal0));

  auto intSerialized1 =
      UnsafeRowDynamicSerializer::serialize(INTEGER(), intVector, buffer_, 3);
  int intVal1 = 0x01234567;
  EXPECT_TRUE(checkFixedLength(intSerialized1, 0, &intVal1));

  // Test set null.
  intVector->setNull(2, true);
  auto nullSerialized =
      UnsafeRowSerializer::serialize<IntegerType>(intVector, buffer_, 2);
  EXPECT_TRUE(checkIsNull(nullSerialized));
}

TEST_F(UnsafeRowSerializerTests, StringsDynamic) {
  bool nulls[4] = {false, false, true, false};
  StringView elements[4] = {
      StringView("Hello, World!", 13),
      StringView(u8"", 0),
      StringView(),
      StringView("INLINE", 6)};
  auto stringVec =
      makeFlatVectorPtr<StringView>(4, VARCHAR(), pool_.get(), nulls, elements);

  auto serialized0 =
      UnsafeRowDynamicSerializer::serialize(VARCHAR(), stringVec, buffer_, 0);
  EXPECT_TRUE(checkVariableLength(serialized0, 13, u8"Hello, World!"));

  auto serialized1 =
      UnsafeRowDynamicSerializer::serialize(VARBINARY(), stringVec, buffer_, 1);
  EXPECT_TRUE(checkVariableLength(serialized1, 0, u8""));

  auto serialized2 =
      UnsafeRowSerializer::serialize<VarcharType>(stringVec, buffer_, 2);
  EXPECT_TRUE(checkIsNull(serialized2));

  // velox::StringView inlines string prefix, check that we can handle inlining.
  auto serialized3 =
      UnsafeRowSerializer::serialize<VarcharType>(stringVec, buffer_, 3);
  EXPECT_TRUE(checkVariableLength(serialized3, 6, u8"INLINE"));
}

TEST_F(UnsafeRowSerializerTests, timestamp) {
  bool nulls[2] = {false, true};
  Timestamp elements[2] = {Timestamp(1, 2'000), Timestamp(0, 0)};
  auto timestampVec = makeFlatVectorPtr<Timestamp>(
      2, TIMESTAMP(), pool_.get(), nulls, elements);

  auto serialized0 = UnsafeRowDynamicSerializer::serialize(
      TIMESTAMP(), timestampVec, buffer_, 0);
  int64_t expected0 = 1'000'000 + 2; // 1s + 2000ns in micros.
  EXPECT_TRUE(checkFixedLength(serialized0, 0, &expected0));

  auto serialized1 =
      UnsafeRowSerializer::serialize<TimestampType>(timestampVec, buffer_, 1);
  EXPECT_TRUE(checkIsNull(serialized1));

  auto timestamp = Timestamp(-1, 2'000);
  auto serialized3 =
      UnsafeRowDynamicSerializer::serialize(TIMESTAMP(), timestamp, buffer_);
  int64_t expected3 = -1'000'000L + 2;
  EXPECT_TRUE(checkFixedLength(serialized3, 0, &expected3));
}

TEST_F(UnsafeRowSerializerTests, arrayStdContainers) {
  // [0x1666, 0x0777, null, 0x0999]

  std::array<std::optional<int16_t>, 4> array = {
      0x1666, 0x0777, std::nullopt, 0x0999};
  auto optionalArray = std::optional(array);
  auto serialized = UnsafeRowSerializer::serialize<Array<SmallintType>>(
      optionalArray, buffer_);

  uint8_t expected[3][8] = {
      {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x16, 0x77, 0x07, 0x00, 0x00, 0x99, 0x09}};
  EXPECT_TRUE(checkVariableLength(serialized, 3 * 8, *expected));
  // The third element (idx 2) is null.
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 2));
  clearBuffer();

  //   [ [5, 6, 7], null, [8] ]
  uint8_t expectedNested[11][8] = {
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x05, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

  std::vector<std::optional<int8_t>> element0 = {0x5, 0x6, 0x7};
  std::vector<std::optional<int8_t>> element2 = {0x8};
  auto optionalElement0 = std::optional(element0);
  auto optionalElement1 = std::optional(element2);
  std::vector<decltype(optionalElement0)> nestedArray = {
      element0, std::nullopt, element2};

  auto serializedNested =
      UnsafeRowSerializer::serialize<Array<Array<TinyintType>>>(
          nestedArray, buffer_);
  EXPECT_TRUE(checkVariableLength(serializedNested, 11 * 8, *expectedNested));
  clearBuffer();
}

TEST_F(UnsafeRowSerializerTests, mapStdContainers) {
  /// {1 : { 2 : 3, 4: null},
  ///     6: {7: 8}}

  using SmallintSmallintMap =
      std::multimap<std::optional<int16_t>, std::optional<int16_t>>;
  SmallintSmallintMap innermap0 = {{2, 3}, {4, std::nullopt}};
  SmallintSmallintMap innermap1 = {{7, 8}};

  std::multimap<std::optional<int16_t>, std::optional<SmallintSmallintMap>>
      outermap = {{1, innermap0}, {6, innermap1}};

  // { 2 : 3, 4: null}
  auto serialized0 =
      UnsafeRowSerializer::serialize<Map<SmallintType, SmallintType>>(
          innermap0, buffer_);
  uint8_t expected0[7][8] = {
      {0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(checkVariableLength(serialized0, 7 * 8, *expected0));
  clearBuffer();

  // {7: 8}
  auto serialized1 =
      UnsafeRowSerializer::serialize<Map<SmallintType, SmallintType>>(
          innermap1, buffer_);
  uint8_t expected1[7][8] = {
      {0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(checkVariableLength(serialized1, 7 * 8, *expected1));
  clearBuffer();

  /// {1 : { 2 : 3, 4: null},
  ///     6: {7: 8}}
  auto serialized = UnsafeRowSerializer::serialize<
      Map<SmallintType, Map<SmallintType, SmallintType>>>(outermap, buffer_);
  uint8_t expected[22][8] = {
      {0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x38, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00},
      {0x38, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(checkVariableLength(serialized, 22 * 8, *expected));
}

TEST_F(UnsafeRowSerializerTests, arrayPrimitives) {
  /// ArrayVector<FlatVector<int16_t>>:
  /// [ null, [0x0333, 0x1444, 0x0555], [0x1666, 0x0777, null, 0x0999] ]
  /// size: 3
  /// offsets: [0, 0, 3]
  /// lengths: [0, 3, 4]
  /// nulls: 0b001
  /// elements:
  ///  FlatVector<int16_t>:
  ///  size: 7
  ///  [0x0333, 0x1444, 0x0555, 0x1666, 0x0777, null, 0x0999]
  ///  nulls: 0b0100000

  size_t flatVectorSize = 7;
  bool flatVectorNulls[7] = {false, false, false, false, false, true, false};
  int16_t flatVectorValues[7] = {
      0x0333, 0x1444, 0x0555, 0x1666, 0x0777, 0x0000, 0x0999};
  auto flatVector = makeFlatVectorPtr<int16_t>(
      flatVectorSize,
      SMALLINT(),
      pool_.get(),
      flatVectorNulls,
      flatVectorValues);

  size_t arrayVectorSize = 3;
  bool nullsValue[3] = {1, 0, 0};
  int32_t offsetsValue[3] = {0, 0, 3};
  vector_size_t lengthsValue[3] = {0, 3, 4};
  auto arrayVector = makeArrayVectorPtr(
      arrayVectorSize,
      pool_.get(),
      offsetsValue,
      lengthsValue,
      nullsValue,
      ARRAY(SMALLINT()),
      flatVector);

  // null
  auto serialized0 =
      UnsafeRowSerializer::serializeComplexVectors<Array<SmallintType>>(
          arrayVector, buffer_, 0);
  EXPECT_TRUE(checkIsNull(serialized0));
  clearBuffer();

  auto dynamic0 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(SMALLINT()), arrayVector, buffer_, 0);
  EXPECT_TRUE(checkIsNull(dynamic0));
  clearBuffer();

  // [0x0333, 0x1444, 0x0555]
  auto serialized1 =
      UnsafeRowSerializer::serializeComplexVectors<Array<SmallintType>>(
          arrayVector, buffer_, 1);
  uint8_t expected1[3][8] = {
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x33, 0x03, 0x44, 0x14, 0x55, 0x05, 0x00, 0x00}};
  EXPECT_TRUE(checkVariableLength(serialized1, 3 * 8, *expected1));
  clearBuffer();

  auto dynamic1 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(SMALLINT()), arrayVector, buffer_, 1);
  EXPECT_TRUE(checkVariableLength(dynamic1, 3 * 8, *expected1));
  clearBuffer();

  // [0x1666, 0x0777, null, 0x0999]
  auto serialized2 =
      UnsafeRowSerializer::serializeComplexVectors<Array<SmallintType>>(
          arrayVector, buffer_, 2);
  uint8_t expected2[3][8] = {
      {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x16, 0x77, 0x07, 0x00, 0x00, 0x99, 0x09}};
  EXPECT_TRUE(checkVariableLength(serialized2, 3 * 8, *expected2));
  // third element (idx 2) is null
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 2));
  clearBuffer();

  auto dynamic2 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(SMALLINT()), arrayVector, buffer_, 2);
  EXPECT_TRUE(checkVariableLength(dynamic2, 3 * 8, *expected2));
  // third element (idx 2) is null
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 2));
  clearBuffer();
}

TEST_F(UnsafeRowSerializerTests, arrayStringView) {
  /// ArrayVector<FlatVector<StringView>>:
  /// [ hello, longString, emptyString, null ], [null, world], null]
  /// size: 3
  /// offsets: [0, 4, 6]
  /// lengths: [4, 2, 0]
  /// nulls: 0b100
  /// elements:
  ///  FlatVector<StringView>:
  ///  size: 6
  ///  [ hello, longString, emptyString, null, null, world]
  ///  nulls: 0b011000

  auto hello = StringView(u8"Hello", 5);
  auto longString =
      StringView(u8"This is a rather long string.  Quite long indeed.", 49);
  auto emptyString = StringView(u8"", 0);
  auto world = StringView(u8"World", 5);
  auto placeHolder = StringView();

  size_t flatVectorSize = 6;
  bool flatVectorNulls[6] = {false, false, false, true, true, false};
  StringView flatVectorValues[6] = {
      hello, longString, emptyString, placeHolder, placeHolder, world};
  auto flatVector = makeFlatVectorPtr<StringView>(
      flatVectorSize,
      VARCHAR(),
      pool_.get(),
      flatVectorNulls,
      flatVectorValues);

  size_t arrayVectorSize = 3;
  bool nullsValue[3] = {false, false, true};
  int32_t offsetsValue[3] = {0, 4, 6};
  vector_size_t lengthsValue[3] = {4, 2, 0};
  auto arrayVector = makeArrayVectorPtr(
      arrayVectorSize,
      pool_.get(),
      offsetsValue,
      lengthsValue,
      nullsValue,
      ARRAY(VARCHAR()),
      flatVector);

  // [ hello, longString, emptyString, null ]
  auto serialized0 =
      UnsafeRowSerializer::serializeComplexVectors<Array<VarcharType>>(
          arrayVector, buffer_, 0);
  uint8_t expected0[14][8] = {
      {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x05, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00},
      {0x31, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00},
      {0x54, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20},
      {0x61, 0x20, 0x72, 0x61, 0x74, 0x68, 0x65, 0x72},
      {0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x73, 0x74},
      {0x72, 0x69, 0x6e, 0x67, 0x2e, 0x20, 0x20, 0x51},
      {0x75, 0x69, 0x74, 0x65, 0x20, 0x6c, 0x6f, 0x6e},
      {0x67, 0x20, 0x69, 0x6e, 0x64, 0x65, 0x65, 0x64},
      {0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

  EXPECT_TRUE(checkVariableLength(serialized0, 14 * 8, *expected0));
  // forth element (idx 3) is null
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 3));
  clearBuffer();

  auto dynamic0 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(VARCHAR()), arrayVector, buffer_, 0);
  EXPECT_TRUE(checkVariableLength(dynamic0, 14 * 8, *expected0));
  // forth element (idx 3) is null
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 3));
  clearBuffer();

  // [null, world]
  auto serialized1 =
      UnsafeRowSerializer::serializeComplexVectors<Array<VarcharType>>(
          arrayVector, buffer_, 1);
  uint8_t expected1[5][8] = {
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x05, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00},
      {0x57, 0x6f, 0x72, 0x6c, 0x64, 0x00, 0x00, 0x00}};

  EXPECT_TRUE(checkVariableLength(serialized1, 5 * 8, *expected1));
  // first element (idx 0) is null
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 0));
  clearBuffer();

  auto dynamic1 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(VARCHAR()), arrayVector, buffer_, 1);
  EXPECT_TRUE(checkVariableLength(dynamic1, 5 * 8, *expected1));
  // first element (idx 0) is null
  ASSERT_TRUE(bits::isBitSet(buffer_ + 8, 0));
  clearBuffer();

  // null
  auto serialized2 =
      UnsafeRowSerializer::serializeComplexVectors<Array<VarcharType>>(
          arrayVector, buffer_, 2);
  EXPECT_TRUE(checkIsNull(serialized2));
  clearBuffer();

  auto dynamic2 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(VARCHAR()), arrayVector, buffer_, 2);
  EXPECT_TRUE(checkIsNull(dynamic2));
  clearBuffer();
}

TEST_F(UnsafeRowSerializerTests, nestedArray) {
  /// ArrayVector<ArrayVector<FlatVector<int8_t>>>
  /// [
  ///  [
  ///    [1, 2], [3, 4]
  ///   ],
  ///  [
  ///    [5, 6, 7], null, [8]
  ///   ],
  ///  [
  ///    [9, 10]
  ///   ],
  /// ]
  /// size: 3
  /// offsets: [0, 2, 5]
  /// lengths: [2, 3, 1]
  /// nullCount: 0
  /// // [1, 2,], [3, 4], [5, 6, 7], null, [8], [9, 10]
  /// ArrayVector<FlatVector<int8_t>>[0] == ArrayVector<FlatVector<int8_t>>[1]
  ///  size: 6
  ///  offsets: [0, 2, 4, 7, 7, 8]
  ///  lengths: [2, 2, 3, 0, 1, 2]
  ///  nulls: 0b001000
  ///  nullCount: 1
  ///  FlatVector<int8_t>
  ///    [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
  ///    size: 10
  ///    nullCount: 0

  size_t flatVectorSize = 10;
  bool flatVectorNulls[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int8_t flatVectorValues[10] = {
      0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0x10};
  auto flatVector = makeFlatVectorPtr<int8_t>(
      flatVectorSize,
      TINYINT(),
      pool_.get(),
      flatVectorNulls,
      flatVectorValues);

  size_t arrayVectorSize = 6;
  bool arrayNullsValue[6] = {0, 0, 0, 1, 0, 0};
  int32_t arrayOffsetsValue[6] = {0, 2, 4, 7, 7, 8};
  vector_size_t arrayLengthsValue[6] = {2, 2, 3, 0, 1, 2};
  auto arrayVector = makeArrayVectorPtr(
      arrayVectorSize,
      pool_.get(),
      arrayOffsetsValue,
      arrayLengthsValue,
      arrayNullsValue,
      ARRAY(TINYINT()),
      flatVector);

  size_t arrayArrayVectorSize = 3;
  bool arrayArrayNullsValue[3] = {0, 0, 0};
  int32_t arrayArrayOffsetsValue[3] = {0, 2, 5};
  vector_size_t arrayArrayLengthsValue[3] = {2, 3, 1};
  auto arrayArrayVector = makeArrayVectorPtr(
      arrayArrayVectorSize,
      pool_.get(),
      arrayArrayOffsetsValue,
      arrayArrayLengthsValue,
      arrayArrayNullsValue,
      ARRAY(ARRAY(TINYINT())),
      arrayVector);

  // [ [1, 2], [3, 4] ]
  auto serialized0 =
      UnsafeRowSerializer::serializeComplexVectors<Array<Array<TinyintType>>>(
          arrayArrayVector, buffer_, 0);
  uint8_t expected0[10][8] = {
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

  EXPECT_TRUE(checkVariableLength(serialized0, 10 * 8, *expected0));
  clearBuffer();

  auto dynamic0 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(ARRAY(TINYINT())), arrayArrayVector, buffer_, 0);
  EXPECT_TRUE(checkVariableLength(dynamic0, 10 * 8, *expected0));
  clearBuffer();

  //   [ [5, 6, 7], null, [8] ]
  auto serialized1 =
      UnsafeRowSerializer::serializeComplexVectors<Array<Array<TinyintType>>>(
          arrayArrayVector, buffer_, 1);
  uint8_t expected1[11][8] = {
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x05, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

  EXPECT_TRUE(checkVariableLength(serialized1, 11 * 8, *expected1));
  clearBuffer();

  auto dynamic1 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(ARRAY(TINYINT())), arrayArrayVector, buffer_, 1);
  EXPECT_TRUE(checkVariableLength(dynamic1, 11 * 8, *expected1));
  clearBuffer();

  // [ [9, 10] ]
  auto serialized2 =
      UnsafeRowSerializer::serializeComplexVectors<Array<Array<TinyintType>>>(
          arrayArrayVector, buffer_, 2);
  uint8_t expected2[6][8] = {
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x09, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

  EXPECT_TRUE(checkVariableLength(serialized2, 6 * 8, expected2));
  clearBuffer();

  auto dynamic2 = UnsafeRowDynamicSerializer::serialize(
      ARRAY(ARRAY(TINYINT())), arrayArrayVector, buffer_, 2);
  EXPECT_TRUE(checkVariableLength(dynamic2, 6 * 8, *expected2));
  clearBuffer();
}

TEST_F(UnsafeRowSerializerTests, map) {
  /// [{
  ///  hello: [0x11, 0x22]
  ///  world: [null, null, null]
  ///  null: [0x33]
  /// },
  /// null,
  /// {
  /// hello: [0x44]
  /// }
  ///
  /// MapVector:
  ///  keys: FlatVector<VARCHAR>
  ///  values: ArrayVector<FlatVector<TINYINT>>
  ///  size: 3
  ///  offsets: [0, 3, 3]
  ///  lengths: [3, 0, 1]
  ///  nulls: 0b010
  ///
  /// keys: FlatVector<VARCHAR>:
  ///  [hello, world, null, hello]
  ///  size: 4
  ///  nulls: 0b0100
  ///
  /// values: ArrayVector<FlatVector<TINYINT>>
  ///  [ [0x11, 0x22], [null, null, null], [0x33], [0x44] ]
  ///  size: 4
  ///  offsets: [0, 2, 5, 6]
  ///  lengths: [2, 3, 1, 1]
  ///  nulls: 0b0000
  ///  FlatVector<TINYINT>:
  ///    [0x11, 0x22, null, null, null, 0x33, 0x44]
  ///    size: 7
  ///    nulls: 0b00111000

  auto hello = StringView(u8"Hello", 5);
  auto world = StringView(u8"World", 5);
  auto placeHolder = StringView();

  size_t keysFlatVectorSize = 4;
  bool keysFlatVectorNulls[4] = {false, false, true, false};
  StringView keysFlatVectorValues[4] = {hello, world, placeHolder, hello};
  auto keysFlatVector = makeFlatVectorPtr<StringView>(
      keysFlatVectorSize,
      VARCHAR(),
      pool_.get(),
      keysFlatVectorNulls,
      keysFlatVectorValues);

  size_t valuesFlatVectorSize = 7;
  bool valuesFlatVectorNulls[7] = {
      false, false, true, true, true, false, false};
  int8_t valuesFlatVectorValues[7] = {0x11, 0x22, 0x00, 0x00, 0x00, 0x33, 0x44};
  auto valuesFlatVector = makeFlatVectorPtr<int8_t>(
      valuesFlatVectorSize,
      TINYINT(),
      pool_.get(),
      valuesFlatVectorNulls,
      valuesFlatVectorValues);

  size_t valuesArrayVectorSize = 4;
  bool valuesNullsValue[4] = {false, false, false, false};
  int32_t valuesOffsetsValue[4] = {0, 2, 5, 6};
  vector_size_t valuesLengthsValue[4] = {2, 3, 1, 1};
  auto valuesArrayVector = makeArrayVectorPtr(
      valuesArrayVectorSize,
      pool_.get(),
      valuesOffsetsValue,
      valuesLengthsValue,
      valuesNullsValue,
      ARRAY(TINYINT()),
      valuesFlatVector);

  size_t mapVectorSize = 3;
  bool mapNullsValue[3] = {false, true, false};
  int32_t mapOffsetsValue[3] = {0, 3, 3};
  vector_size_t mapLengthsValue[3] = {3, 0, 1};
  auto mapVector = makeMapVectorPtr(
      mapVectorSize,
      pool_.get(),
      mapOffsetsValue,
      mapLengthsValue,
      mapNullsValue,
      MAP(VARCHAR(), ARRAY(TINYINT())), // MAP(VARCHAR(), ARRAY(TINYINT()))
      keysFlatVector,
      valuesArrayVector); // valuesArrayVector

  /// {
  ///  hello: [0x11, 0x22]
  ///  world: [null, null, null]
  ///  null: [0x33]
  /// }
  auto serialized0 = UnsafeRowSerializer::serializeComplexVectors<
      Map<VarcharType, Array<TinyintType>>>(mapVector, buffer_, 0);
  uint8_t expected0[22][8] = {
      {0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x05, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00},
      {0x05, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00},
      {0x57, 0x6f, 0x72, 0x6c, 0x64, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00},
      {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x11, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(checkVariableLength(serialized0, 22 * 8, *expected0));
  clearBuffer();

  auto dynamic0 = UnsafeRowDynamicSerializer::serialize(
      MAP(VARCHAR(), ARRAY(TINYINT())), mapVector, buffer_, 0);
  EXPECT_TRUE(checkVariableLength(dynamic0, 22 * 8, *expected0));
  clearBuffer();

  // null
  auto serialized1 = UnsafeRowSerializer::serializeComplexVectors<
      Map<VarcharType, Array<TinyintType>>>(mapVector, buffer_, 1);
  EXPECT_TRUE(checkIsNull(serialized1));
  clearBuffer();

  auto dynamic1 = UnsafeRowDynamicSerializer::serialize(
      MAP(VARCHAR(), ARRAY(TINYINT())), mapVector, buffer_, 1);
  EXPECT_TRUE(checkIsNull(dynamic1));
  clearBuffer();

  /// {
  /// hello: [0x44]
  /// }
  auto serialized2 = UnsafeRowSerializer::serializeComplexVectors<
      Map<VarcharType, Array<TinyintType>>>(mapVector, buffer_, 2);
  uint8_t expected2[11][8] = {
      {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x05, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
      {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x18, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(checkVariableLength(serialized2, 11 * 8, *expected2));
  clearBuffer();

  auto dynamic2 = UnsafeRowDynamicSerializer::serialize(
      MAP(VARCHAR(), ARRAY(TINYINT())), mapVector, buffer_, 2);
  EXPECT_TRUE(checkVariableLength(dynamic2, 11 * 8, *expected2));
  clearBuffer();
}

TEST_F(UnsafeRowSerializerTests, rowFixedLength) {
  bool nulls0[5] = {false, true, false, false, false};
  int64_t elements0[5] = {
      0x0101010101010101,
      0x0101010101010101,
      0x0101010101010101,
      0x0123456789ABCDEF,
      0x1111111111111111};
  auto c0 =
      makeFlatVectorPtr<int64_t>(5, BIGINT(), pool_.get(), nulls0, elements0);

  bool nulls1[5] = {true, false, false, true, false};
  int32_t elements1[5] = {
      0x00C0C0C0, 0x0FFFFFFF, 0x0AAAAAAA, 0x0BBBBBBB, 0x10101010};
  auto c1 =
      makeFlatVectorPtr<int32_t>(5, INTEGER(), pool_.get(), nulls1, elements1);

  bool nulls2[5] = {false, false, false, false, true};
  int16_t elements2[5] = {0x1111, 0x00FF, 0x7E00, 0x1234, 0x0101};
  auto c2 =
      makeFlatVectorPtr<int16_t>(5, SMALLINT(), pool_.get(), nulls2, elements2);

  auto c3 = vectorMaker_->constantVector<int32_t>(
      std::vector<std::optional<int32_t>>(5, 0x22222222));

  auto c4 = vectorMaker_->constantVector<int32_t>(
      std::vector<std::optional<int32_t>>(5, std::nullopt));

  auto c5 = vectorMaker_->constantVector<Timestamp>(
      std::vector<std::optional<Timestamp>>(5, Timestamp(0, 0xFF * 1000)));

  auto c6 = vectorMaker_->constantVector<Timestamp>(
      std::vector<std::optional<Timestamp>>(5, std::nullopt));

  auto rowVector = vectorMaker_->rowVector({c0, c1, c2, c3, c4, c5, c6});

  // row[0], 0b1010010
  // {0x0101010101010101, null, 0x1111, 0x22222222, null, 0xFF, null}
  uint8_t expected0[8][8] = {
      {0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  auto bytes0 = UnsafeRowDynamicSerializer::serialize(
      ROW(
          {BIGINT(),
           INTEGER(),
           SMALLINT(),
           INTEGER(),
           INTEGER(),
           TIMESTAMP(),
           TIMESTAMP()}),
      rowVector,
      buffer_,
      0);
  EXPECT_TRUE(checkVariableLength(bytes0, 8 * 8, *expected0));
  clearBuffer();

  // row[1], 0b1010001
  // {null, 0x0FFFFFFF, 0x00FF, 0x22222222, null, 0xFF, null}
  uint8_t expected1[8][8] = {
      {0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  auto bytes1 = UnsafeRowDynamicSerializer::serialize(
      ROW(
          {BIGINT(),
           INTEGER(),
           SMALLINT(),
           INTEGER(),
           INTEGER(),
           TIMESTAMP(),
           TIMESTAMP()}),
      rowVector,
      buffer_,
      1);
  EXPECT_TRUE(checkVariableLength(bytes1, 8 * 8, *expected1));
  clearBuffer();

  // row[2], 0b1010000
  // {0x0101010101010101, 0x0AAAAAAA, 0x7E00, 0x22222222, null, 0xFF, null}
  uint8_t expected2[8][8] = {
      {0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
      {0xAA, 0xAA, 0xAA, 0x0A, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  auto bytes2 = UnsafeRowDynamicSerializer::serialize(
      ROW(
          {BIGINT(),
           INTEGER(),
           SMALLINT(),
           INTEGER(),
           INTEGER(),
           TIMESTAMP(),
           TIMESTAMP()}),
      rowVector,
      buffer_,
      2);
  EXPECT_TRUE(checkVariableLength(bytes2, 8 * 8, *expected2));
  clearBuffer();

  // row[3], 0b1010010
  // {0x0123456789ABCDEF, null, 0x1234, 0x22222222, null, 0xFF, null}
  uint8_t expected3[8][8] = {
      {0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  auto bytes3 = UnsafeRowDynamicSerializer::serialize(
      ROW(
          {BIGINT(),
           INTEGER(),
           SMALLINT(),
           INTEGER(),
           INTEGER(),
           TIMESTAMP(),
           TIMESTAMP()}),
      rowVector,
      buffer_,
      3);
  EXPECT_TRUE(checkVariableLength(bytes3, 8 * 8, *expected3));
  clearBuffer();

  // row[4], 0b1010100
  // {0x1111111111111111, 0x10101010, null, 0x22222222, null, 0xFF, null}
  uint8_t expected4[8][8] = {
      {0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
      {0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  auto bytes4 = UnsafeRowDynamicSerializer::serialize(
      ROW(
          {BIGINT(),
           INTEGER(),
           SMALLINT(),
           INTEGER(),
           INTEGER(),
           TIMESTAMP(),
           TIMESTAMP()}),
      rowVector,
      buffer_,
      4);
  EXPECT_TRUE(checkVariableLength(bytes4, 8 * 8, *expected4));
}

TEST_F(UnsafeRowSerializerTests, rowVarLength) {
  /*
   * The StringView class reserves a 12-bytes space for inlined string,
   * logically separating into a 4-bytes prefix_ and a 8-bytes union value_.
   * If a string is less than 12 bytes, it is entirely copied into this reserved
   * space. Otherwise, the first 4 bytes of the string is copied into prefix_
   * and union value_ stores the pointer to the string (not 4-bytes after the
   * beginning of the string).
   * Function begin() returns prefix_ when inlined and value_ when not, so the
   * string doesn't get truncated.
   */
  bool nulls0[2] = {false, true};
  int64_t elements0[2] = {0x0101010101010101, 0x0101010101010101};
  auto c0 =
      makeFlatVectorPtr<int64_t>(2, BIGINT(), pool_.get(), nulls0, elements0);

  bool nulls1[2] = {true, false};
  StringView elements1[2] = {StringView("abcd"), StringView("Hello World!")};
  auto c1 = makeFlatVectorPtr<StringView>(
      2, VARCHAR(), pool_.get(), nulls1, elements1);

  bool nulls2[2] = {false, false};
  int64_t elements2[2] = {0xABCDEF, 0xAAAAAAAAAA};
  auto c2 =
      makeFlatVectorPtr<int64_t>(2, BIGINT(), pool_.get(), nulls2, elements2);

  auto c3 = vectorMaker_->constantVector<StringView>(
      std::vector<std::optional<StringView>>(2, StringView("1234")));

  auto c4 = vectorMaker_->constantVector<StringView>(
      std::vector<std::optional<StringView>>(2, std::nullopt));

  bool nulls5[2] = {false, false};
  StringView elements5[2] = {
      StringView("Im a string with 30 characters"),
      StringView("Pero yo tengo veinte")};
  auto c5 = makeFlatVectorPtr<StringView>(
      2, VARCHAR(), pool_.get(), nulls5, elements5);

  auto rowVector = vectorMaker_->rowVector({c0, c1, c2, c3, c4, c5});

  // row[0], 0b010010
  // {0x0101010101010101, null, 0xABCDEF, 56llu << 32 | 4, null, 64llu << 32 |
  // 30, "1234", "Im a string with 30 characters"}
  uint8_t expected0[12][8] = {
      {0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0xEF, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x04, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x1E, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00},
      {'1', '2', '3', '4', 0x00, 0x00, 0x00, 0x00},
      {'I', 'm', ' ', 'a', ' ', 's', 't', 'r'},
      {'i', 'n', 'g', ' ', 'w', 'i', 't', 'h'},
      {' ', '3', '0', ' ', 'c', 'h', 'a', 'r'},
      {'a', 'c', 't', 'e', 'r', 's', 0x00, 0x00},
  };
  auto bytes0 = UnsafeRowDynamicSerializer::serialize(
      ROW({BIGINT(), VARCHAR(), BIGINT(), VARCHAR(), VARCHAR(), VARCHAR()}),
      rowVector,
      buffer_,
      0);
  EXPECT_TRUE(checkVariableLength(bytes0, 12 * 8 - 2, *expected0));
  clearBuffer();

  // row[1], 0b010001
  // {null, 56llu << 32 | 12, 0xAAAAAAAAAA, 72llu << 32 | 4, null, 80llu << 32 |
  // 20, "Hello World!", "1234", "Pero yo tengo veinte"}
  uint8_t expected1[13][8] = {
      {0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x0C, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00},
      {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x00, 0x00, 0x00},
      {0x04, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x14, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00},
      {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o'},
      {'r', 'l', 'd', '!', 0x00, 0x00, 0x00, 0x00},
      {'1', '2', '3', '4', 0x00, 0x00, 0x00, 0x00},
      {'P', 'e', 'r', 'o', ' ', 'y', 'o', ' '},
      {'t', 'e', 'n', 'g', 'o', ' ', 'v', 'e'},
      {'i', 'n', 't', 'e', 0x00, 0x00, 0x00, 0x00},
  };
  auto bytes1 = UnsafeRowDynamicSerializer::serialize(
      ROW({BIGINT(), VARCHAR(), BIGINT(), VARCHAR(), VARCHAR(), VARCHAR()}),
      rowVector,
      buffer_,
      1);
  EXPECT_TRUE(checkVariableLength(bytes1, 13 * 8 - 4, *expected1));
}

TEST_F(UnsafeRowSerializerTests, LazyVector) {
  VectorPtr lazyVector0 = vectorMaker_->lazyFlatVector<StringView>(
      1, [](vector_size_t i) { return StringView("Hello, World!", 13); });

  auto serialized0 =
      UnsafeRowDynamicSerializer::serialize(VARCHAR(), lazyVector0, buffer_, 0);
  EXPECT_TRUE(checkVariableLength(serialized0, 13, u8"Hello, World!"));

  VectorPtr lazyVector1 = vectorMaker_->lazyFlatVector<Timestamp>(
      1, [](vector_size_t i) { return Timestamp(2, 1'000); });

  auto serialized1 = UnsafeRowDynamicSerializer::serialize(
      TIMESTAMP(), lazyVector1, buffer_, 0);
  int64_t expected1 = 2'000'001;
  EXPECT_TRUE(checkFixedLength(serialized1, 0, &expected1));

  VectorPtr lazyVector2 = vectorMaker_->lazyFlatVector<int32_t>(
      1, [](vector_size_t i) { return 0x01010101; });

  auto serialized2 =
      UnsafeRowSerializer::serialize<IntegerType>(lazyVector2, buffer_, 0);
  int intVal = 0x01010101;
  EXPECT_TRUE(checkFixedLength(serialized2, 0, &intVal));
}

} // namespace facebook::velox::row
