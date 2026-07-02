/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/PythonBindings.h>
#include <LibWeb/HTML/Scripting/PythonScript.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PythonScript);

GC::Ref<PythonScript> PythonScript::create(ByteString filename, StringView source, EnvironmentSettingsObject& settings, URL::URL base_url)
{
    auto& vm = settings.vm();

    auto script = vm.heap().allocate<PythonScript>(move(base_url), move(filename), settings);
    script->set_parse_error(JS::js_null());
    script->set_error_to_rethrow(JS::js_null());

    if (is_scripting_disabled(settings))
        source = ""sv;
    script->m_source = source;

    return script;
}

// Mirrors the skeleton of "run a classic script" (ClassicScript::run): the
// prepare/clean-up pair keeps the settings object's execution-context
// bookkeeping consistent even though evaluation happens in CPython rather
// than LibJS. Python exceptions are logged inside run_source and never
// propagate as JS completions.
void PythonScript::run()
{
    auto& settings = settings_object();

    if (can_run_script(settings) == RunScriptDecision::DoNotRun)
        return;

    prepare_to_run_script(settings);
    ScopeGuard clean_up_guard = [&] {
        clean_up_after_running_script(settings);
    };

    if (!is<Window>(settings.global_object()))
        return;
    auto& document = as<Window>(settings.global_object()).associated_document();

    Python::run_source(m_source, document, filename());
}

PythonScript::PythonScript(URL::URL base_url, ByteString filename, EnvironmentSettingsObject& settings)
    : Script(move(base_url), move(filename), settings)
{
}

PythonScript::~PythonScript() = default;

}
