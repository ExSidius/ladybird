/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>
#include <LibWeb/WebAssembly/DOMHost/HostFunctions.h>
#include <LibWeb/WebAssembly/DOMHost/NativeEventCallback.h>

namespace Web::WebAssembly::DOMHost {

// Unwrap an ErrorOr<_, Wasm::Trap>, turning the trap into the host function's result.
#define TRY_OR_TRAP(...)                                       \
    ({                                                         \
        auto&& _temporary_result = (__VA_ARGS__);              \
        if (_temporary_result.is_error()) [[unlikely]]         \
            return Wasm::Result { _temporary_result.release_error() }; \
        _temporary_result.release_value();                     \
    })

static Wasm::Result return_i32(i32 value)
{
    return Wasm::Result { Vector<Wasm::Value> { Wasm::Value(value) } };
}

static Wasm::Result return_nothing()
{
    return Wasm::Result { Vector<Wasm::Value> {} };
}

// dom.get_element_by_id(id_ptr: i32, id_len: i32) -> handle: i32 (0 if not found)
static Wasm::Result get_element_by_id(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto id = TRY_OR_TRAP(instance.resolve_fly_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto element = instance.document().get_element_by_id(id);
    if (!element)
        return return_i32(0);
    return return_i32(instance.allocate_handle(*element, DOMHostInstance::HandleKind::Node));
}

// dom.create_element(name_ptr: i32, name_len: i32) -> handle: i32 (0 on invalid name)
static Wasm::Result create_element(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto local_name = TRY_OR_TRAP(instance.resolve_fly_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto element_or_error = DOM::create_element(instance.document(), local_name, Namespace::HTML);
    if (element_or_error.is_error())
        return return_i32(0);
    return return_i32(instance.allocate_handle(*element_or_error.release_value(), DOMHostInstance::HandleKind::Node));
}

// dom.create_text_node(data_ptr: i32, data_len: i32) -> handle: i32
static Wasm::Result create_text_node(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto data = TRY_OR_TRAP(instance.resolve_utf16_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto text = instance.document().create_text_node(data);
    return return_i32(instance.allocate_handle(*text, DOMHostInstance::HandleKind::Node));
}

// dom.append_child(parent: i32, child: i32) -> status: i32 (0 ok, -1 rejected)
static Wasm::Result append_child(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto parent = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    auto child = TRY_OR_TRAP(instance.node_from_handle(arguments[1].to<i32>()));
    if (parent->append_child(child).is_error())
        return return_i32(-1);
    return return_i32(0);
}

// dom.set_attribute(node: i32, name_ptr: i32, name_len: i32, value_ptr: i32, value_len: i32) -> status: i32
static Wasm::Result set_attribute(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto node = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    if (!is<DOM::Element>(*node))
        return Wasm::Result { Wasm::Trap::from_string("set_attribute requires an element handle"sv) };
    auto name = TRY_OR_TRAP(instance.resolve_fly_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    auto value = TRY_OR_TRAP(instance.resolve_string(arguments[3].to<u32>(), arguments[4].to<u32>()));
    static_cast<DOM::Element&>(*node).set_attribute_value(name, value);
    return return_i32(0);
}

// dom.get_attribute(node: i32, name_ptr: i32, name_len: i32, dst_ptr: i32, dst_cap: i32) -> len: i32
// Returns -1 if the attribute is absent; otherwise the attribute value's full byte
// length, having written min(len, dst_cap) bytes (the caller-buffer retry ABI).
static Wasm::Result get_attribute(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto node = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    if (!is<DOM::Element>(*node))
        return Wasm::Result { Wasm::Trap::from_string("get_attribute requires an element handle"sv) };
    auto name = TRY_OR_TRAP(instance.resolve_fly_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    auto value = static_cast<DOM::Element&>(*node).attribute(name);
    if (!value.has_value())
        return return_i32(-1);
    return return_i32(TRY_OR_TRAP(instance.write_string(*value, arguments[3].to<u32>(), arguments[4].to<u32>())));
}

// dom.text_content_get(node: i32, dst_ptr: i32, dst_cap: i32) -> len: i32 (-1 if null)
static Wasm::Result text_content_get(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto node = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    auto content = node->text_content();
    if (!content.has_value())
        return return_i32(-1);
    return return_i32(TRY_OR_TRAP(instance.write_utf16_string(content->utf16_view(), arguments[1].to<u32>(), arguments[2].to<u32>())));
}

// dom.text_content_set(node: i32, ptr: i32, len: i32) -> status: i32
static Wasm::Result text_content_set(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto node = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    auto content = TRY_OR_TRAP(instance.resolve_utf16_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    if (node->set_text_content(content).is_error())
        return return_i32(-1);
    return return_i32(0);
}

// dom.add_event_listener(node: i32, type_ptr: i32, type_len: i32, callback: i32, user_data: i32) -> status: i32
// `callback` is an index into the guest's exported funcref table; the function there
// must have signature (i32 event_handle, i32 user_data) -> ().
static Wasm::Result add_event_listener(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto node = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    auto type = TRY_OR_TRAP(instance.resolve_fly_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    auto function = TRY_OR_TRAP(instance.callback_from_table_index(arguments[3].to<u32>()));

    auto& heap = instance.heap();
    auto callback = heap.allocate<NativeEventCallback>(instance, function, arguments[4].to<i32>());
    auto listener = heap.allocate<DOM::DOMEventListener>();
    listener->type = type;
    listener->native_callback = callback;
    node->add_an_event_listener(*listener);
    return return_i32(0);
}

// dom.event_type(event: i32, dst_ptr: i32, dst_cap: i32) -> len: i32
static Wasm::Result event_type(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto event = TRY_OR_TRAP(instance.event_from_handle(arguments[0].to<i32>()));
    return return_i32(TRY_OR_TRAP(instance.write_string(event->type().bytes_as_string_view(), arguments[1].to<u32>(), arguments[2].to<u32>())));
}

// dom.set_timeout(ms: i32, callback: i32, user_data: i32) -> status: i32
// The callback is invoked once from the event loop with argument 0. No handle, no
// cancellation in v0.
static Wasm::Result set_timeout(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto milliseconds = arguments[0].to<i32>();
    auto function = TRY_OR_TRAP(instance.callback_from_table_index(arguments[1].to<u32>()));
    instance.set_timeout(milliseconds < 0 ? 0 : static_cast<u32>(milliseconds), function, arguments[2].to<i32>());
    return return_i32(0);
}

// dom.fetch(url_ptr: i32, url_len: i32, callback: i32, user_data: i32) -> status: i32
// The URL is parsed relative to the document. The callback receives a Response
// handle (0 on network failure) valid only for the duration of the callback.
static Wasm::Result fetch(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto url_string = TRY_OR_TRAP(instance.resolve_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto function = TRY_OR_TRAP(instance.callback_from_table_index(arguments[2].to<u32>()));
    auto url = instance.document().encoding_parse_url(url_string);
    if (!url.has_value())
        return return_i32(-1);
    instance.start_fetch(url.release_value(), function, arguments[3].to<i32>());
    return return_i32(0);
}

// dom.response_status(response: i32) -> i32
static Wasm::Result response_status(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto response = TRY_OR_TRAP(instance.response_from_handle(arguments[0].to<i32>()));
    return return_i32(response->status());
}

// dom.response_read(response: i32, dst_ptr: i32, dst_cap: i32) -> len: i32
// Same caller-buffer retry ABI as the string getters.
static Wasm::Result response_read(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto response = TRY_OR_TRAP(instance.response_from_handle(arguments[0].to<i32>()));
    auto body = StringView { response->body() };
    return return_i32(TRY_OR_TRAP(instance.write_string(body, arguments[1].to<u32>(), arguments[2].to<u32>())));
}

// dom.intern(ptr: i32, len: i32) -> id: i32
// Stores the string once, pre-converted to every flavor; pass (id, 0xFFFFFFFF)
// wherever a string parameter is expected.
static Wasm::Result intern(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto string = TRY_OR_TRAP(instance.read_utf8_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    return return_i32(instance.intern_string(move(string)));
}

// dom.noop() -> (): does nothing; exists to measure pure boundary-crossing cost.
static Wasm::Result noop(DOMHostInstance&, Span<Wasm::Value>)
{
    return return_nothing();
}

// dom.release(handle: i32) -> ()
static Wasm::Result release(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    instance.release_handle(arguments[0].to<i32>());
    return return_nothing();
}

// dom.last_error_message(dst_ptr: i32, dst_cap: i32) -> len: i32 (-1 if no error recorded)
static Wasm::Result last_error_message(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto const& error = instance.last_error();
    if (!error.has_value())
        return return_i32(-1);
    return return_i32(TRY_OR_TRAP(instance.write_string(*error, arguments[0].to<u32>(), arguments[1].to<u32>())));
}

// dom.now() -> f64 milliseconds (monotonic; for benchmarks/timing)
static Wasm::Result now(DOMHostInstance&, Span<Wasm::Value>)
{
    auto milliseconds = static_cast<f64>(MonotonicTime::now().nanoseconds()) / 1e6;
    return Wasm::Result { Vector<Wasm::Value> { Wasm::Value(milliseconds) } };
}

static constexpr auto s_host_functions = to_array<HostFunctionSpec>({
    { "get_element_by_id"sv, get_element_by_id, "ii"sv, "i"sv },
    { "create_element"sv, create_element, "ii"sv, "i"sv },
    { "create_text_node"sv, create_text_node, "ii"sv, "i"sv },
    { "append_child"sv, append_child, "ii"sv, "i"sv },
    { "set_attribute"sv, set_attribute, "iiiii"sv, "i"sv },
    { "get_attribute"sv, get_attribute, "iiiii"sv, "i"sv },
    { "text_content_get"sv, text_content_get, "iii"sv, "i"sv },
    { "text_content_set"sv, text_content_set, "iii"sv, "i"sv },
    { "add_event_listener"sv, add_event_listener, "iiiii"sv, "i"sv },
    { "event_type"sv, event_type, "iii"sv, "i"sv },
    { "set_timeout"sv, set_timeout, "iii"sv, "i"sv },
    { "fetch"sv, fetch, "iiii"sv, "i"sv },
    { "response_status"sv, response_status, "i"sv, "i"sv },
    { "response_read"sv, response_read, "iii"sv, "i"sv },
    { "last_error_message"sv, last_error_message, "ii"sv, "i"sv },
    { "intern"sv, intern, "ii"sv, "i"sv },
    { "noop"sv, noop, ""sv, ""sv },
    { "now"sv, now, ""sv, "d"sv },
    { "release"sv, release, "i"sv, ""sv },
});

static Wasm::ValueType value_type_for_char(char c)
{
    switch (c) {
    case 'I':
        return Wasm::ValueType { Wasm::ValueType::Kind::I64 };
    case 'f':
        return Wasm::ValueType { Wasm::ValueType::Kind::F32 };
    case 'd':
        return Wasm::ValueType { Wasm::ValueType::Kind::F64 };
    default:
        return Wasm::ValueType { Wasm::ValueType::Kind::I32 };
    }
}

static Wasm::FunctionType function_type_for_spec(HostFunctionSpec const& spec)
{
    Vector<Wasm::ValueType> parameters;
    parameters.ensure_capacity(spec.parameters.length());
    for (auto c : spec.parameters)
        parameters.unchecked_append(value_type_for_char(c));
    Vector<Wasm::ValueType> results;
    results.ensure_capacity(spec.results.length());
    for (auto c : spec.results)
        results.unchecked_append(value_type_for_char(c));
    return Wasm::FunctionType { move(parameters), move(results) };
}

static bool function_types_match(Wasm::FunctionType const& declared, Wasm::FunctionType const& expected)
{
    if (declared.parameters().size() != expected.parameters().size() || declared.results().size() != expected.results().size())
        return false;
    for (size_t i = 0; i < declared.parameters().size(); ++i) {
        if (declared.parameters()[i].kind() != expected.parameters()[i].kind())
            return false;
    }
    for (size_t i = 0; i < declared.results().size(); ++i) {
        if (declared.results()[i].kind() != expected.results()[i].kind())
            return false;
    }
    return true;
}

ErrorOr<HashMap<Wasm::Linker::Name, Wasm::ExternValue>, ByteString> resolve_dom_imports(Wasm::Linker& linker, DOMHostInstance& instance)
{
    HashMap<Wasm::Linker::Name, Wasm::ExternValue> resolved_imports;

    for (auto const& import_name : linker.unresolved_imports()) {
        if (import_name.module != "dom"sv)
            return ByteString::formatted("unknown import module \"{}\" (only \"dom\" is available)", import_name.module);

        auto const* type_index = import_name.type.get_pointer<Wasm::TypeIndex>();
        if (!type_index)
            return ByteString::formatted("import dom.{} is not a function", import_name.name);

        auto const* spec = [&]() -> HostFunctionSpec const* {
            for (auto const& candidate : s_host_functions) {
                if (candidate.name == import_name.name.view())
                    return &candidate;
            }
            for (auto const& candidate : generated_host_function_specs()) {
                if (candidate.name == import_name.name.view())
                    return &candidate;
            }
            return nullptr;
        }();
        if (!spec)
            return ByteString::formatted("unknown import dom.{}", import_name.name);

        auto expected_type = function_type_for_spec(*spec);
        auto const& declared_type = instance.module().type_section().types()[type_index->value()].function();
        if (!function_types_match(declared_type, expected_type))
            return ByteString::formatted("import dom.{} has the wrong signature (expected ({}) -> ({}))", import_name.name, spec->parameters, spec->results);

        // The lambda captures the instance cell by reference: the host function lives
        // in the machine's store, and the machine is owned by (and dies with) the
        // instance cell, which the GC never moves.
        auto host_function = Wasm::HostFunction {
            [function = spec->function, &instance](Wasm::Configuration&, Span<Wasm::Value> arguments) -> Wasm::Result {
                return function(instance, arguments);
            },
            expected_type,
            ByteString { spec->name },
        };
        auto address = instance.machine().store().allocate(move(host_function));
        if (!address.has_value())
            return ByteString::formatted("failed to allocate host function dom.{}", import_name.name);
        resolved_imports.set(import_name, Wasm::ExternValue { *address });
    }

    return resolved_imports;
}

}
