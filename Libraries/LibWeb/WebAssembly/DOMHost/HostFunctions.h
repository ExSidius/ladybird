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

// Resolve every import of the module against the "dom" host interface, allocating
// host functions in the instance's own store. This is the JS-free analogue of the
// import-object resolution in LibWeb/WebAssembly/WebAssembly.cpp — no JS values,
// no realm; the functions call straight into LibWeb's C++ DOM.
ErrorOr<HashMap<Wasm::Linker::Name, Wasm::ExternValue>, ByteString> resolve_dom_imports(Wasm::Linker&, DOMHostInstance&);

}
