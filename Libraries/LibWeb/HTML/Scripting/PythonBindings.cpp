/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Python.h must come first and wants this defined for Py_ssize_t argument parsing.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <AK/Format.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/PythonBindings.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML::Python {

enum class InterpreterState {
    Uninitialized,
    Available,
    Unavailable,
};

static InterpreterState s_interpreter_state { InterpreterState::Uninitialized };
static PyObject* s_dom_node_type { nullptr };
static PyObject* s_traceback_module { nullptr };

// A Python object wrapping a DOM node. The GC::Root keeps the node (and
// therefore its document) alive for as long as the Python wrapper exists;
// wrappers are not identity-cached, so two lookups of the same element yield
// distinct Python objects.
struct PyDOMNode {
    PyObject_HEAD
    GC::Root<DOM::Node>* root;
};

static DOM::Node& node_of(PyObject* self)
{
    return **reinterpret_cast<PyDOMNode*>(self)->root;
}

static PyObject* py_string(StringView view)
{
    return PyUnicode_FromStringAndSize(view.characters_without_null_termination(), static_cast<Py_ssize_t>(view.length()));
}

static PyObject* wrap_node(DOM::Node& node)
{
    auto* object = reinterpret_cast<PyDOMNode*>(PyType_GenericAlloc(reinterpret_cast<PyTypeObject*>(s_dom_node_type), 0));
    if (!object)
        return nullptr;
    object->root = new GC::Root<DOM::Node>(GC::make_root(node));
    return reinterpret_cast<PyObject*>(object);
}

static void dom_node_dealloc(PyObject* self)
{
    delete reinterpret_cast<PyDOMNode*>(self)->root;
    Py_TYPE(self)->tp_free(self);
}

static void log_python_exception(StringView when)
{
    if (!PyErr_Occurred())
        return;

#if PY_VERSION_HEX >= 0x030c0000
    PyObject* exception = PyErr_GetRaisedException();
    PyObject* formatted_list = nullptr;
    if (exception && s_traceback_module)
        formatted_list = PyObject_CallMethod(s_traceback_module, "format_exception", "O", exception);
#else
    PyObject *exception_type, *exception_value, *exception_traceback;
    PyErr_Fetch(&exception_type, &exception_value, &exception_traceback);
    PyErr_NormalizeException(&exception_type, &exception_value, &exception_traceback);
    PyObject* formatted_list = nullptr;
    if (s_traceback_module) {
        formatted_list = PyObject_CallMethod(s_traceback_module, "format_exception", "OOO",
            exception_type ? exception_type : Py_None,
            exception_value ? exception_value : Py_None,
            exception_traceback ? exception_traceback : Py_None);
    }
#endif

    bool logged = false;
    if (formatted_list) {
        PyObject* separator = py_string(""sv);
        PyObject* joined = separator ? PyUnicode_Join(separator, formatted_list) : nullptr;
        if (joined) {
            if (char const* text = PyUnicode_AsUTF8(joined)) {
                dbgln("Python exception ({}):\n{}", when, StringView { text, strlen(text) });
                logged = true;
            }
        }
        Py_XDECREF(joined);
        Py_XDECREF(separator);
        Py_XDECREF(formatted_list);
    }

#if PY_VERSION_HEX >= 0x030c0000
    if (!logged && exception) {
        PyErr_SetRaisedException(exception);
        exception = nullptr;
        PyErr_Print();
        logged = true;
    }
    Py_XDECREF(exception);
#else
    if (!logged && exception_type) {
        PyErr_Restore(exception_type, exception_value, exception_traceback);
        exception_type = exception_value = exception_traceback = nullptr;
        PyErr_Print();
        logged = true;
    }
    Py_XDECREF(exception_type);
    Py_XDECREF(exception_value);
    Py_XDECREF(exception_traceback);
#endif

    if (!logged)
        dbgln("Python exception ({}): <unable to format traceback>", when);
    PyErr_Clear();
}

static PyObject* dom_node_get_element_by_id(PyObject* self, PyObject* args)
{
    char const* id = nullptr;
    if (!PyArg_ParseTuple(args, "s:getElementById", &id))
        return nullptr;

    auto& node = node_of(self);
    if (!is<DOM::ParentNode>(node)) {
        PyErr_SetString(PyExc_TypeError, "getElementById requires a document or element");
        return nullptr;
    }

    auto element = static_cast<DOM::ParentNode&>(node).get_element_by_id(MUST(FlyString::from_utf8(StringView { id, strlen(id) })));
    if (!element)
        Py_RETURN_NONE;
    return wrap_node(*element);
}

static PyObject* dom_node_create_element(PyObject* self, PyObject* args)
{
    char const* tag_name = nullptr;
    if (!PyArg_ParseTuple(args, "s:createElement", &tag_name))
        return nullptr;

    auto& node = node_of(self);
    if (!is<DOM::Document>(node)) {
        PyErr_SetString(PyExc_TypeError, "createElement requires a document");
        return nullptr;
    }

    auto element_or_error = static_cast<DOM::Document&>(node).create_element(
        MUST(String::from_utf8(StringView { tag_name, strlen(tag_name) })), Variant<String, Bindings::ElementCreationOptions> { String {} });
    if (element_or_error.is_error()) {
        PyErr_SetString(PyExc_RuntimeError, "createElement failed (invalid tag name?)");
        return nullptr;
    }
    return wrap_node(*element_or_error.release_value());
}

static PyObject* dom_node_create_text_node(PyObject* self, PyObject* args)
{
    char const* data = nullptr;
    if (!PyArg_ParseTuple(args, "s:createTextNode", &data))
        return nullptr;

    auto& node = node_of(self);
    if (!is<DOM::Document>(node)) {
        PyErr_SetString(PyExc_TypeError, "createTextNode requires a document");
        return nullptr;
    }

    auto text = static_cast<DOM::Document&>(node).create_text_node(Utf16String::from_utf8(StringView { data, strlen(data) }));
    return wrap_node(*text);
}

static PyObject* dom_node_append_child(PyObject* self, PyObject* args)
{
    PyObject* child = nullptr;
    if (!PyArg_ParseTuple(args, "O!:appendChild", reinterpret_cast<PyTypeObject*>(s_dom_node_type), &child))
        return nullptr;

    auto result = node_of(self).append_child(node_of(child));
    if (result.is_error()) {
        PyErr_SetString(PyExc_RuntimeError, "appendChild failed (hierarchy error?)");
        return nullptr;
    }
    Py_INCREF(child);
    return child;
}

static PyObject* dom_node_set_attribute(PyObject* self, PyObject* args)
{
    char const* name = nullptr;
    char const* value = nullptr;
    if (!PyArg_ParseTuple(args, "ss:setAttribute", &name, &value))
        return nullptr;

    auto& node = node_of(self);
    if (!is<DOM::Element>(node)) {
        PyErr_SetString(PyExc_TypeError, "setAttribute requires an element");
        return nullptr;
    }

    static_cast<DOM::Element&>(node).set_attribute_value(
        MUST(FlyString::from_utf8(StringView { name, strlen(name) })),
        MUST(String::from_utf8(StringView { value, strlen(value) })));
    Py_RETURN_NONE;
}

static PyObject* dom_node_get_attribute(PyObject* self, PyObject* args)
{
    char const* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:getAttribute", &name))
        return nullptr;

    auto& node = node_of(self);
    if (!is<DOM::Element>(node)) {
        PyErr_SetString(PyExc_TypeError, "getAttribute requires an element");
        return nullptr;
    }

    auto value = static_cast<DOM::Element&>(node).get_attribute(MUST(FlyString::from_utf8(StringView { name, strlen(name) })));
    if (!value.has_value())
        Py_RETURN_NONE;
    return py_string(value->bytes_as_string_view());
}

static PyObject* wrap_event_for_callback(DOM::Event& event)
{
    PyObject* dict = PyDict_New();
    if (!dict)
        return nullptr;

    PyObject* type = py_string(event.type().bytes_as_string_view());
    if (type) {
        PyDict_SetItemString(dict, "type", type);
        Py_DECREF(type);
    }

    PyObject* target = nullptr;
    if (auto event_target = event.target(); event_target && is<DOM::Node>(*event_target))
        target = wrap_node(static_cast<DOM::Node&>(*event_target));
    if (!target) {
        target = Py_None;
        Py_INCREF(target);
    }
    PyDict_SetItemString(dict, "target", target);
    Py_DECREF(target);

    return dict;
}

static PyObject* dom_node_add_event_listener(PyObject* self, PyObject* args)
{
    char const* type = nullptr;
    PyObject* callable = nullptr;
    if (!PyArg_ParseTuple(args, "sO:addEventListener", &type, &callable))
        return nullptr;
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "addEventListener requires a callable");
        return nullptr;
    }

    auto& node = node_of(self);
    auto& realm = relevant_realm(node);

    // The callable is intentionally leaked: listeners registered by this
    // prototype live for the lifetime of the process.
    Py_INCREF(callable);

    // Same shape as the engine-internal listener helpers (see
    // Streams/AbstractOperations.cpp add_message_event_listener): a native
    // function boxed as a WebIDL callback. Event dispatch has already pushed
    // an execution context by the time this runs.
    auto behavior = [callable](JS::VM& vm) -> JS::Value {
        auto& event = vm.argument(0).as<DOM::Event>();
        PyObject* event_dict = wrap_event_for_callback(event);
        if (!event_dict) {
            log_python_exception("building event object"sv);
            return JS::js_undefined();
        }
        PyObject* result = PyObject_CallFunctionObjArgs(callable, event_dict, nullptr);
        Py_DECREF(event_dict);
        if (!result)
            log_python_exception("event callback"sv);
        else
            Py_DECREF(result);
        return JS::js_undefined();
    };

    auto function = JS::NativeFunction::create(realm, move(behavior), 1, Utf16FlyString {}, &realm);
    auto callback = realm.heap().allocate<WebIDL::CallbackType>(function, realm);
    auto listener = DOM::IDLEventListener::create(realm, callback);
    node.add_event_listener_without_options(MUST(FlyString::from_utf8(StringView { type, strlen(type) })), listener);

    Py_RETURN_NONE;
}

