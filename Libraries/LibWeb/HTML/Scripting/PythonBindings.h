/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

// Prototype embedded-CPython support for <script type="text/python">.
// All CPython API usage is confined to PythonBindings.cpp; this header must
// stay free of Python.h so the rest of LibWeb never sees CPython types.
namespace Web::HTML::Python {

// Initializes the embedded interpreter once per process. Must be called before
// any process sandbox is applied, because CPython needs filesystem access to
// its standard library during startup. Idempotent; returns false if the
// interpreter is unavailable (in which case python scripts become no-ops).
WEB_API bool initialize_interpreter();

// Executes Python source against the given document. The script runs in the
// persistent __main__ namespace with a `document` global bound to `document`.
// Uncaught Python exceptions are logged and swallowed; they never propagate.
WEB_API void run_source(ByteString const& source, DOM::Document&, ByteString const& filename);

}
