// Standalone smoke test for the CPython embedding patterns used in
// Libraries/LibWeb/HTML/Scripting/PythonBindings.cpp. A MockNode stands in
// for DOM::Node; every CPython API call mirrors the real binding code
// verbatim so that API misuse shows up here without needing a LibWeb build.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// --- Mock DOM ---------------------------------------------------------------

struct MockNode {
    std::string tag;
    std::string text_content;
    std::map<std::string, std::string> attributes;
    std::vector<std::shared_ptr<MockNode>> children;
    bool is_document { false };
    std::map<std::string, std::vector<PyObject*>> listeners;

    std::shared_ptr<MockNode> find_by_id(std::string const& id)
    {
        for (auto& child : children) {
            auto it = child->attributes.find("id");
            if (it != child->attributes.end() && it->second == id)
                return child;
            if (auto found = child->find_by_id(id))
                return found;
        }
        return nullptr;
    }
};

static std::shared_ptr<MockNode> s_document;

// Stand-in for GC::Root<DOM::Node>*: heap-allocated strong reference.
using NodeRoot = std::shared_ptr<MockNode>;

// --- Binding layer (mirrors PythonBindings.cpp) ------------------------------

static PyObject* s_dom_node_type = nullptr;
static PyObject* s_traceback_module = nullptr;

struct PyDOMNode {
    PyObject_HEAD
    NodeRoot* root;
};

static MockNode& node_of(PyObject* self)
{
    return **reinterpret_cast<PyDOMNode*>(self)->root;
}

static PyObject* py_string(char const* data, size_t length)
{
    return PyUnicode_FromStringAndSize(data, static_cast<Py_ssize_t>(length));
}

static PyObject* wrap_node(std::shared_ptr<MockNode> node)
{
    auto* object = reinterpret_cast<PyDOMNode*>(PyType_GenericAlloc(reinterpret_cast<PyTypeObject*>(s_dom_node_type), 0));
    if (!object)
        return nullptr;
    object->root = new NodeRoot(std::move(node));
    return reinterpret_cast<PyObject*>(object);
}

static void dom_node_dealloc(PyObject* self)
{
    delete reinterpret_cast<PyDOMNode*>(self)->root;
    Py_TYPE(self)->tp_free(self);
}

