// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#pragma once

#include "RTOS.h"
#include "pw_assert/assert.h"
#include "pw_interrupt/context.h"
#include "pw_sync/mutex.h"

namespace pw::sync {

inline Mutex::Mutex() : native_type_() { OS_CreateRSema(&native_type_); }

inline Mutex::~Mutex() { OS_DeleteRSema(&native_type_); }

inline void Mutex::lock() {
  PW_ASSERT(!interrupt::InInterruptContext());
  const int lock_count = OS_Use(&native_type_);
  PW_ASSERT(lock_count == 1);  // Recursive locking is not permitted.
}

inline bool Mutex::try_lock() {
  PW_ASSERT(!interrupt::InInterruptContext());
  return OS_Request(&native_type_) != 0;
}

inline void Mutex::unlock() {
  PW_ASSERT(!interrupt::InInterruptContext());
  OS_Unuse(&native_type_);
}

inline Mutex::native_handle_type Mutex::native_handle() { return native_type_; }

}  // namespace pw::sync
