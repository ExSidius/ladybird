/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly::DOMHost {

// A completed dom.fetch response, exposed to the guest as a Response handle that
// is valid only for the duration of the completion callback.
class FetchResponse final : public JS::Cell {
    GC_CELL(FetchResponse, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchResponse);

public:
    FetchResponse(u16 status, ByteBuffer body)
        : m_status(status)
        , m_body(move(body))
    {
    }

    u16 status() const { return m_status; }
    ReadonlyBytes body() const { return m_body.bytes(); }

private:
    u16 m_status { 0 };
    ByteBuffer m_body;
};

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

    // Resolve a guest callback: an index into the exported funcref table, checked
    // to be a function of type (i32, i32) -> ().
    ErrorOr<Wasm::FunctionAddress, Wasm::Trap> callback_from_table_index(u32 index);
    void invoke_callback(Wasm::FunctionAddress, i32 argument, i32 user_data);

    enum class HandleKind : u8 {
        Node,
        Event,
        Response,
        // Anything else the generated bindings hand out (DOMTokenList etc.).
        Object,
    };

    i32 allocate_handle(GC::Cell&, HandleKind);
    // For generated code: pick the handle kind from the cell's dynamic type.
    i32 allocate_handle_for_cell(GC::Cell&);
    ErrorOr<GC::Ref<DOM::Node>, Wasm::Trap> node_from_handle(i32);
    ErrorOr<GC::Ref<DOM::Event>, Wasm::Trap> event_from_handle(i32);
    ErrorOr<GC::Ref<FetchResponse>, Wasm::Trap> response_from_handle(i32);
    // For generated code: any-kind lookup; callers type-check the cell themselves.
    ErrorOr<GC::Ref<GC::Cell>, Wasm::Trap> cell_from_handle(i32);
    void release_handle(i32);

    // Expected DOM exceptions from generated bindings are recorded here (and a
    // sentinel returned); dom.last_error_message reads the record back.
    void set_last_error(WebIDL::Exception const&);
    Optional<String> const& last_error() const { return m_last_error; }

    // Async host operations: start now, deliver later by calling the guest callback
    // from the event loop. The guest brings its own scheduling (if any); the host
    // only provides operations and wakeups.
    void set_timeout(u32 milliseconds, Wasm::FunctionAddress, i32 user_data);
    void start_fetch(URL::URL, Wasm::FunctionAddress, i32 user_data);

    ErrorOr<String, Wasm::Trap> read_utf8_string(u32 pointer, u32 length);
    ErrorOr<i32, Wasm::Trap> write_string(StringView, u32 destination_pointer, u32 destination_capacity);
    // Like write_string but for UTF-16 DOM strings: ASCII-backed strings (the
    // common case) are written directly with no transcode or allocation.
    ErrorOr<i32, Wasm::Trap> write_utf16_string(Utf16View const&, u32 destination_pointer, u32 destination_capacity);

    // Interned strings: dom.intern(ptr, len) stores a string once (pre-converted
    // to every flavor the DOM wants) and returns an id; any string parameter then
    // accepts (id, interned_length_sentinel) instead of (ptr, len), skipping the
    // per-call copy, transcode, and FlyString hashing.
    static constexpr u32 interned_length_sentinel = 0xFFFFFFFF;
    i32 intern_string(String);
    ErrorOr<String, Wasm::Trap> resolve_string(u32 pointer_or_id, u32 length);
    ErrorOr<FlyString, Wasm::Trap> resolve_fly_string(u32 pointer_or_id, u32 length);
    ErrorOr<Utf16String, Wasm::Trap> resolve_utf16_string(u32 pointer_or_id, u32 length);

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
        u32 ref_count { 0 };
        u8 generation { 1 };
        HandleKind kind { HandleKind::Node };
    };
    ErrorOr<HandleEntry*, Wasm::Trap> entry_from_handle(i32, Optional<HandleKind>);

    Optional<String> m_last_error;

    // Handles are identity-cached: acquiring the same cell twice returns the same
    // handle with a bumped reference count, so guest-side handle equality is
    // object identity. The map only mirrors live entries (which pin their cells,
    // and the GC does not move cells), so raw keys are safe.
    HashMap<GC::Cell*, u32> m_cell_to_handle_index;

    struct InternedString {
        String utf8;
        FlyString fly;
        Utf16String utf16;
    };
    Vector<InternedString> m_interned_strings;

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

    // Same-machine reentrant invocation cannot happen in v0 (no host function
    // dispatches events synchronously, and a module cannot re-run its own script
    // element); this enforces that assumption.
    bool m_invoking { false };
};

}