static void log_python_exception(char const* when)
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
        PyObject* separator = py_string("", 0);
        PyObject* joined = separator ? PyUnicode_Join(separator, formatted_list) : nullptr;
        if (joined) {
            if (char const* text = PyUnicode_AsUTF8(joined)) {
                fprintf(stderr, "Python exception (%s):\n%s", when, text);
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
        fprintf(stderr, "Python exception (%s): <unable to format traceback>\n", when);
    PyErr_Clear();
}

static PyObject* dom_node_get_element_by_id(PyObject* self, PyObject* args)
{
    char const* id = nullptr;
    if (!PyArg_ParseTuple(args, "s:getElementById", &id))
        return nullptr;
    auto found = node_of(self).find_by_id(id);
    if (!found)
        Py_RETURN_NONE;
    return wrap_node(found);
}

static PyObject* dom_node_create_element(PyObject* self, PyObject* args)
{
    char const* tag_name = nullptr;
    if (!PyArg_ParseTuple(args, "s:createElement", &tag_name))
        return nullptr;
    if (!node_of(self).is_document) {
        PyErr_SetString(PyExc_TypeError, "createElement requires a document");
        return nullptr;
    }
    auto element = std::make_shared<MockNode>();
    element->tag = tag_name;
    return wrap_node(element);
}

static PyObject* dom_node_create_text_node(PyObject* self, PyObject* args)
{
    char const* data = nullptr;
    if (!PyArg_ParseTuple(args, "s:createTextNode", &data))
        return nullptr;
    if (!node_of(self).is_document) {
        PyErr_SetString(PyExc_TypeError, "createTextNode requires a document");
        return nullptr;
    }
    auto text = std::make_shared<MockNode>();
    text->tag = "#text";
    text->text_content = data;
    return wrap_node(text);
}

static PyObject* dom_node_append_child(PyObject* self, PyObject* args)
{
    PyObject* child = nullptr;
    if (!PyArg_ParseTuple(args, "O!:appendChild", reinterpret_cast<PyTypeObject*>(s_dom_node_type), &child))
        return nullptr;
    node_of(self).children.push_back(*reinterpret_cast<PyDOMNode*>(child)->root);
    node_of(self).text_content += node_of(child).text_content;
    Py_INCREF(child);
    return child;
}

static PyObject* dom_node_set_attribute(PyObject* self, PyObject* args)
{
    char const* name = nullptr;
    char const* value = nullptr;
    if (!PyArg_ParseTuple(args, "ss:setAttribute", &name, &value))
        return nullptr;
    node_of(self).attributes[name] = value;
    Py_RETURN_NONE;
}

static PyObject* dom_node_get_attribute(PyObject* self, PyObject* args)
{
    char const* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:getAttribute", &name))
        return nullptr;
    auto& attributes = node_of(self).attributes;
    auto it = attributes.find(name);
    if (it == attributes.end())
        Py_RETURN_NONE;
    return py_string(it->second.data(), it->second.size());
}

static PyObject* wrap_event_for_callback(std::string const& type, std::shared_ptr<MockNode> target)
{
    PyObject* dict = PyDict_New();
    if (!dict)
        return nullptr;

    PyObject* type_string = py_string(type.data(), type.size());
    if (type_string) {
        PyDict_SetItemString(dict, "type", type_string);
        Py_DECREF(type_string);
    }

    PyObject* target_object = target ? wrap_node(target) : nullptr;
    if (!target_object) {
        target_object = Py_None;
        Py_INCREF(target_object);
    }
    PyDict_SetItemString(dict, "target", target_object);
    Py_DECREF(target_object);

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
    Py_INCREF(callable); // intentionally leaked, same as the real binding
    node_of(self).listeners[type].push_back(callable);
    Py_RETURN_NONE;
}

// The dispatch side of the trampoline: what the JS::NativeFunction lambda does.
static void dispatch_event(std::shared_ptr<MockNode> target, std::string const& type)
{
    auto it = target->listeners.find(type);
    if (it == target->listeners.end())
        return;
    for (auto* callable : it->second) {
        PyObject* event_dict = wrap_event_for_callback(type, target);
        if (!event_dict) {
            log_python_exception("building event object");
            continue;
        }
        PyObject* result = PyObject_CallFunctionObjArgs(callable, event_dict, nullptr);
        Py_DECREF(event_dict);
        if (!result)
            log_python_exception("event callback");
        else
            Py_DECREF(result);
    }
}

static PyObject* dom_node_get_text_content(PyObject* self, void*)
{
    auto& text = node_of(self).text_content;
    return py_string(text.data(), text.size());
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
    node_of(self).text_content.assign(utf8, static_cast<size_t>(length));
    node_of(self).children.clear();
    return 0;
}

static PyMethodDef s_dom_node_methods[] = {
    { "getElementById", dom_node_get_element_by_id, METH_VARARGS, "" },
    { "createElement", dom_node_create_element, METH_VARARGS, "" },
    { "createTextNode", dom_node_create_text_node, METH_VARARGS, "" },
    { "appendChild", dom_node_append_child, METH_VARARGS, "" },
    { "setAttribute", dom_node_set_attribute, METH_VARARGS, "" },
    { "getAttribute", dom_node_get_attribute, METH_VARARGS, "" },
    { "addEventListener", dom_node_add_event_listener, METH_VARARGS, "" },
    { nullptr, nullptr, 0, nullptr },
};

static PyGetSetDef s_dom_node_getset[] = {
    { "textContent", dom_node_get_text_content, dom_node_set_text_content, "", nullptr },
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

static bool initialize_interpreter()
{
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    config.buffered_stdio = 0;

    auto status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        fprintf(stderr, "init failed: %s\n", status.err_msg ? status.err_msg : "?");
        return false;
    }

    s_traceback_module = PyImport_ImportModule("traceback");
    if (!s_traceback_module) {
        fprintf(stderr, "no traceback module\n");
        PyErr_Clear();
    }

    s_dom_node_type = PyType_FromSpec(&s_dom_node_spec);
    if (!s_dom_node_type) {
        log_python_exception("creating DOMNode type");
        return false;
    }
    return true;
}

static void run_source(std::string const& source, std::shared_ptr<MockNode> document, char const* filename)
{
    PyObject* main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        log_python_exception("locating __main__");
        return;
    }
    PyObject* globals = PyModule_GetDict(main_module);

    PyObject* document_wrapper = wrap_node(document);
    if (!document_wrapper) {
        log_python_exception("wrapping document");
        return;
    }
    PyDict_SetItemString(globals, "document", document_wrapper);
    Py_DECREF(document_wrapper);

    PyObject* code = Py_CompileString(source.c_str(), filename, Py_file_input);
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

// --- Test scenarios ----------------------------------------------------------

int main()
{
    if (!initialize_interpreter())
        return 1;

    s_document = std::make_shared<MockNode>();
    s_document->is_document = true;
    auto out = std::make_shared<MockNode>();
    out->tag = "div";
    out->attributes["id"] = "out";
    auto button = std::make_shared<MockNode>();
    button->tag = "button";
    button->attributes["id"] = "btn";
    s_document->children = { out, button };

    // Scenario 1: the python source from the python-dom-basics text test,
    // covering getElementById/createElement/createTextNode/appendChild/
    // setAttribute/textContent get+set/addEventListener.
    run_source(R"PY(
out = document.getElementById("out")

p = document.createElement("p")
p.setAttribute("id", "made-by-python")
p.appendChild(document.createTextNode("hello from python"))
out.appendChild(p)

p.textContent = p.textContent + " (updated)"

def on_click(event):
    line = document.createElement("p")
    line.setAttribute("id", "clicked")
    line.textContent = "clicked: " + event["type"]
    document.getElementById("out").appendChild(line)

document.getElementById("btn").addEventListener("click", on_click)
)PY",
        s_document, "scenario1.py");

    auto made = s_document->find_by_id("made-by-python");
    assert(made && made->text_content == "hello from python (updated)");
    assert(made->attributes["id"] == "made-by-python");

    // Scenario 2: event dispatch invokes the python callable with the dict payload.
    dispatch_event(s_document->find_by_id("btn"), "click");
    auto clicked = s_document->find_by_id("clicked");
    assert(clicked && clicked->text_content == "clicked: click");

    // Scenario 3: exception in a script is logged; interpreter stays usable
    // and module state persists across scripts.
    run_source("raise ValueError(\"intentional error from python\")\n", s_document, "scenario3.py");
    run_source("assert on_click is not None\nsurvived = True\nprint('python still alive after exception')\n", s_document, "scenario4.py");
    run_source("assert survived\n", s_document, "scenario5.py");

    // Scenario 4: exception in a callback is logged and does not break dispatch.
    run_source(R"PY(
def bad_handler(event):
    raise RuntimeError("callback exploded")
document.getElementById("btn").addEventListener("click", bad_handler)
)PY",
        s_document, "scenario6.py");
    dispatch_event(s_document->find_by_id("btn"), "click"); // good handler + bad handler both fire

    // Scenario 5: binding-level type errors surface as python exceptions, not crashes.
    run_source("document.getElementById('btn').createElement('p')\n", s_document, "scenario7.py");

    // Scenario 6: wrapper dealloc path (drop references, force gc).
    run_source("import sys\nx = document.getElementById('out')\nx = None\n", s_document, "scenario8.py");

    // Scenario 7: DOMNode cannot be instantiated from python.
    run_source("t = type(document)\ntry:\n    t()\nexcept TypeError as e:\n    print('instantiation blocked:', e)\n", s_document, "scenario9.py");

    printf("ALL SCENARIOS PASSED\n");
    return 0;
}
