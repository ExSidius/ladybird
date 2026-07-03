#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""WebIDL -> wasm-dom host function generator.

A second backend over the same IDL corpus the JS bindings are generated from.
For a curated set of interfaces it emits:
  - DOMHostGeneratedFunctions.cpp: Wasm host functions calling the C++ DOM
    implementation directly (no JS), plus their signature table.
  - dom_generated.rs: matching extern declarations for Rust guests.

With --stats it instead reports, across the WHOLE corpus, how much of the web
platform's surface the current type-lowering subset covers.

Type lowering (v1):
  parameters: numeric/boolean -> i32/i64/f32/f64; strings -> (ptr, len) i32
  pair; interface types -> i32 handle (nullable: 0 = null). returns: undefined
  -> i32 status (0 ok, -1 exception); numeric/boolean -> value; strings
  (nullable ok) -> caller buffer (dst, cap) appended to parameters, returns
  full length (-1 = null, -2 = exception); interfaces -> i32 handle (0 = null
  or exception). Exceptions store a message retrievable via
  dom.last_error_message. Unions, dictionaries, sequences, records, promises,
  callbacks, enums, buffer sources, and optional/variadic parameters are out of
  scope for v1 and counted in --stats.
"""

import argparse
import sys

from io import StringIO
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import (
    cpp_type_name_for_string,
    fully_qualified_name_for_interface,
    implementation_header_for_interface,
    idl_implementation_cpp_name,
    is_numeric_type,
    is_string_type,
)
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import (
    IDLParameterizedType,
    IDLType,
    IDLUnionType,
    parse_module,
)

EMIT_INTERFACES = [
    "Node",
    "Element",
    "Document",
    "CharacterData",
    "Text",
    "Comment",
    "Event",
    "DOMTokenList",
    "HTMLElement",
]

# Operations/attributes whose C++ conventions the v1 emitter cannot call
# correctly; keyed by generated function base name.
BLOCKLIST: set[str] = set()

NUMERIC_WASM_TYPE = {
    "byte": "i",
    "octet": "i",
    "short": "i",
    "unsigned short": "i",
    "long": "i",
    "unsigned long": "i",
    "long long": "I",
    "unsigned long long": "I",
    "float": "f",
    "unrestricted float": "f",
    "double": "d",
    "unrestricted double": "d",
}

WASM_TO_CPP = {"i": "i32", "I": "i64", "f": "f32", "d": "f64"}


def snake(name: str) -> str:
    return title_case_to_snake_case(name).replace("-", "_")


class Unsupported(Exception):
    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


def classify_type(idl_type: IDLType, context: GenerationContext, extended_attributes: dict) -> dict:
    """Returns a lowering descriptor for a type, or raises Unsupported."""
    name = idl_type.name
    if isinstance(idl_type, IDLUnionType):
        raise Unsupported("union")
    if isinstance(idl_type, IDLParameterizedType):
        raise Unsupported({"Promise": "promise", "sequence": "sequence", "FrozenArray": "sequence", "record": "record"}.get(name, "parameterized"))
    if name == "undefined":
        return {"kind": "undefined"}
    if name == "boolean":
        return {"kind": "bool"}
    if is_numeric_type(name):
        return {"kind": "numeric", "wasm": NUMERIC_WASM_TYPE[name]}
    if is_string_type(name):
        return {"kind": "string", "cpp": cpp_type_name_for_string(name, extended_attributes)}
    if name in ("any", "object", "bigint"):
        raise Unsupported(name)
    if context.enumeration(idl_type) is not None:
        raise Unsupported("enum")
    if context.dictionary(idl_type) is not None:
        raise Unsupported("dictionary")
    if context.callback_function(idl_type) is not None:
        raise Unsupported("callback")
    interface = context.interface(idl_type)
    if interface is not None:
        if interface.is_callback_interface:
            raise Unsupported("callback interface")
        return {"kind": "interface", "interface": interface}
    raise Unsupported(f"unknown type {name}")


def classify_operation(op, context) -> dict:
    if "FIXME" in op.extended_attributes:
        raise Unsupported("FIXME")
    for parameter in op.parameters:
        if parameter.optional:
            raise Unsupported("optional parameter")
        if parameter.variadic:
            raise Unsupported("variadic parameter")
    params = [classify_type(p.type, context, p.extended_attributes) for p in op.parameters]
    for p, lowered in zip(op.parameters, params):
        if lowered["kind"] == "undefined":
            raise Unsupported("undefined parameter")
        if lowered["kind"] == "string" and p.type.nullable:
            raise Unsupported("nullable string parameter")
        lowered["nullable"] = p.type.nullable
        lowered["name"] = p.name
    ret = classify_type(op.return_type, context, op.extended_attributes)
    ret["nullable"] = op.return_type.nullable
    return {"params": params, "return": ret}


# ---------------------------------------------------------------- statistics


def run_stats(context: GenerationContext) -> None:
    totals: dict[str, int] = {}
    reasons: dict[str, int] = {}
    supported = 0
    total = 0
    supported_interfaces = set()
    all_interfaces = set()

    def consider(kind: str, describe: str, thunk) -> None:
        nonlocal supported, total
        total += 1
        totals[kind] = totals.get(kind, 0) + 1
        try:
            thunk()
        except Unsupported as e:
            reasons[e.reason] = reasons.get(e.reason, 0) + 1
        else:
            supported += 1
            supported_interfaces.add(describe)

    for module in context.modules:
        interface = module.interface
        if interface is None or interface.is_namespace or interface.is_callback_interface:
            continue
        all_interfaces.add(interface.name)
        for op in interface.regular_operations:
            if not op.name:
                continue
            consider("operation", interface.name, lambda op=op: classify_operation(op, context))
        for attribute in interface.regular_attributes:
            def check(attribute=attribute):
                lowered = classify_type(attribute.type, context, attribute.extended_attributes)
                if lowered["kind"] == "undefined":
                    raise Unsupported("undefined attribute")
            consider("attribute getter", interface.name, check)
            if not attribute.readonly:
                def check_setter(attribute=attribute):
                    lowered = classify_type(attribute.type, context, attribute.extended_attributes)
                    if lowered["kind"] == "undefined":
                        raise Unsupported("undefined attribute")
                    if lowered["kind"] == "string" and attribute.type.nullable and "Reflect" not in attribute.extended_attributes:
                        raise Unsupported("nullable string setter")
                consider("attribute setter", interface.name, check_setter)

    print(f"wasm-dom lowering coverage across the IDL corpus")
    print(f"  interfaces scanned: {len(all_interfaces)}")
    print(f"  members (operations + attribute accessors): {total}")
    print(f"  generatable with v1 lowering: {supported} ({100.0 * supported / total:.1f}%)")
    print(f"  interfaces with at least one generatable member: {len(supported_interfaces)}")
    print(f"  by member kind: {totals}")
    print(f"  skip reasons:")
    for reason, count in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f"    {reason:28} {count}")


# ------------------------------------------------------------------ emission


def wasm_param_signature(params: list[dict]) -> str:
    sig = "i"  # self handle
    for p in params:
        sig += {"bool": "i", "numeric": None, "string": "ii", "interface": "i"}[p["kind"]] or p["wasm"]
    return sig


class FunctionEmitter:
    def __init__(self, context: GenerationContext, out: StringIO):
        self.context = context
        self.out = out
        self.specs: list[tuple[str, str, str, str]] = []  # (import name, fn, params, results)
        self.rust: list[str] = []
        self.referenced_interfaces: dict = {}  # name -> Interface

    def emit(self, interface, base_name: str, lowered: dict, call_generator) -> None:
        """Emit one host function. call_generator(body, args) writes the call."""
        params = lowered["params"]
        ret = lowered["return"]
        self.referenced_interfaces[interface.name] = interface
        for referenced in [*params, ret]:
            if referenced["kind"] == "interface":
                self.referenced_interfaces[referenced["interface"].name] = referenced["interface"]
        fq_self = fully_qualified_name_for_interface(interface)
        function_name = f"{snake(interface.name)}_{base_name}"
        if function_name in BLOCKLIST:
            raise Unsupported("blocklisted")

        signature = wasm_param_signature(params)
        if ret["kind"] == "string":
            signature += "ii"  # dst, cap
        results = {
            "undefined": "i",
            "bool": "i",
            "numeric": ret.get("wasm", "i"),
            "string": "i",
            "interface": "i",
        }[ret["kind"]]

        w = self.out.write
        w(f"static Wasm::Result {function_name}(DOMHostInstance& instance, Span<Wasm::Value> arguments)\n{{\n")
        w("    (void)arguments;\n")
        w(f"    auto self_cell = TRY_OR_TRAP(instance.cell_from_handle(arguments[0].to<i32>()));\n")
        w(f"    auto* self = as_if<Web::{fq_self}>(self_cell.ptr());\n")
        w(f"    if (!self)\n        return Wasm::Result {{ Wasm::Trap::from_string(\"{function_name} expects a {interface.name} handle\"sv) }};\n")

        argument_index = 1
        call_arguments: list[str] = []
        for i, p in enumerate(params):
            if p["kind"] == "numeric":
                cpp = WASM_TO_CPP[p["wasm"]]
                w(f"    auto p{i} = arguments[{argument_index}].to<{cpp}>();\n")
                argument_index += 1
                call_arguments.append(f"p{i}")
            elif p["kind"] == "bool":
                w(f"    bool p{i} = arguments[{argument_index}].to<i32>() != 0;\n")
                argument_index += 1
                call_arguments.append(f"p{i}")
            elif p["kind"] == "string":
                # Flavor-specific resolution so interned strings skip conversion.
                cpp = p["cpp"]
                pair = f"arguments[{argument_index}].to<u32>(), arguments[{argument_index + 1}].to<u32>()"
                if cpp == "String":
                    w(f"    auto p{i} = TRY_OR_TRAP(instance.resolve_string({pair}));\n")
                elif cpp == "FlyString":
                    w(f"    auto p{i} = TRY_OR_TRAP(instance.resolve_fly_string({pair}));\n")
                elif cpp == "Utf16String":
                    w(f"    auto p{i} = TRY_OR_TRAP(instance.resolve_utf16_string({pair}));\n")
                elif cpp == "Utf16FlyString":
                    w(f"    Utf16FlyString p{i} {{ TRY_OR_TRAP(instance.resolve_utf16_string({pair})) }};\n")
                else:
                    raise Unsupported(f"string flavor {cpp}")
                argument_index += 2
                call_arguments.append(f"p{i}")
            elif p["kind"] == "interface":
                target = fully_qualified_name_for_interface(p["interface"])
                if p["nullable"]:
                    w(f"    GC::Ptr<Web::{target}> p{i};\n")
                    w(f"    if (arguments[{argument_index}].to<i32>() != 0) {{\n")
                    w(f"        auto p{i}_cell = TRY_OR_TRAP(instance.cell_from_handle(arguments[{argument_index}].to<i32>()));\n")
                    w(f"        p{i} = as_if<Web::{target}>(p{i}_cell.ptr());\n")
                    w(f"        if (!p{i})\n            return Wasm::Result {{ Wasm::Trap::from_string(\"{function_name}: argument {i} must be a {p['interface'].name}\"sv) }};\n")
                    w("    }\n")
                    call_arguments.append(f"p{i}")
                else:
                    w(f"    auto p{i}_cell = TRY_OR_TRAP(instance.cell_from_handle(arguments[{argument_index}].to<i32>()));\n")
                    w(f"    auto* p{i} = as_if<Web::{target}>(p{i}_cell.ptr());\n")
                    w(f"    if (!p{i})\n        return Wasm::Result {{ Wasm::Trap::from_string(\"{function_name}: argument {i} must be a {p['interface'].name}\"sv) }};\n")
                    call_arguments.append(f"*p{i}")
                argument_index += 1

        call_generator(w, call_arguments)

        # Outcome handling lives in templated runners (if constexpr only discards
        # branches inside templates), so the generated body is a single call.
        if ret["kind"] == "undefined":
            w("    return run_status(instance, invoke_member);\n")
        elif ret["kind"] == "bool":
            w("    return run_numeric<i32>(instance, invoke_member);\n")
        elif ret["kind"] == "numeric":
            w(f"    return run_numeric<{WASM_TO_CPP[ret['wasm']]}>(instance, invoke_member);\n")
        elif ret["kind"] == "string":
            w(f"    return run_string_out(instance, invoke_member, arguments[{argument_index}].to<u32>(), arguments[{argument_index + 1}].to<u32>());\n")
        elif ret["kind"] == "interface":
            w("    return run_handle_out(instance, invoke_member);\n")
        w("}\n\n")

        self.specs.append((function_name, function_name, signature, results))
        self.rust.append(rust_extern(function_name, signature, results, ret["kind"] == "string"))

    def emit_operation(self, interface, op) -> None:
        lowered = classify_operation(op, self.context)
        method = idl_implementation_cpp_name(op)
        base = snake(op.name)

        def call(w, args):
            w(f"    auto invoke_member = [&] {{ return self->{method}({', '.join(args)}); }};\n")

        self.emit(interface, base, lowered, call)

    def emit_attribute_getter(self, interface, attribute) -> None:
        lowered_type = classify_type(attribute.type, self.context, attribute.extended_attributes)
        if lowered_type["kind"] == "undefined":
            raise Unsupported("undefined attribute")
        lowered_type["nullable"] = attribute.type.nullable
        lowered = {"params": [], "return": lowered_type}
        reflect = "Reflect" in attribute.extended_attributes

        if reflect:
            content_name = attribute.extended_attributes.get("Reflect") or attribute.name.lower()
            if lowered_type["kind"] == "string" and not attribute.type.nullable:
                def call(w, args):
                    w(f"    auto invoke_member = [&] {{ return self->get_attribute_value(\"{content_name}\"_fly_string); }};\n")
            elif lowered_type["kind"] == "string" and attribute.type.nullable:
                def call(w, args):
                    w(f"    auto invoke_member = [&] {{ return self->get_attribute(\"{content_name}\"_fly_string); }};\n")
            elif lowered_type["kind"] == "bool":
                def call(w, args):
                    w(f"    auto invoke_member = [&] {{ return self->has_attribute(\"{content_name}\"_fly_string); }};\n")
            else:
                raise Unsupported("non-string reflect")
        else:
            method = idl_implementation_cpp_name(attribute)

            def call(w, args):
                w(f"    auto invoke_member = [&] {{ return self->{method}(); }};\n")

        self.emit(interface, snake(attribute.name), lowered, call)

    def emit_attribute_setter(self, interface, attribute) -> None:
        lowered_type = classify_type(attribute.type, self.context, attribute.extended_attributes)
        lowered_type["nullable"] = attribute.type.nullable
        lowered_type["name"] = "value"
        reflect = "Reflect" in attribute.extended_attributes
        if lowered_type["kind"] == "string" and attribute.type.nullable and not reflect:
            raise Unsupported("nullable string setter")
        lowered = {"params": [lowered_type], "return": {"kind": "undefined", "nullable": False}}

        if reflect:
            content_name = attribute.extended_attributes.get("Reflect") or attribute.name.lower()
            if lowered_type["kind"] == "string":
                def call(w, args):
                    w(f"    auto invoke_member = [&] {{ self->set_attribute_value(\"{content_name}\"_fly_string, {args[0]}); }};\n")
            elif lowered_type["kind"] == "bool":
                def call(w, args):
                    w("    auto invoke_member = [&] {\n")
                    w(f"        if ({args[0]})\n            self->set_attribute_value(\"{content_name}\"_fly_string, String {{}});\n")
                    w(f"        else\n            self->remove_attribute(\"{content_name}\"_fly_string);\n")
                    w("    };\n")
            else:
                raise Unsupported("non-string reflect")
        else:
            method = idl_implementation_cpp_name(attribute)

            def call(w, args):
                w(f"    auto invoke_member = [&] {{ return self->set_{method}({args[0]}); }};\n")

        self.emit(interface, f"set_{snake(attribute.name)}", lowered, call)


def rust_extern(name: str, params: str, results: str, has_string_out: bool) -> str:
    rust_types = {"i": "i32", "I": "i64", "f": "f32", "d": "f64"}
    args = ", ".join(f"a{i}: {rust_types[c]}" for i, c in enumerate(params))
    ret = f" -> {rust_types[results]}" if results else ""
    note = "  // string out: last two params are (dst_ptr, dst_cap); returns full length" if has_string_out else ""
    return f"    pub fn {name}({args}){ret};{note}"


def run_emit(context: GenerationContext, output_dir: Path) -> None:
    body = StringIO()
    emitter = FunctionEmitter(context, body)
    includes: set[str] = set()
    emitted = 0
    skipped: dict[str, int] = {}

    for module in context.modules:
        interface = module.interface
        if interface is None or interface.name not in EMIT_INTERFACES:
            continue
        includes.add(implementation_header_for_interface(interface))
        members = []
        seen_operation_names: set[str] = set()
        for op in interface.regular_operations:
            if not op.name or op.name in seen_operation_names:
                continue  # v1: first declaration of an overload set only
            seen_operation_names.add(op.name)
            members.append(("op", op))
        for attribute in interface.regular_attributes:
            members.append(("get", attribute))
            if not attribute.readonly:
                members.append(("set", attribute))
        for kind, member in members:
            try:
                if kind == "op":
                    emitter.emit_operation(interface, member)
                elif kind == "get":
                    emitter.emit_attribute_getter(interface, member)
                else:
                    emitter.emit_attribute_setter(interface, member)
                emitted += 1
            except Unsupported as e:
                skipped[e.reason] = skipped.get(e.reason, 0) + 1
        for param_interface in EMIT_INTERFACES:
            pass

    for referenced in emitter.referenced_interfaces.values():
        includes.add(implementation_header_for_interface(referenced))

    with open(output_dir / "DOMHostGeneratedFunctions.cpp", "w") as f:
        f.write("// Generated by Meta/Generators/generate_wasm_dom_bindings.py — do not edit.\n\n")
        f.write("#include <AK/TypeCasts.h>\n")
        for include in sorted(includes):
            f.write(f"#include <{include}>\n")
        f.write("#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>\n")
        f.write("#include <LibWeb/WebAssembly/DOMHost/HostFunctions.h>\n")
        f.write("\nnamespace Web::WebAssembly::DOMHost {\n\n")
        f.write(GENERATED_PRELUDE)
        f.write(body.getvalue())
        f.write("static constexpr auto s_generated_host_functions = to_array<HostFunctionSpec>({\n")
        for name, fn, params, results in emitter.specs:
            f.write(f"    {{ \"{name}\"sv, {fn}, \"{params}\"sv, \"{results}\"sv }},\n")
        f.write("});\n\n")
        f.write("ReadonlySpan<HostFunctionSpec> generated_host_function_specs()\n{\n    return s_generated_host_functions;\n}\n\n}\n")

    with open(output_dir / "dom_generated.rs", "w") as f:
        f.write("// Generated by Meta/Generators/generate_wasm_dom_bindings.py — do not edit.\n")
        f.write("// Extern declarations for the generated wasm-dom host surface.\n")
        f.write("#[link(wasm_import_module = \"dom\")]\n")
        f.write("extern \"C\" {\n")
        for line in emitter.rust:
            f.write(line + "\n")
        f.write("}\n")

    print(f"wasm-dom bindings: emitted {emitted} host functions", file=sys.stderr)
    if skipped:
        print(f"wasm-dom bindings: skipped {sum(skipped.values())} members: {skipped}", file=sys.stderr)


GENERATED_PRELUDE = """\
// Unwrap an ErrorOr<_, Wasm::Trap>, turning the trap into the host function's result.
#define TRY_OR_TRAP(...)                                                       \\
    ({                                                                         \\
        auto&& _temporary_result = (__VA_ARGS__);                              \\
        if (_temporary_result.is_error()) [[unlikely]]                         \\
            return Wasm::Result { _temporary_result.release_error() };         \\
        _temporary_result.release_value();                                     \\
    })

