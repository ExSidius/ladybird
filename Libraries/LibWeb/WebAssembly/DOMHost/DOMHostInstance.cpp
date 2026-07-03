/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/TemporaryChange.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Function.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebAssembly/DOMHost/HostFunctions.h>

namespace Web::WebAssembly::DOMHost {

GC_DEFINE_ALLOCATOR(FetchResponse);
GC_DEFINE_ALLOCATOR(DOMHostInstance);

GC::Ref<DOMHostInstance> DOMHostInstance::create(GC::Heap& heap, DOM::Document& document, NonnullRefPtr<Wasm::Module> module)
{
    return heap.allocate<DOMHostInstance>(document, move(module));
}

DOMHostInstance::DOMHostInstance(DOM::Document& document, NonnullRefPtr<Wasm::Module> module)
    : m_document(document)
    , m_machine(make<Wasm::AbstractMachine>())
    , m_module(move(module))
{
    // Slot 0 is reserved so that handle 0 always means null.
    m_handles.append({});
}

void DOMHostInstance::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    for (auto& entry : m_handles)
        visitor.visit(entry.cell);
}

ErrorOr<void, ByteString> DOMHostInstance::link_and_instantiate()
{
    // The interpreter path is the known-good one; native compilation is what the
    // JS WebAssembly API currently uses for instantiation-by-buffer as well.
    if (auto result = m_machine->validate(*m_module, {}, Wasm::CompileToNative::No); result.is_error())
        return ByteString::formatted("validation failed: {}", result.error().error_string);

    Wasm::Linker linker { *m_module };
    auto imports_or_error = resolve_dom_imports(linker, *this);
    if (imports_or_error.is_error())
        return imports_or_error.release_error();

    linker.link(imports_or_error.release_value());
    auto link_result = linker.finish();
    if (link_result.is_error()) {
        StringBuilder builder;
        builder.append("missing imports: "sv);
        builder.join(' ', link_result.error().missing_imports);
        return builder.to_byte_string();
    }

    auto instance_result = m_machine->instantiate(*m_module, link_result.release_value());
    if (instance_result.is_error())
        return ByteString::formatted("instantiation failed: {}", instance_result.error().error);
    m_instance = instance_result.release_value();

    for (auto& export_instance : m_instance->exports()) {
        export_instance.value().visit(
            [&](Wasm::FunctionAddress const& address) {
                if (export_instance.name() == "_start"sv)
                    m_start = address;
            },
            [&](Wasm::MemoryAddress const& address) {
                if (export_instance.name() == "memory"sv)
                    m_memory = address;
            },
            [&](Wasm::TableAddress const& address) {
                if (export_instance.name() == "__indirect_function_table"sv)
                    m_function_table = address;
            },
            [](auto const&) {});
    }

    if (!m_memory.has_value())
        return ByteString { "module does not export its linear memory as \"memory\""sv };
    if (!m_start.has_value())
        return ByteString { "module does not export a \"_start\" function"sv };

    return {};
}

ErrorOr<void, ByteString> DOMHostInstance::invoke_start()
{
    VERIFY(!m_invoking);
    TemporaryChange invoking { m_invoking, true };
    auto result = m_machine->invoke(*m_start, {});
    if (result.is_trap())
        return ByteString::formatted("_start trapped: {}", result.trap().format());
    return {};
}

ErrorOr<Wasm::FunctionAddress, Wasm::Trap> DOMHostInstance::callback_from_table_index(u32 index)
{
    if (!m_function_table.has_value())
        return Wasm::Trap::from_string("module does not export its function table as \"__indirect_function_table\""sv);
    auto* table = m_machine->store().get(*m_function_table);
    if (!table)
        return Wasm::Trap::from_string("exported function table is gone"sv);
    if (index >= table->elements().size())
        return Wasm::Trap::from_string(ByteString::formatted("callback index {} is outside the function table", index));

    auto const* function_reference = table->elements()[index].ref().get_pointer<Wasm::Reference::Func>();
    if (!function_reference)
        return Wasm::Trap::from_string(ByteString::formatted("function table slot {} does not hold a function", index));

    auto const* function_instance = m_machine->store().get(function_reference->address);
    VERIFY(function_instance);
    auto const& type = function_instance->visit([](auto const& function) -> Wasm::FunctionType const& { return function.type(); });
    bool signature_matches = type.parameters().size() == 2 && type.results().is_empty()
        && type.parameters()[0].kind() == Wasm::ValueType::Kind::I32
        && type.parameters()[1].kind() == Wasm::ValueType::Kind::I32;
    if (!signature_matches)
        return Wasm::Trap::from_string(ByteString::formatted("callback {} must have signature (i32, i32) -> ()", index));

    return function_reference->address;
}

