#include "context.h"
#include <v8-initialization.h>
#include <v8-local-handle.h>
#include <v8-locker.h>
#include <v8-persistent-handle.h>
#include <v8-platform.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include "binary_value.h"
#include "callback.h"
#include "cancelable_task_runner.h"
#include "code_evaluator.h"
#include "context_holder.h"
#include "heap_reporter.h"
#include "isolate_manager.h"
#include "isolate_memory_monitor.h"
#include "js_callback_maker.h"
#include "object_manipulator.h"

namespace MiniRacer {

Context::Context(v8::Platform* platform, Callback callback)
    : isolate_manager_(std::make_shared<IsolateManager>(platform)),
      isolate_object_collector_(isolate_manager_.get()),
      isolate_memory_monitor_(
          std::make_shared<IsolateMemoryMonitor>(isolate_manager_)),
      bv_factory_(
          std::make_shared<BinaryValueFactory>(&isolate_object_collector_)),
      bv_registry_(std::make_shared<BinaryValueRegistry>()),
      callback_(
          [bv_registry = bv_registry_,
           callback = callback](uint64_t callback_id, BinaryValue::Ptr val) {
            callback(callback_id, bv_registry->Remember(std::move(val)));
          }),
      context_holder_(std::make_shared<ContextHolder>(isolate_manager_)),
      js_callback_maker_(std::make_shared<JSCallbackMaker>(context_holder_,
                                                           bv_factory_,
                                                           callback_)),
      code_evaluator_(std::make_shared<CodeEvaluator>(context_holder_,
                                                      bv_factory_,
                                                      isolate_memory_monitor_)),
      heap_reporter_(std::make_shared<HeapReporter>(bv_factory_)),
      object_manipulator_(
          std::make_shared<ObjectManipulator>(context_holder_, bv_factory_)),
      cancelable_task_runner_(
          std::make_shared<CancelableTaskRunner>(isolate_manager_)) {}

auto Context::MakeJSCallback(uint64_t callback_id) -> BinaryValueHandle* {
  return bv_registry_->Remember(
      isolate_manager_->RunAndAwait([js_callback_maker = js_callback_maker_,
                                     callback_id](v8::Isolate* isolate) {
        return js_callback_maker->MakeJSCallback(isolate, callback_id);
      }));
}

template <typename Runnable>
auto Context::RunTask(Runnable runnable, uint64_t callback_id) -> uint64_t {
  // Start an async task!

  return cancelable_task_runner_->Schedule(
      /*runnable=*/
      std::move(runnable),
      /*on_completed=*/
      [callback = callback_, bv_registry = bv_registry_, callback_id](
          const BinaryValue::Ptr& val) { callback(callback_id, val); },
      /*on_canceled=*/
      [callback = callback_, callback_id, bv_factory = bv_factory_,
       bv_registry = bv_registry_](const BinaryValue::Ptr& /*val*/) {
        auto err =
            bv_factory->New("execution terminated", type_terminated_exception);
        callback(callback_id, err);
      });
}

auto Context::MakeHandleConverter(BinaryValueHandle* handle,
                                  const char* err_msg) -> ValueHandleConverter {
  return {bv_factory_, bv_registry_, handle, err_msg};
}

ValueHandleConverter::ValueHandleConverter(
    std::shared_ptr<BinaryValueFactory> bv_factory,
    const std::shared_ptr<BinaryValueRegistry>& bv_registry,
    BinaryValueHandle* handle,
    const char* err_msg)
    : bv_registry_(bv_registry),
      ptr_(bv_registry->FromHandle(handle)),
      err_([&bv_factory, err_msg, this]() {
        if (ptr_) {
          return BinaryValue::Ptr();
        }

        return bv_factory->New(err_msg, type_value_exception);
      }()) {}

ValueHandleConverter::operator bool() const {
  return static_cast<bool>(ptr_);
}

auto ValueHandleConverter::GetErrorPtr() -> BinaryValue::Ptr {
  return err_;
}

auto ValueHandleConverter::GetErrorHandle() -> BinaryValueHandle* {
  return bv_registry_->Remember(err_);
}

auto ValueHandleConverter::GetPtr() -> BinaryValue::Ptr {
  return ptr_;
}

auto Context::Eval(BinaryValueHandle* code_handle,
                   uint64_t callback_id) -> uint64_t {
  auto code_hc = MakeHandleConverter(code_handle, "Bad handle: code");
  if (!code_hc) {
    return RunTask(
        [err = code_hc.GetErrorPtr()](v8::Isolate* /*isolate*/) { return err; },
        callback_id);
  }

  return RunTask(
      [code_ptr = code_hc.GetPtr(),
       code_evaluator = code_evaluator_](v8::Isolate* isolate) {
        return code_evaluator->Eval(isolate, code_ptr.get());
      },
      callback_id);
}

void Context::CancelTask(uint64_t task_id) {
  cancelable_task_runner_->Cancel(task_id);
}

auto Context::HeapSnapshot(uint64_t callback_id) -> uint64_t {
  return RunTask(
      [heap_reporter = heap_reporter_](v8::Isolate* isolate) {
        return heap_reporter->HeapSnapshot(isolate);
      },
      callback_id);
}

auto Context::HeapStats(uint64_t callback_id) -> uint64_t {
  return RunTask(
      [heap_reporter = heap_reporter_](v8::Isolate* isolate) {
        return heap_reporter->HeapStats(isolate);
      },
      callback_id);
}

auto Context::GetIdentityHash(BinaryValueHandle* obj_handle)
    -> BinaryValueHandle* {
  auto obj_hc = MakeHandleConverter(obj_handle, "Bad handle: obj");
  if (!obj_hc) {
    return obj_hc.GetErrorHandle();
  }

  return bv_registry_->Remember(isolate_manager_->RunAndAwait(
      [object_manipulator = object_manipulator_,
       obj_ptr = obj_hc.GetPtr()](v8::Isolate* isolate) {
        return object_manipulator->GetIdentityHash(isolate, obj_ptr.get());
      }));
}

auto Context::GetOwnPropertyNames(BinaryValueHandle* obj_handle)
    -> BinaryValueHandle* {
  auto obj_hc = MakeHandleConverter(obj_handle, "Bad handle: obj");
  if (!obj_hc) {
    return obj_hc.GetErrorHandle();
  }

  return bv_registry_->Remember(isolate_manager_->RunAndAwait(
      [object_manipulator = object_manipulator_,
       obj_ptr = obj_hc.GetPtr()](v8::Isolate* isolate) {
        return object_manipulator->GetOwnPropertyNames(isolate, obj_ptr.get());
      }));
}

auto Context::GetObjectItem(BinaryValueHandle* obj_handle,
                            BinaryValueHandle* key_handle)
    -> BinaryValueHandle* {
  auto obj_hc = MakeHandleConverter(obj_handle, "Bad handle: obj");
  if (!obj_hc) {
    return obj_hc.GetErrorHandle();
  }

  auto key_hc = MakeHandleConverter(key_handle, "Bad handle: key");
  if (!key_hc) {
    return key_hc.GetErrorHandle();
  }

  return bv_registry_->Remember(isolate_manager_->RunAndAwait(
      [object_manipulator = object_manipulator_, obj_ptr = obj_hc.GetPtr(),
       key_ptr = key_hc.GetPtr()](v8::Isolate* isolate) mutable {
        return object_manipulator->Get(isolate, obj_ptr.get(), key_ptr.get());
      }));
}

auto Context::SetObjectItem(BinaryValueHandle* obj_handle,
                            BinaryValueHandle* key_handle,
                            BinaryValueHandle* val_handle)
    -> BinaryValueHandle* {
  auto obj_hc = MakeHandleConverter(obj_handle, "Bad handle: obj");
  if (!obj_hc) {
    return obj_hc.GetErrorHandle();
  }

  auto key_hc = MakeHandleConverter(key_handle, "Bad handle: key");
  if (!key_hc) {
    return key_hc.GetErrorHandle();
  }

  auto val_hc = MakeHandleConverter(val_handle, "Bad handle: val");
  if (!val_hc) {
    return val_hc.GetErrorHandle();
  }

  return bv_registry_->Remember(isolate_manager_->RunAndAwait(
      [object_manipulator = object_manipulator_, obj_ptr = obj_hc.GetPtr(),
       key_ptr = key_hc.GetPtr(),
       val_ptr = val_hc.GetPtr()](v8::Isolate* isolate) mutable {
        return object_manipulator->Set(isolate, obj_ptr.get(), key_ptr.get(),
                                       val_ptr.get());
      }));
}

auto Context::DelObjectItem(BinaryValueHandle* obj_handle,
                            BinaryValueHandle* key_handle)
    -> BinaryValueHandle* {
  auto obj_hc = MakeHandleConverter(obj_handle, "Bad handle: obj");
  if (!obj_hc) {
    return obj_hc.GetErrorHandle();
  }

  auto key_hc = MakeHandleConverter(key_handle, "Bad handle: key");
  if (!key_hc) {
    return key_hc.GetErrorHandle();
  }

  return bv_registry_->Remember(isolate_manager_->RunAndAwait(
      [object_manipulator = object_manipulator_, obj_ptr = obj_hc.GetPtr(),
       key_ptr = key_hc.GetPtr()](v8::Isolate* isolate) mutable {
        return object_manipulator->Del(isolate, obj_ptr.get(), key_ptr.get());
      }));
}

auto Context::SpliceArray(BinaryValueHandle* obj_handle,
                          int32_t start,
                          int32_t delete_count,
                          BinaryValueHandle* new_val_handle)
    -> BinaryValueHandle* {
  auto obj_hc = MakeHandleConverter(obj_handle, "Bad handle: obj");
  if (!obj_hc) {
    return obj_hc.GetErrorHandle();
  }

  BinaryValue::Ptr new_val_ptr;
  if (new_val_handle != nullptr) {
    auto new_val_hc =
        MakeHandleConverter(new_val_handle, "Bad handle: new_val");
    if (!new_val_hc) {
      return new_val_hc.GetErrorHandle();
    }
    new_val_ptr = new_val_hc.GetPtr();
  }

  return bv_registry_->Remember(isolate_manager_->RunAndAwait(
      [object_manipulator = object_manipulator_, obj_ptr = obj_hc.GetPtr(),
       start, delete_count, new_val_ptr](v8::Isolate* isolate) {
        return object_manipulator->Splice(isolate, obj_ptr.get(), start,
                                          delete_count, new_val_ptr.get());
      }));
}

void Context::FreeBinaryValue(BinaryValueHandle* val) {
  bv_registry_->Forget(val);
}

auto Context::CallFunction(BinaryValueHandle* func_handle,
                           BinaryValueHandle* this_handle,
                           BinaryValueHandle* argv_handle,
                           uint64_t callback_id) -> uint64_t {
  auto func_hc = MakeHandleConverter(func_handle, "Bad handle: func");
  if (!func_hc) {
    return RunTask(
        [err = func_hc.GetErrorPtr()](v8::Isolate* /*isolate*/) { return err; },
        callback_id);
  }

  auto this_hc = MakeHandleConverter(this_handle, "Bad handle: this");
  if (!this_hc) {
    return RunTask(
        [err = this_hc.GetErrorPtr()](v8::Isolate* /*isolate*/) { return err; },
        callback_id);
  }

  auto argv_hc = MakeHandleConverter(argv_handle, "Bad handle: argv");
  if (!argv_hc) {
    return RunTask(
        [err = argv_hc.GetErrorPtr()](v8::Isolate* /*isolate*/) { return err; },
        callback_id);
  }

  return RunTask(
      [object_manipulator = object_manipulator_, func_ptr = func_hc.GetPtr(),
       this_ptr = this_hc.GetPtr(),
       argv_ptr = argv_hc.GetPtr()](v8::Isolate* isolate) {
        return object_manipulator->Call(isolate, func_ptr.get(), this_ptr.get(),
                                        argv_ptr.get());
      },
      callback_id);
}

auto Context::BinaryValueCount() -> size_t {
  return bv_registry_->Count();
}

}  // end namespace MiniRacer