static Wasm::Result return_i32(i32 value)
{
    return Wasm::Result { Vector<Wasm::Value> { Wasm::Value(value) } };
}

template<typename T>
constexpr bool IsWasmDOMExceptionOr = false;
template<typename T>
constexpr bool IsWasmDOMExceptionOr<WebIDL::ExceptionOr<T>> = true;

// Write any supported string-ish value through the caller-buffer retry ABI.
template<typename T>
static Wasm::Result string_out(DOMHostInstance& instance, T const& value, u32 destination, u32 capacity)
{
    if constexpr (IsSpecializationOf<RemoveCVReference<T>, Optional>) {
        if (!value.has_value())
            return return_i32(-1);
        return string_out(instance, *value, destination, capacity);
    } else if constexpr (requires { value.utf16_view(); }) {
        return return_i32(TRY_OR_TRAP(instance.write_utf16_string(value.utf16_view(), destination, capacity)));
    } else if constexpr (SameAs<RemoveCVReference<T>, FlyString>) {
        return return_i32(TRY_OR_TRAP(instance.write_string(value.bytes_as_string_view(), destination, capacity)));
    } else {
        return return_i32(TRY_OR_TRAP(instance.write_string(value, destination, capacity)));
    }
}

// Wrap any supported cell-ish return value as a handle (0 for null). Handles are
// mutable references by design, so constness is shed here (as the JS bindings do).
static i32 cell_to_handle(DOMHostInstance& instance, GC::Cell const& cell)
{
    return instance.allocate_handle_for_cell(const_cast<GC::Cell&>(cell));
}