void DOMHostInstance::invoke_callback(Wasm::FunctionAddress function, i32 argument, i32 user_data)
{
    VERIFY(!m_invoking);
    TemporaryChange invoking { m_invoking, true };
    auto result = m_machine->invoke(function, { Wasm::Value(argument), Wasm::Value(user_data) });
    // A trapping event callback must not take down the renderer or affect other
    // listeners; log it and move on (mirrors how JS listener exceptions are
    // reported rather than propagated).
    if (result.is_trap())
        dbgln("wasm-dom: event callback trapped: {}", result.trap().format());
}

i32 DOMHostInstance::allocate_handle(GC::Cell& cell, HandleKind kind)
{
    u32 index = 0;
    if (!m_free_handle_indices.is_empty()) {
        index = m_free_handle_indices.take_last();
        m_handles[index].cell = &cell;
        m_handles[index].kind = kind;
    } else {
        index = m_handles.size();
        // With 24-bit indices a page would need ~16 million simultaneously live
        // handles to overflow; trap-by-VERIFY is fine for a prototype.
        VERIFY(index <= handle_index_mask);
        m_handles.append({ &cell, 1, kind });
    }
    return static_cast<i32>((static_cast<u32>(m_handles[index].generation) << handle_index_bits) | index);
}

ErrorOr<DOMHostInstance::HandleEntry*, Wasm::Trap> DOMHostInstance::entry_from_handle(i32 handle, Optional<HandleKind> kind)
{
    auto raw = static_cast<u32>(handle);
    auto index = raw & handle_index_mask;
    auto generation = static_cast<u8>(raw >> handle_index_bits);
    if (index == 0 || index >= m_handles.size())
        return Wasm::Trap::from_string(ByteString::formatted("invalid dom handle {:#x}", raw));
    auto& entry = m_handles[index];
    if (!entry.cell || entry.generation != generation)
        return Wasm::Trap::from_string(ByteString::formatted("stale dom handle {:#x}", raw));
    if (kind.has_value() && entry.kind != *kind)
        return Wasm::Trap::from_string(ByteString::formatted("dom handle {:#x} has the wrong kind", raw));
    return &entry;
}

i32 DOMHostInstance::allocate_handle_for_cell(GC::Cell& cell)
{
    auto kind = HandleKind::Object;
    if (is<DOM::Node>(cell))
        kind = HandleKind::Node;
    else if (is<DOM::Event>(cell))
        kind = HandleKind::Event;
    return allocate_handle(cell, kind);
}

ErrorOr<GC::Ref<GC::Cell>, Wasm::Trap> DOMHostInstance::cell_from_handle(i32 handle)
{
    auto* entry = TRY(entry_from_handle(handle, {}));
    return GC::Ref { *entry->cell };
}

void DOMHostInstance::set_last_error(WebIDL::Exception const& exception)
{
    m_last_error = exception.visit(
        [](WebIDL::SimpleException const& simple) {
            return simple.message.visit(
                [](String const& message) { return message; },
                [](StringView message) { return MUST(String::from_utf8(message)); });
        },
        [](GC::Ref<WebIDL::DOMException> const& dom_exception) {
            return MUST(String::formatted("{}: {}", dom_exception->name(), dom_exception->message()));
        },
        [](JS::Completion const&) {
            return "JavaScript completion exception"_string;
        });
}

ErrorOr<GC::Ref<DOM::Node>, Wasm::Trap> DOMHostInstance::node_from_handle(i32 handle)
{
    auto* entry = TRY(entry_from_handle(handle, HandleKind::Node));
    return *as<DOM::Node>(entry->cell.ptr());
}

ErrorOr<GC::Ref<DOM::Event>, Wasm::Trap> DOMHostInstance::event_from_handle(i32 handle)
{
    auto* entry = TRY(entry_from_handle(handle, HandleKind::Event));
    return *as<DOM::Event>(entry->cell.ptr());
}

ErrorOr<GC::Ref<FetchResponse>, Wasm::Trap> DOMHostInstance::response_from_handle(i32 handle)
{
    auto* entry = TRY(entry_from_handle(handle, HandleKind::Response));
    return *as<FetchResponse>(entry->cell.ptr());
}

void DOMHostInstance::set_timeout(u32 milliseconds, Wasm::FunctionAddress function, i32 user_data)
{
    // An active Platform::Timer survives garbage collection on its own, and the
    // handler's captured GC::Ref keeps this instance (and thus the machine) alive
    // until it fires. The callback runs as an ordinary event-loop turn, never
    // nested inside a guest invocation.
    auto handler = GC::create_function(heap(), [self = GC::Ref { *this }, function, user_data] {
        self->invoke_callback(function, 0, user_data);
    });
    Platform::Timer::create_single_shot(heap(), static_cast<int>(milliseconds), handler)->start();
}

