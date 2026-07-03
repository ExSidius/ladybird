/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>
#include <LibWeb/WebAssembly/DOMHost/HostFunctions.h>

namespace Web::WebAssembly::DOMHost {

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
    auto result = m_machine->invoke(*m_start, {});
    if (result.is_trap())
        return ByteString::formatted("_start trapped: {}", result.trap().format());
    return {};
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

ErrorOr<DOMHostInstance::HandleEntry*, Wasm::Trap> DOMHostInstance::entry_from_handle(i32 handle, HandleKind kind)
{
    auto raw = static_cast<u32>(handle);
    auto index = raw & handle_index_mask;
    auto generation = static_cast<u8>(raw >> handle_index_bits);
    if (index == 0 || index >= m_handles.size())
        return Wasm::Trap::from_string(ByteString::formatted("invalid dom handle {:#x}", raw));
    auto& entry = m_handles[index];
    if (!entry.cell || entry.generation != generation)
        return Wasm::Trap::from_string(ByteString::formatted("stale dom handle {:#x}", raw));
    if (entry.kind != kind)
        return Wasm::Trap::from_string(ByteString::formatted("dom handle {:#x} has the wrong kind", raw));
    return &entry;
}

ErrorOr<GC::Ref<DOM::Node>, Wasm::Trap> DOMHostInstance::node_from_handle(i32 handle)
{
    auto* entry = TRY(entry_from_handle(handle, HandleKind::Node));
    return *as<DOM::Node>(entry->cell.ptr());
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
