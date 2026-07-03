/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly::DOMHost {

// The environment for one <script type="application/wasm-dom"> module: owns the
// abstract machine, the module instance, and the handle table through which the
// guest refers to DOM objects. Everything the guest can reach stays alive exactly
// as long as this cell is reachable (handles are traced, not rooted), so a
// document that goes away takes its wasm environment and pinned nodes with it.
class DOMHostInstance final : public JS::Cell {
    GC_CELL(DOMHostInstance, JS::Cell);
    GC_DECLARE_ALLOCATOR(DOMHostInstance);

public:
    static GC::Ref<DOMHostInstance> create(GC::Heap&, DOM::Document&, NonnullRefPtr<Wasm::Module>);

    ErrorOr<void, ByteString> link_and_instantiate();
    ErrorOr<void, ByteString> invoke_start();

    enum class HandleKind : u8 {
        Node,
        Event,
    };

    i32 allocate_handle(GC::Cell&, HandleKind);
    ErrorOr<GC::Ref<DOM::Node>, Wasm::Trap> node_from_handle(i32);
    void release_handle(i32);

    ErrorOr<String, Wasm::Trap> read_utf8_string(u32 pointer, u32 length);
    ErrorOr<i32, Wasm::Trap> write_string(StringView, u32 destination_pointer, u32 destination_capacity);

    DOM::Document& document() { return *m_document; }
    Wasm::Module const& module() const { return *m_module; }
    Wasm::AbstractMachine& machine() { return *m_machine; }

private:
    DOMHostInstance(DOM::Document&, NonnullRefPtr<Wasm::Module>);

    virtual void visit_edges(Visitor&) override;

    ErrorOr<Wasm::MemoryInstance*, Wasm::Trap> memory();

    static constexpr u32 handle_index_bits = 24;
    static constexpr u32 handle_index_mask = (1 << handle_index_bits) - 1;

    struct HandleEntry {
        GC::Ptr<GC::Cell> cell;
        u8 generation { 1 };
        HandleKind kind { HandleKind::Node };
    };
    ErrorOr<HandleEntry*, Wasm::Trap> entry_from_handle(i32, HandleKind);

    GC::Ref<DOM::Document> m_document;
    // The machine must be heap-allocated and never move: its store hands out
    // pointers into itself (see the note on WebAssemblyCache).
    NonnullOwnPtr<Wasm::AbstractMachine> m_machine;
    NonnullRefPtr<Wasm::Module> m_module;
    RefPtr<Wasm::ModuleInstance> m_instance;

    Optional<Wasm::MemoryAddress> m_memory;
    Optional<Wasm::TableAddress> m_function_table;
    Optional<Wasm::FunctionAddress> m_start;

    // Slot 0 is reserved so that handle 0 always means null.
    Vector<HandleEntry> m_handles;
    Vector<u32> m_free_handle_indices;
};

}