template<typename T>
static Wasm::Result handle_out(DOMHostInstance& instance, T&& value)
{
    using Bare = RemoveCVReference<T>;
    if constexpr (IsSpecializationOf<Bare, GC::Ptr> || IsPointer<Bare>) {
        if (!value)
            return return_i32(0);
        return return_i32(cell_to_handle(instance, *value));
    } else {
        return return_i32(cell_to_handle(instance, *value));
    }
}

// Runners: templated so that if constexpr genuinely discards the branches that
// do not apply to the member's return type (T, void, ExceptionOr<T/void>).

template<typename F>
static Wasm::Result run_status(DOMHostInstance& instance, F invoke)
{
    using Outcome = decltype(invoke());
    if constexpr (SameAs<Outcome, void>) {
        invoke();
        return return_i32(0);
    } else if constexpr (IsWasmDOMExceptionOr<Outcome>) {
        auto outcome = invoke();
        if (outcome.is_exception()) {
            instance.set_last_error(outcome.exception());
            return return_i32(-1);
        }
        return return_i32(0);
    } else {
        (void)invoke();
        return return_i32(0);
    }
}

template<typename WasmType, typename F>
static Wasm::Result run_numeric(DOMHostInstance& instance, F invoke)
{
    auto make = [](WasmType value) { return Wasm::Result { Vector<Wasm::Value> { Wasm::Value(value) } }; };
    auto outcome = invoke();
    if constexpr (IsWasmDOMExceptionOr<decltype(outcome)>) {
        if (outcome.is_exception()) {
            instance.set_last_error(outcome.exception());
            return make(static_cast<WasmType>(0));
        }
        return make(static_cast<WasmType>(outcome.release_value()));
    } else {
        return make(static_cast<WasmType>(outcome));
    }
}

