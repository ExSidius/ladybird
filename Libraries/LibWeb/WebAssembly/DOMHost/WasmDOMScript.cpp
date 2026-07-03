/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAssembly/DOMHost/WasmDOMScript.h>

namespace Web::WebAssembly::DOMHost {

GC_DEFINE_ALLOCATOR(WasmDOMScript);

GC::Ref<WasmDOMScript> WasmDOMScript::create(URL::URL base_url, ByteString filename, HTML::EnvironmentSettingsObject& settings, DOM::Document& document, NonnullRefPtr<Wasm::Module> module)
{
    auto& heap = settings.vm().heap();
    auto host_instance = DOMHostInstance::create(heap, document, move(module));
    auto script = heap.allocate<WasmDOMScript>(move(base_url), move(filename), settings, host_instance);
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());
    return script;
}

WasmDOMScript::WasmDOMScript(URL::URL base_url, ByteString filename, HTML::EnvironmentSettingsObject& settings, GC::Ref<DOMHostInstance> host_instance)
    : Script(move(base_url), move(filename), settings)
    , m_host_instance(host_instance)
{
}

WasmDOMScript::~WasmDOMScript() = default;

void WasmDOMScript::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host_instance);
}

bool WasmDOMScript::run()
{
    // Note the absence of prepare_to_run_script(): nothing here touches the JS
    // execution context stack, because no JS runs.
    if (auto result = m_host_instance->link_and_instantiate(); result.is_error()) {
        dbgln("wasm-dom: {}: {}", filename(), result.error());
        return false;
    }
    if (auto result = m_host_instance->invoke_start(); result.is_error()) {
        dbgln("wasm-dom: {}: {}", filename(), result.error());
        return false;
    }
    return true;
}

}
