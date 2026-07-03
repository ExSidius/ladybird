/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>
#include <LibWeb/WebAssembly/DOMHost/HostFunctions.h>

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
    auto id = TRY_OR_TRAP(instance.read_utf8_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto element = instance.document().get_element_by_id(FlyString { id });
    if (!element)
        return return_i32(0);
    return return_i32(instance.allocate_handle(*element, DOMHostInstance::HandleKind::Node));
}

// dom.create_element(name_ptr: i32, name_len: i32) -> handle: i32 (0 on invalid name)
static Wasm::Result create_element(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto local_name = TRY_OR_TRAP(instance.read_utf8_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto element_or_error = DOM::create_element(instance.document(), FlyString { local_name }, Namespace::HTML);
    if (element_or_error.is_error())
        return return_i32(0);
    return return_i32(instance.allocate_handle(*element_or_error.release_value(), DOMHostInstance::HandleKind::Node));
}

// dom.create_text_node(data_ptr: i32, data_len: i32) -> handle: i32
static Wasm::Result create_text_node(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto data = TRY_OR_TRAP(instance.read_utf8_string(arguments[0].to<u32>(), arguments[1].to<u32>()));
    auto text = instance.document().create_text_node(Utf16String::from_utf8(data));
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
    auto name = TRY_OR_TRAP(instance.read_utf8_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    auto value = TRY_OR_TRAP(instance.read_utf8_string(arguments[3].to<u32>(), arguments[4].to<u32>()));
    static_cast<DOM::Element&>(*node).set_attribute_value(FlyString { name }, value);
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
    auto name = TRY_OR_TRAP(instance.read_utf8_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    auto value = static_cast<DOM::Element&>(*node).attribute(FlyString { name });
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
    auto utf8 = content->to_utf8();
    return return_i32(TRY_OR_TRAP(instance.write_string(utf8, arguments[1].to<u32>(), arguments[2].to<u32>())));
}

// dom.text_content_set(node: i32, ptr: i32, len: i32) -> status: i32
static Wasm::Result text_content_set(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    auto node = TRY_OR_TRAP(instance.node_from_handle(arguments[0].to<i32>()));
    auto content = TRY_OR_TRAP(instance.read_utf8_string(arguments[1].to<u32>(), arguments[2].to<u32>()));
    if (node->set_text_content(Utf16String::from_utf8(content)).is_error())
        return return_i32(-1);
    return return_i32(0);
}

// dom.release(handle: i32) -> ()
static Wasm::Result release(DOMHostInstance& instance, Span<Wasm::Value> arguments)
{
    instance.release_handle(arguments[0].to<i32>());
    return return_nothing();
}

struct HostFunctionSpec {
    StringView name;
    Wasm::Result (*function)(DOMHostInstance&, Span<Wasm::Value>);
    size_t parameter_count { 0 };
    size_t result_count { 0 };
};

static constexpr auto s_host_functions = to_array<HostFunctionSpec>({
    { "get_element_by_id"sv, get_element_by_id, 2, 1 },
    { "create_element"sv, create_element, 2, 1 },
    { "create_text_node"sv, create_text_node, 2, 1 },
    { "append_child"sv, append_child, 2, 1 },
    { "set_attribute"sv, set_attribute, 5, 1 },
    { "get_attribute"sv, get_attribute, 5, 1 },
    { "text_content_get"sv, text_content_get, 3, 1 },
    { "text_content_set"sv, text_content_set, 3, 1 },
    { "release"sv, release, 1, 0 },
});

// Every parameter and result in the "dom" interface is an i32 (handles, pointers,
// lengths, statuses), so a signature is fully described by its arity.
static Wasm::FunctionType function_type_for_spec(HostFunctionSpec const& spec)
{
    Vector<Wasm::ValueType> parameters;
    parameters.ensure_capacity(spec.parameter_count);
    for (size_t i = 0; i < spec.parameter_count; ++i)
        parameters.unchecked_append(Wasm::ValueType { Wasm::ValueType::Kind::I32 });
    Vector<Wasm::ValueType> results;
    results.ensure_capacity(spec.result_count);
    for (size_t i = 0; i < spec.result_count; ++i)
        results.unchecked_append(Wasm::ValueType { Wasm::ValueType::Kind::I32 });
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
            return nullptr;
        }();
        if (!spec)
            return ByteString::formatted("unknown import dom.{}", import_name.name);

        auto expected_type = function_type_for_spec(*spec);
        auto const& declared_type = instance.module().type_section().types()[type_index->value()].function();
        if (!function_types_match(declared_type, expected_type))
            return ByteString::formatted("import dom.{} has the wrong signature (expected {} i32 parameters and {} i32 results)", import_name.name, spec->parameter_count, spec->result_count);

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