template<typename F>
static Wasm::Result run_string_out(DOMHostInstance& instance, F invoke, u32 destination, u32 capacity)
{
    auto outcome = invoke();
    if constexpr (IsWasmDOMExceptionOr<decltype(outcome)>) {
        if (outcome.is_exception()) {
            instance.set_last_error(outcome.exception());
            return return_i32(-2);
        }
        return string_out(instance, outcome.release_value(), destination, capacity);
    } else {
        return string_out(instance, outcome, destination, capacity);
    }
}

template<typename F>
static Wasm::Result run_handle_out(DOMHostInstance& instance, F invoke)
{
    auto outcome = invoke();
    if constexpr (IsWasmDOMExceptionOr<decltype(outcome)>) {
        if (outcome.is_exception()) {
            instance.set_last_error(outcome.exception());
            return return_i32(0);
        }
        return handle_out(instance, outcome.release_value());
    } else {
        return handle_out(instance, outcome);
    }
}

"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output-path")
    parser.add_argument("--stats", action="store_true")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()

    paths = args.paths
    if len(paths) == 1 and paths[0].startswith("@"):
        paths = Path(paths[0][1:]).read_text().split()

    modules = []
    for path in paths:
        path = Path(path)
        modules.append(parse_module(path, path.read_text()))
    context = GenerationContext(modules)

    if args.stats:
        run_stats(context)
        return 0

    output_dir = Path(args.output_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    run_emit(context, output_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
