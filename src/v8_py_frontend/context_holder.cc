#include "context_holder.h"

#include <v8-context.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-persistent-handle.h>
#include <memory>
#include <utility>
#include "isolate_manager.h"

namespace MiniRacer {

ContextHolder::ContextHolder(std::shared_ptr<IsolateManager> isolate_manager)
    : isolate_manager_(std::move(isolate_manager)),
      context_(isolate_manager_->RunAndAwait([](v8::Isolate* isolate) {
        const v8::Isolate::Scope isolate_scope(isolate);
        const v8::HandleScope handle_scope(isolate);

        return std::make_unique<v8::Persistent<v8::Context>>(
            isolate, v8::Context::New(isolate));
      })) {}

ContextHolder::~ContextHolder() {
  isolate_manager_->RunAndAwait(
      [context = std::move(context_)](v8::Isolate*) { context->Reset(); });
}

}  // end namespace MiniRacer
