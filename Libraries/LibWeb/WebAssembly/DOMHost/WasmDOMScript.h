/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/WebAssembly/DOMHost/DOMHostInstance.h>

namespace Web::WebAssembly::DOMHost {

// Non-standard: the script-element result for <script type="application/wasm-dom">.
// Unlike ClassicScript, running one never enters the JS execution context — the
// module executes on the wasm abstract machine and reaches the DOM only through
// the "dom" host interface.
class WasmDOMScript final : public HTML::Script {
    GC_CELL(WasmDOMScript, HTML::Script);
    GC_DECLARE_ALLOCATOR(WasmDOMScript);

public:
    virtual ~WasmDOMScript() override;

    static GC::Ref<WasmDOMScript> create(URL::URL base_url, ByteString filename, HTML::EnvironmentSettingsObject&, DOM::Document&, NonnullRefPtr<Wasm::Module>);

    // Link, instantiate, and invoke the module's exported _start. Returns false on
    // any failure (link error, missing exports, trap) after logging the reason.
    bool run();

private:
    WasmDOMScript(URL::URL base_url, ByteString filename, HTML::EnvironmentSettingsObject&, GC::Ref<DOMHostInstance>);

    virtual bool is_wasm_dom_script() const final { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOMHostInstance> m_host_instance;
};

}

template<>
inline bool JS::Script::HostDefined::fast_is<Web::WebAssembly::DOMHost::WasmDOMScript>() const { return is_wasm_dom_script(); }
