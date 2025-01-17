/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/os_thread.h"

#include <thread>

namespace NEO {
class ThreadWin : public Thread {
  public:
    ThreadWin(std::thread *thread);
    void join() override;
    void yield() override;
    ~ThreadWin() override = default;

  protected:
    std::unique_ptr<std::thread> thread;
};
} // namespace NEO
