# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_library(velox_codegen Codegen.cpp)
if(${CODEGEN_SUPPORT})
  target_link_libraries(velox_codegen velox_experimental_codegen)
else()
  target_link_libraries(velox_codegen velox_core velox_exec velox_expression
                        ${FOLLY_WITH_DEPENDENCIES})
endif()
if(${VELOX_BUILD_TESTING})
  add_subdirectory(tests)
endif()
