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
#include "pw_thread_embos/util.h"

#include "RTOS.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::thread::embos {

namespace internal {

// Iterates through all threads that haven't been deleted, calling the provided
// callback.
Status ForEachThread(const OS_TASK& starting_thread, ThreadCallback& cb) {
  if (!OS_IsRunning()) {
    return Status::FailedPrecondition();
  }

  const OS_TASK* thread = &starting_thread;
  while (thread != nullptr) {
    if (!cb(*thread)) {
      // Early-terminate iteration if requested by the callback.
      return Status::Aborted();
    }
    thread = thread->pNext;
  }

  return OkStatus();
}

}  // namespace internal

Status ForEachThread(ThreadCallback& cb) {
  return internal::ForEachThread(*OS_GetpCurrentTask(), cb);
}

}  // namespace pw::thread::embos