static PyObject* dom_node_get_text_content(PyObject* self, void*)
{
    auto text = node_of(self).text_content();
    if (!text.has_value())
        Py_RETURN_NONE;
    auto utf8 = text->to_well_formed_utf8();
    return py_string(utf8.bytes_as_string_view());
}

static int dom_node_set_text_content(PyObject* self, PyObject* value, void*)
{
    if (!value || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "textContent must be a string");
        return -1;
    }
    Py_ssize_t length = 0;
    char const* utf8 = PyUnicode_AsUTF8AndSize(value, &length);
    if (!utf8)
        return -1;

    auto result = node_of(self).set_text_content(Utf16String::from_utf8(StringView { utf8, static_cast<size_t>(length) }));
    if (result.is_error()) {
        PyErr_SetString(PyExc_RuntimeError, "setting textContent failed");
        return -1;
    }
    return 0;
}

static PyMethodDef s_dom_node_methods[] = {
    { "getElementById", dom_node_get_element_by_id, METH_VARARGS, "Find a descendant element by its id attribute" },
    { "createElement", dom_node_create_element, METH_VARARGS, "Create an element (documents only)" },
    { "createTextNode", dom_node_create_text_node, METH_VARARGS, "Create a text node (documents only)" },
    { "appendChild", dom_node_append_child, METH_VARARGS, "Append a node as the last child of this node" },
    { "setAttribute", dom_node_set_attribute, METH_VARARGS, "Set an attribute (elements only)" },
    { "getAttribute", dom_node_get_attribute, METH_VARARGS, "Get an attribute value or None (elements only)" },
    { "addEventListener", dom_node_add_event_listener, METH_VARARGS, "Register a callable for a DOM event type" },
    { nullptr, nullptr, 0, nullptr },
};

