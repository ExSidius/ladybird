/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>
#include <LibWeb/WebAssembly/DOMHost/NativeEventCallback.h>

namespace Web::WebAssembly::DOMHost {

GC_DEFINE_ALLOCATOR(NativeEventCallback);

NativeEventCallback::NativeEventCallback(DOMHostInstance& host_instance, Wasm::FunctionAddress function, i32 user_data)
    : m_host_instance(host_instance)
    , m_function(function)
    , m_user_data(user_data)
{
}

void NativeEventCallback::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host_instance);
}

void NativeEventCallback::invoke(DOM::Event& event)
{
    // The event handle is valid only for the duration of the callback; a copy the
    // guest stashes away goes stale when we release it below (and traps on use).
    auto handle = m_host_instance->allocate_handle(event, DOMHostInstance::HandleKind::Event);
    m_host_instance->invoke_callback(m_function, handle, m_user_data);
    m_host_instance->release_handle(handle);
}

}