void DOMHostInstance::start_fetch(URL::URL url, Wasm::FunctionAddress function, i32 user_data)
{
    auto& realm = m_document->realm();
    auto& vm = realm.vm();

    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(move(url));
    request->set_client(&m_document->relevant_settings_object());
    // CORS mode, like the fetch() API: the host hands the guest the raw body, so
    // cross-origin reads must require CORS opt-in. (Consequently, like fetch(),
    // this does not work on file:// pages — the file-scheme fetch guard blocks
    // destination-less requests to prevent data exfiltration.)
    request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = [self = GC::Ref { *this }, function, user_data](auto response, auto body_bytes) {
        response = response->unsafe_response();

        // Network failure or bodyless response -> callback argument 0.
        i32 argument = 0;
        if (body_bytes.template has<Core::ImmutableBytes>()) {
            auto body = body_bytes.template get<Core::ImmutableBytes>();
            auto body_copy = ByteBuffer::copy(body.bytes());
            if (!body_copy.is_error()) {
                auto response_cell = self->heap().allocate<FetchResponse>(response->status(), body_copy.release_value());
                argument = self->allocate_handle(*response_cell, HandleKind::Response);
            }
        }
        self->invoke_callback(function, argument, user_data);
        // Like event handles, response handles are valid only during the callback.
        if (argument != 0)
            self->release_handle(argument);
    };
    Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
}

void DOMHostInstance::release_handle(i32 handle)
{
    // Deliberately does not trap: releasing null or an already-released handle is a no-op.
    auto raw = static_cast<u32>(handle);
    auto index = raw & handle_index_mask;
    auto generation = static_cast<u8>(raw >> handle_index_bits);
    if (index == 0 || index >= m_handles.size())
        return;
    auto& entry = m_handles[index];
    if (!entry.cell || entry.generation != generation)
        return;
    entry.cell = nullptr;
    // Bump the generation so outstanding copies of this handle go stale. Generation
    // 0 is never issued, so a full wrap cannot collide with the null encoding;
    // aliasing after 255 reuses of one slot is an accepted prototype limitation.
    entry.generation = entry.generation == 255 ? 1 : entry.generation + 1;
    m_free_handle_indices.append(index);
}

ErrorOr<Wasm::MemoryInstance*, Wasm::Trap> DOMHostInstance::memory()
{
    // Host functions can legally be reached before instantiation finishes (a wasm
    // start section may call imports); the exported memory is only known afterwards.
    if (!m_memory.has_value())
        return Wasm::Trap::from_string("dom host functions may not be called before instantiation completes (use _start, not the wasm start section)"sv);
    auto* memory = m_machine->store().get(*m_memory);
    if (!memory)
        return Wasm::Trap::from_string("exported memory is gone"sv);
    return memory;
}

ErrorOr<String, Wasm::Trap> DOMHostInstance::read_utf8_string(u32 pointer, u32 length)
{
    auto* memory = TRY(this->memory());
    Checked<u64> end = static_cast<u64>(pointer);
    end += length;
    if (end.has_overflow() || end.value() > memory->size())
        return Wasm::Trap::from_string(ByteString::formatted("string ({:#x}, {}) is outside linear memory", pointer, length));
    auto view = StringView { memory->data().offset_pointer(pointer), length };
    auto string_or_error = String::from_utf8(view);
    if (string_or_error.is_error())
        return Wasm::Trap::from_string(ByteString::formatted("string ({:#x}, {}) is not valid UTF-8", pointer, length));
    return string_or_error.release_value();
}

ErrorOr<i32, Wasm::Trap> DOMHostInstance::write_string(StringView string, u32 destination_pointer, u32 destination_capacity)
{
    auto* memory = TRY(this->memory());
    Checked<u64> end = static_cast<u64>(destination_pointer);
    end += destination_capacity;
    if (end.has_overflow() || end.value() > memory->size())
        return Wasm::Trap::from_string(ByteString::formatted("destination buffer ({:#x}, {}) is outside linear memory", destination_pointer, destination_capacity));
    if (string.length() > NumericLimits<i32>::max())
        return Wasm::Trap::from_string("string too long"sv);
    auto bytes_to_write = min(string.length(), static_cast<size_t>(destination_capacity));
    if (bytes_to_write > 0)
        memory->data().overwrite(destination_pointer, string.characters_without_null_termination(), bytes_to_write);
    return static_cast<i32>(string.length());
}

}