static PyGetSetDef s_dom_node_getset[] = {
    { "textContent", dom_node_get_text_content, dom_node_set_text_content, "Text content of this node and its descendants", nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr },
};

static PyType_Slot s_dom_node_slots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(dom_node_dealloc) },
    { Py_tp_methods, s_dom_node_methods },
    { Py_tp_getset, s_dom_node_getset },
    { 0, nullptr },
};

static PyType_Spec s_dom_node_spec = {
    .name = "ladybird.DOMNode",
    .basicsize = sizeof(PyDOMNode),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots = s_dom_node_slots,
};

bool initialize_interpreter()
{
    if (s_interpreter_state != InterpreterState::Uninitialized)
        return s_interpreter_state == InterpreterState::Available;
    s_interpreter_state = InterpreterState::Unavailable;

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    // Unbuffered stdio, so print() output is visible immediately: the
    // interpreter lives as long as the process and is never finalized, so
    // buffered output would otherwise be lost or arbitrarily delayed.
    config.buffered_stdio = 0;

    // Py_InitializeFromConfig reports failure via PyStatus instead of
    // aborting the process like Py_Initialize does.
    auto status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        dbgln("Python: interpreter initialization failed: {}",
            status.err_msg ? StringView { status.err_msg, strlen(status.err_msg) } : "unknown error"sv);
        return false;
    }

    // Pre-import traceback while the filesystem is still fully accessible;
    // the process sandbox applied later would block loading it on demand.
    s_traceback_module = PyImport_ImportModule("traceback");
    if (!s_traceback_module) {
        dbgln("Python: failed to import traceback module; tracebacks will use PyErr_Print");
        PyErr_Clear();
    }

    s_dom_node_type = PyType_FromSpec(&s_dom_node_spec);
    if (!s_dom_node_type) {
        log_python_exception("creating DOMNode type"sv);
        return false;
    }

    s_interpreter_state = InterpreterState::Available;
    return true;
}

void run_source(ByteString const& source, DOM::Document& document, ByteString const& filename)
{
    if (!initialize_interpreter())
        return;

    // Scripts share the persistent __main__ namespace so that module-level
    // state (and event callbacks) survive across <script> elements.
    PyObject* main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        log_python_exception("locating __main__"sv);
        return;
    }
    PyObject* globals = PyModule_GetDict(main_module); // borrowed

    PyObject* document_wrapper = wrap_node(document);
    if (!document_wrapper) {
        log_python_exception("wrapping document"sv);
        return;
    }
    PyDict_SetItemString(globals, "document", document_wrapper);
    Py_DECREF(document_wrapper);

    // Compile separately from evaluation so tracebacks carry the document URL
    // as the file name.
    PyObject* code = Py_CompileString(source.characters(), filename.characters(), Py_file_input);
    if (!code) {
        log_python_exception(filename);
        return;
    }

    PyObject* result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result)
        log_python_exception(filename);
    else
        Py_DECREF(result);
}

}
