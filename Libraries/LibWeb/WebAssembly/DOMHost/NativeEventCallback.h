/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly::DOMHost {

// A DOM event listener whose target is a function in a wasm-dom module's exported
// funcref table rather than a JS callback. Holding one keeps the whole module
// environment (DOMHostInstance) alive; the resulting node -> listener -> instance ->
// node cycles are ordinary same-heap GC cycles and are collected with the document.
class NativeEventCallback final : public JS::Cell {
    GC_CELL(NativeEventCallback, JS::Cell);
    GC_DECLARE_ALLOCATOR(NativeEventCallback);

public:
    NativeEventCallback(DOMHostInstance&, Wasm::FunctionAddress, i32 user_data);

    void invoke(DOM::Event&);

private:
    virtual void visit_edges(Visitor&) override;

    GC::Ref<DOMHostInstance> m_host_instance;
    // Resolved from the funcref table at registration time; a later table.set does
    // not rebind the listener.
    Wasm::FunctionAddress m_function;
    i32 m_user_data { 0 };
};

}
