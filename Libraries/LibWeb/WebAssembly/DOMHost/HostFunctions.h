/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly::DOMHost {

// One entry of the "dom" host interface. Signatures are encoded as one char per
// value: 'i' = i32, 'I' = i64, 'f' = f32, 'd' = f64.
struct HostFunctionSpec {
    StringView name;
    Wasm::Result (*function)(DOMHostInstance&, Span<Wasm::Value>);
    StringView parameters;
    StringView results;
};

// The WebIDL-generated portion of the interface (DOMHostGeneratedFunctions.cpp,
// emitted by Meta/Generators/generate_wasm_dom_bindings.py).
ReadonlySpan<HostFunctionSpec> generated_host_function_specs();

// Resolve every import of the module against the "dom" host interface, allocating
// host functions in the instance's own store. This is the JS-free analogue of the
// import-object resolution in LibWeb/WebAssembly/WebAssembly.cpp — no JS values,
// no realm; the functions call straight into LibWeb's C++ DOM. Hand-written
// entries win over generated ones on name collision.
ErrorOr<HashMap<Wasm::Linker::Name, Wasm::ExternValue>, ByteString> resolve_dom_imports(Wasm::Linker&, DOMHostInstance&);

}
