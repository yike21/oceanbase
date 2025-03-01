/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX LIB

#include "thread.h"
#include <pthread.h>
#include <sys/syscall.h>
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log.h"
#include "lib/ob_running_mode.h"
#include "lib/allocator/ob_page_manager.h"
#include "lib/rc/context.h"
#include "lib/thread_local/ob_tsi_factory.h"
#include "lib/thread/protected_stack_allocator.h"
#include "lib/utility/ob_defer.h"
#include "lib/utility/ob_hang_fatal_error.h"
#include "lib/signal/ob_signal_struct.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::lib;

TLOCAL(Thread *, Thread::current_thread_) = nullptr;
int64_t Thread::total_thread_count_ = 0;

Thread &Thread::current()
{
  assert(current_thread_ != nullptr);
  return *current_thread_;
}

Thread::Thread()
    : Thread(nullptr)
{}

Thread::Thread(int64_t stack_size)
    : Thread(nullptr, stack_size)
{}

Thread::Thread(Runnable runnable, int64_t stack_size)
    : pth_(0),
      pid_(0),
      tid_(0),
      runnable_(runnable),
#ifndef OB_USE_ASAN
      stack_addr_(nullptr),
#endif
      stack_size_(stack_size),
      stop_(true)
{}

Thread::~Thread()
{
  destroy();
}

int Thread::start()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(runnable_)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const int64_t count = ATOMIC_FAA(&total_thread_count_, 1);
    if (count >= get_max_thread_num() - OB_RESERVED_THREAD_NUM) {
      ATOMIC_FAA(&total_thread_count_, -1);
      ret = OB_SIZE_OVERFLOW;
      LOG_ERROR("thread count reach limit", K(ret), "current count", count);
    } else if (stack_size_ <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("invalid stack_size", K(ret), K(stack_size_));
#ifndef OB_USE_ASAN
    } else if (OB_ISNULL(stack_addr_ = g_stack_allocer.alloc(0 == GET_TENANT_ID() ? OB_SERVER_TENANT_ID : GET_TENANT_ID(), stack_size_ + SIG_STACK_SIZE))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc stack memory failed", K(stack_size_));
#endif
    } else {
      pthread_attr_t attr;
      bool need_destroy = false;
      int pret = pthread_attr_init(&attr);
      if (pret == 0) {
        need_destroy = true;
#ifndef OB_USE_ASAN
        pret = pthread_attr_setstack(&attr, stack_addr_, stack_size_);
#endif
      }
      if (pret == 0) {
        stop_ = false;
        pret = pthread_create(&pth_, &attr, __th_start, this);
        if (pret != 0) {
          LOG_ERROR("pthread create failed", K(pret), K(errno));
          pth_ = 0;
        }
      }
      if (0 != pret) {
        ATOMIC_FAA(&total_thread_count_, -1);
        ret = OB_ERR_SYS;
        stop_ = true;
      }
      if (need_destroy) {
        pthread_attr_destroy(&attr);
      }
    }
  }
  if (OB_FAIL(ret)) {
    destroy();
  }
  return ret;
}

int Thread::start(Runnable runnable)
{
  runnable_ = runnable;
  return start();
}

void Thread::stop()
{
  stop_ = true;
}

void Thread::wait()
{
  if (pth_ != 0) {
    pthread_join(pth_, nullptr);
    destroy_stack();
    pth_ = 0;
    pid_ = 0;
    tid_ = 0;
    runnable_ = nullptr;
  }
}

void Thread::destroy()
{
  if (pth_ != 0) {
    /* NOTE: must wait pthread quit before release user_stack
       because the pthread's tcb was allocated from it */
    wait();
  } else {
    destroy_stack();
  }
}

void Thread::destroy_stack()
{

#ifndef OB_USE_ASAN
  if (stack_addr_ != nullptr) {
    g_stack_allocer.dealloc(stack_addr_);
    stack_addr_ = nullptr;
  }
#endif
}

void* Thread::__th_start(void *arg)
{
  Thread * const th = reinterpret_cast<Thread*>(arg);
  current_thread_ = th;
#ifndef OB_USE_ASAN
  ObStackHeader *stack_header = ProtectedStackAllocator::stack_header(th->stack_addr_);
  abort_unless(stack_header->check_magic());

  #ifndef OB_USE_ASAN
  /**
    signal handler stack
   */
  stack_t nss;
  stack_t oss;
  bzero(&nss, sizeof(nss));
  bzero(&oss, sizeof(oss));
  nss.ss_sp = &((char*)th->stack_addr_)[th->stack_size_];
  nss.ss_size = SIG_STACK_SIZE;
  bool restore_sigstack = false;
  if (-1 == sigaltstack(&nss, &oss)) {
    LOG_WARN("sigaltstack failed, ignore it", K(errno));
  } else {
    restore_sigstack = true;
  }
  DEFER(if (restore_sigstack) { sigaltstack(&oss, nullptr); });
  #endif

  stack_header->pth_ = (uint64_t)pthread_self();
#endif

  int ret = OB_SUCCESS;
  if (OB_ISNULL(th)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_ERROR("invalid argument", K(th), K(ret));
  } else {
    // pm析构逻辑上会访问其它pthread_key绑定的对象，为了避免析构顺序的影响
    // pm不用TSI而是自己做线程局部(__thread)
    // create page manager
    ObPageManager pm;
    ret = pm.set_tenant_ctx(common::OB_SERVER_TENANT_ID, common::ObCtxIds::GLIBC);
    if (OB_FAIL(ret)) {
      LOG_ERROR("set tenant ctx failed", K(ret));
    } else {
      const int cache_cnt = !lib::is_mini_mode() ? ObPageManager::DEFAULT_CHUNK_CACHE_CNT :
        ObPageManager::MINI_MODE_CHUNK_CACHE_CNT;
      pm.set_max_chunk_cache_cnt(cache_cnt);
      ObPageManager::set_thread_local_instance(pm);
      MemoryContext *mem_context = GET_TSI0(MemoryContext);
      ret = ROOT_CONTEXT->CREATE_CONTEXT(*mem_context,
          ContextParam().set_properties(RETURN_MALLOC_DEFAULT)
                        .set_label("ThreadRoot"));
      if (OB_FAIL(ret)) {
        LOG_ERROR("create memory context failed", K(ret));
      } else if (OB_ISNULL(mem_context)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("null ptr", K(ret));
      } else {
        th->pid_ = getpid();
        th->tid_ = static_cast<pid_t>(syscall(__NR_gettid));
        WITH_CONTEXT(*mem_context) {
          try {
            in_try_stmt = true;
            th->runnable_();
            in_try_stmt = false;
          } catch (OB_BASE_EXCEPTION &except) {
            // we don't catch other exception because we don't know how to handle it
            _LOG_ERROR("Exception caught!!! errno = %d, exception info = %s", except.get_errno(), except.what());
            ret = OB_ERR_UNEXPECTED;
            in_try_stmt = false;
          }
        }
      }
      if (mem_context != nullptr && *mem_context != nullptr) {
        DESTROY_CONTEXT(*mem_context);
      }
    }
  }

  ATOMIC_FAA(&total_thread_count_, -1);
  return nullptr;
}

namespace oceanbase
{
namespace lib
{
int __attribute__((weak)) get_max_thread_num()
{
  return 4096;
}
}
}
