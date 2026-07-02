/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

// Prototype counterpart of ClassicScript for <script type="text/python">.
// Holds raw Python source; evaluation is delegated to the embedded CPython
// interpreter (see PythonBindings.h). There is no parse step at creation
// time, so syntax errors surface at run() like any other Python exception.
class WEB_API PythonScript final : public Script {
    GC_CELL(PythonScript, Script);
    GC_DECLARE_ALLOCATOR(PythonScript);

public:
    virtual ~PythonScript() override;

    static GC::Ref<PythonScript> create(ByteString filename, StringView source, EnvironmentSettingsObject&, URL::URL base_url);

    void run();

private:
    PythonScript(URL::URL base_url, ByteString filename, EnvironmentSettingsObject&);

    ByteString m_source;
};

}
