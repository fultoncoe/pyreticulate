
#include "libpython.h"

#define RCPP_NO_MODULES
#define RCPP_NO_SUGAR

#include <Rcpp.h>
using namespace Rcpp;

#include "signals.h"
#include "reticulate_types.h"
#include "common.h"

#include "event_loop.h"
#include "tinythread.h"

#include <fstream>
#include <time.h>

#ifndef _WIN32
#include <dlfcn.h>
#else
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#endif

using namespace reticulate::libpython;

// track whether we are using python 3 (set during py_initialize)
bool s_isPython3 = false;

// [[Rcpp::export]]
bool is_python3() {
  return s_isPython3;
}

// track whether this is an interactive session
bool s_isInteractive = false;
bool is_interactive() {
  return s_isInteractive;
}

// a simplified version of loadSymbol adopted from libpython.cpp
void loadSymbol(void* pLib, const std::string& name, void** ppSymbol)
{
  *ppSymbol = NULL;
#ifdef _WIN32
  *ppSymbol = (void*) ::GetProcAddress((HINSTANCE)pLib, name.c_str());
#else
  *ppSymbol = ::dlsym(pLib, name.c_str());
#endif
}


// track whether we have required numpy
std::string s_numpy_load_error;
bool haveNumPy() {
  return s_numpy_load_error.empty();
}

bool requireNumPy() {
  if (!haveNumPy())
    stop("Required version of NumPy not available: " + s_numpy_load_error);
  return true;
}

bool isPyArray(PyObject* object) {
  if (!haveNumPy()) return false;

  return PyArray_Check(object);
}

bool isPyArrayScalar(PyObject* object) {
  if (!haveNumPy()) return false;

  return PyArray_CheckScalar(object);
}

// static buffers for Py_SetProgramName / Py_SetPythonHome
std::string s_python;
std::wstring s_python_v3;
std::string s_pythonhome;
std::wstring s_pythonhome_v3;



// helper to convert std::string to std::wstring
std::wstring to_wstring(const std::string& str) {
  std::wstring ws = std::wstring(str.size(), L' ');
  ws.resize(std::mbstowcs(&ws[0], str.c_str(), str.size()));
  return ws;
}

// helper to convert std::wstring to std::string
std::string to_string(const std::wstring& ws) {
  int maxnchar = ws.size() * 4;
  char *buffer = (char*) malloc(sizeof(char) * maxnchar);
  int nchar = wcstombs(buffer, ws.c_str(), maxnchar);
  std::string s(buffer, nchar);
  free(buffer);
  return s;
}


// forward declare error handling utility
SEXP py_fetch_error(bool maybe_reuse_cached_r_trace = false);


const char *r_object_string = "r_object";

// wrap an R object in a longer-lived python object "capsule"
SEXP py_capsule_read(PyObject* capsule) {

  SEXP object = (SEXP) PyCapsule_GetPointer(capsule, r_object_string);
  if (object == NULL)
    throw PythonException(py_fetch_error());

  // Rcpp_precious_preserve() returns a cell of a doubly linked list
  // with the original object preserved in the cell TAG().
  return TAG(object);

}

tthread::thread::id s_main_thread = 0;
bool is_main_thread() {
  if (s_main_thread == 0)
    return true;
  return s_main_thread == tthread::this_thread::get_id();
}


int free_sexp(void* sexp) {
  // wrap Rcpp_precious_remove() to satisfy
  // Py_AddPendingCall() signature and return value requirements
  Rcpp_precious_remove((SEXP) sexp);
  return 0;
}

void Rcpp_precious_remove_main_thread(SEXP object) {
  if (is_main_thread()) {
    return Rcpp_precious_remove(object);
  }

  // #Py_AddPendingCall can fail sometimes, so we retry a few times
  const size_t wait_ms = 100;
  size_t waited_ms = 0;
  while (Py_AddPendingCall(free_sexp, object) != 0) {

    tthread::this_thread::sleep_for(tthread::chrono::milliseconds(wait_ms));

    // increment total wait time and print a warning every 60 seconds
    waited_ms += wait_ms;
    if ((waited_ms % 60000) == 0)
        PySys_WriteStderr("Waiting to schedule object finalizer on main R interpeter thread...\n");
    else if (waited_ms > 60000 * 2) {
        // if we've waited more than 2 minutes, something is wrong
        PySys_WriteStderr("Error: unable to register R object finalizer on main thread\n");
        return;
    }
  }
}

void py_capsule_free(PyObject* capsule) {

  SEXP object = (SEXP)PyCapsule_GetPointer(capsule, r_object_string);
  if (object == NULL)
    throw PythonException(py_fetch_error());

  // the R api access must be from the main thread
  Rcpp_precious_remove_main_thread(object);
}

PyObject* py_capsule_new(SEXP object) {

  // if object == R_NilValue, this is a no-op, R_NilValue is reflected back.
  object = Rcpp_precious_preserve(object);

  return PyCapsule_New((void *)object, r_object_string, py_capsule_free);

}

PyObject* py_get_attr(PyObject* object, const std::string& name) {

  if (PyObject_HasAttrString(object, name.c_str()))
    return PyObject_GetAttrString(object, name.c_str());
  else
    return NULL;

}

bool is_r_object_capsule(PyObject* capsule) {
  return PyCapsule_IsValid(capsule, r_object_string);
}

// helper class for ensuring decref of PyObject in the current scope
template <typename T>
class PyPtr {

public:

  // attach on creation, decref on destruction
  PyPtr()
    : object_(NULL)
  {
  }

  explicit PyPtr(T* object)
    : object_(object)
  {
  }

  virtual ~PyPtr()
  {
    if (object_ != NULL) {
      Py_DecRef((PyObject*) object_);
    }
  }

  operator T*() const
  {
    return object_;
  }

  T* get() const
  {
    return object_;
  }

  void assign(T* object)
  {
    object_ = object;
  }

  T* detach()
  {
    T* object = object_;
    object_ = NULL;
    return object;
  }

  bool is_null() const
  {
    return object_ == NULL;
  }

private:

  // prevent copying
  PyPtr(const PyPtr&);
  PyPtr& operator=(const PyPtr&);

  // underlying object
  T* object_;
};

typedef PyPtr<PyObject> PyObjectPtr;
typedef PyPtr<PyArray_Descr> PyArray_DescrPtr;

inline PyObject* PyUnicode_AsBytes(PyObject* str) {
  return PyUnicode_AsEncodedString(str, /* encoding = */ NULL, /* errors = */ "ignore");
  // encoding = NULL  is fastpath to "utf-8"
}

PyObject* as_python_str(const std::string& str);

std::string as_std_string(PyObject* str) {

  // conver to bytes if its unicode
  PyObjectPtr pStr;
  if (PyUnicode_Check(str) || isPyArrayScalar(str)) {
    str = PyUnicode_AsBytes(str);
    pStr.assign(str);
  }

  char* buffer;
  Py_ssize_t length;
  int res = is_python3() ?
    PyBytes_AsStringAndSize(str, &buffer, &length) :
    PyString_AsStringAndSize(str, &buffer, &length);
  if (res == -1)
    throw PythonException(py_fetch_error());

  return std::string(buffer, length);
}

#define as_utf8_r_string(str) Rcpp::String(as_std_string(str))

PyObject* as_python_str(SEXP strSEXP, bool handle_na=false) {
  if (handle_na && strSEXP == NA_STRING) {
    Py_IncRef(Py_None);
    return Py_None;
  }

  if (is_python3()) {
    // python3 doesn't have PyString and all strings are unicode so
    // make sure we get a unicode representation from R
    const char * value = Rf_translateCharUTF8(strSEXP);
    return PyUnicode_FromString(value);
  } else {
    const char * value = Rf_translateChar(strSEXP);
    return PyString_FromString(value);
  }
}

PyObject* as_python_str(const std::string& str) {
  if (is_python3()) {
    return PyUnicode_FromString(str.c_str());
  } else {
    return PyString_FromString(str.c_str());
  }
}

bool has_null_bytes(PyObject* str) {
  char* buffer;
  int res = PyString_AsStringAndSize(str, &buffer, NULL);
  if (res == -1) {
    py_fetch_error();
    return true;
  } else {
    return false;
  }
}

// helpers to narrow python array type to something convertable from R,
// guaranteed to return NPY_BOOL, NPY_LONG, NPY_DOUBLE, or NPY_CDOUBLE
// (throws an exception if it's unable to return one of these types)
int narrow_array_typenum(int typenum) {

  switch(typenum) {
  // logical
  case NPY_BOOL:
    typenum = NPY_BOOL;
    break;
    // integer
  case NPY_BYTE:
  case NPY_UBYTE:
  case NPY_SHORT:
  case NPY_USHORT:
  case NPY_INT:
    typenum = NPY_LONG;
    break;
    // double
  case NPY_UINT:
  case NPY_ULONG:
  case NPY_ULONGLONG:
  case NPY_LONG:
  case NPY_LONGLONG:
  case NPY_HALF:
  case NPY_FLOAT:
  case NPY_DOUBLE:
    typenum = NPY_DOUBLE;
    break;

    // complex
  case NPY_CFLOAT:
  case NPY_CDOUBLE:
    typenum = NPY_CDOUBLE;
    break;


    // string/object (leave these alone)
  case NPY_STRING:
  case NPY_UNICODE:
  case NPY_OBJECT:
    break;

    // unsupported
  default:
    stop("Conversion from numpy array type %d is not supported", typenum);
    break;
  }

  return typenum;
}

int narrow_array_typenum(PyArrayObject* array) {
  return narrow_array_typenum(PyArray_TYPE(array));
}

int narrow_array_typenum(PyArray_Descr* descr) {
  return narrow_array_typenum(descr->type_num);
}

bool is_numpy_str(PyObject* x) {
  if (!isPyArrayScalar(x))
    return false; // ndarray or other, not string

  PyArray_DescrPtr descrPtr(PyArray_DescrFromScalar(x));
  int typenum = narrow_array_typenum(descrPtr);
  return (typenum == NPY_STRING || typenum == NPY_UNICODE);
}

bool is_python_str(PyObject* x) {

  if (PyUnicode_Check(x))
    return true;

  // python3 doesn't have PyString_* so mask it out (all strings in
  // python3 will get caught by PyUnicode_Check, we'll ignore
  // PyBytes entirely and let it remain a python object)
  else if (!is_python3() && PyString_Check(x) && !has_null_bytes(x))
    return true;

  else if (is_numpy_str(x))
    return true;

  else
    return false;
}

// check whether a PyObject is None
bool py_is_none(PyObject* object) {
  return object == Py_None;
}

// convenience wrapper for PyImport_Import
PyObject* py_import(const std::string& module) {
  PyObjectPtr module_str(as_python_str(module));
  return PyImport_Import(module_str);
}

std::string as_r_class(PyObject* classPtr) {

  PyObjectPtr namePtr(PyObject_GetAttrString(classPtr, "__name__"));
  std::ostringstream ostr;
  std::string module;

  if (PyObject_HasAttrString(classPtr, "__module__")) {
    PyObjectPtr modulePtr(PyObject_GetAttrString(classPtr, "__module__"));
    module = as_std_string(modulePtr) + ".";
    std::string builtin("__builtin__");
    if (module.find(builtin) == 0)
      module.replace(0, builtin.length(), "python.builtin");
    std::string builtins("builtins");
    if (module.find(builtins) == 0)
      module.replace(0, builtins.length(), "python.builtin");
  } else {
    module = "python.builtin.";
  }

  ostr << module << as_std_string(namePtr);
  return ostr.str();

}

std::vector<std::string> py_class_names(PyObject* object) {

  // class
  PyObjectPtr classPtr(PyObject_GetAttrString(object, "__class__"));
  if (classPtr.is_null())
    throw PythonException(py_fetch_error());

  // call inspect.getmro to get the class and it's bases in
  // method resolution order
  static PyObject* getmro = NULL;
  if (getmro == NULL) {
    PyObjectPtr inspect(py_import("inspect"));
    if (inspect.is_null())
      throw PythonException(py_fetch_error());

    getmro = PyObject_GetAttrString(inspect, "getmro");
    if (getmro == NULL)
      throw PythonException(py_fetch_error());
  }

  PyObjectPtr classes(PyObject_CallFunctionObjArgs(getmro, classPtr.get(), NULL));
  if (classes.is_null())
    throw PythonException(py_fetch_error());

  // start adding class names
  std::vector<std::string> classNames;

  // add the bases to the R class attribute
  Py_ssize_t len = PyTuple_Size(classes);
  for (Py_ssize_t i = 0; i < len; i++) {
    PyObject* base = PyTuple_GetItem(classes, i); // borrowed
    classNames.push_back(as_r_class(base));
  }

  // return constructed class names
  return classNames;

}

// wrap a PyObject
PyObjectRef py_ref(PyObject* object,
                   bool convert,
                   const std::string& extraClass = "")
{

  // wrap
  PyObjectRef ref(object, convert);

  // class attribute
  std::vector<std::string> attrClass;

  // add extra class if requested
  if (!extraClass.empty() &&
      std::find(attrClass.begin(),
                attrClass.end(),
                extraClass) == attrClass.end()) {
    attrClass.push_back(extraClass);
  }

  // register R classes
  if (PyObject_HasAttrString(object, "__class__")) {
    std::vector<std::string> classNames = py_class_names(object);
    attrClass.insert(attrClass.end(), classNames.begin(), classNames.end());
  }

  // add python.builtin.object if we don't already have it
  if (std::find(attrClass.begin(), attrClass.end(), "python.builtin.object") == attrClass.end()) {
    attrClass.push_back("python.builtin.object");
  }

  // apply class filter
  Rcpp::Environment pkgEnv = Rcpp::Environment::namespace_env("reticulate");
  Rcpp::Function py_filter_classes = pkgEnv["py_filter_classes"];
  attrClass = as< std::vector<std::string> >(py_filter_classes(attrClass));

  // set classes
  ref.attr("class") = attrClass;

  // return ref
  return ref;

}

//' Check if a Python object is a null externalptr
//'
//' @param x Python object
//'
//' @return Logical indicating whether the object is a null externalptr
//'
//' @details When Python objects are serialized within a persisted R
//'  environment (e.g. .RData file) they are deserialized into null
//'  externalptr objects (since the Python session they were originally
//'  connected to no longer exists). This function allows you to safely
//'  check whether whether a Python object is a null externalptr.
//'
//'  The `py_validate` function is a convenience function which calls
//'  `py_is_null_xptr` and throws an error in the case that the xptr
//'  is `NULL`.
//'
//' @export
// [[Rcpp::export]]
bool py_is_null_xptr(PyObjectRef x) {
  return x.is_null_xptr();
}

//' @rdname py_is_null_xptr
//' @export
// [[Rcpp::export]]
void py_validate_xptr(PyObjectRef x) {
  if (py_is_null_xptr(x)) {
    stop("Object is a null externalptr (it may have been disconnected from "
           " the session where it was created)");
  }
}


bool option_is_true(const std::string& name) {
  SEXP valueSEXP = Rf_GetOption(Rf_install(name.c_str()), R_BaseEnv);
  return Rf_isLogical(valueSEXP) && (as<bool>(valueSEXP) == true);
}

bool traceback_enabled() {
  Environment pkgEnv = Environment::namespace_env("reticulate");
  Function func = pkgEnv["traceback_enabled"];
  return as<bool>(func());
}





// copied directly from purrr; used to call rlang::trace_back() in
// py_fetch_error() in such a way that it doesn't introduce a new
// frame in returned traceback
SEXP current_env(void) {
  static SEXP call = NULL;

  if (!call) {
    // `sys.frame(sys.nframe())` doesn't work because `sys.nframe()`
    // returns the number of the frame in which evaluation occurs. It
    // doesn't return the number of frames on the stack. So we'd need
    // to evaluate it in the last frame on the stack which is what we
    // are looking for to begin with. We use instead this workaround:
    // Call `sys.frame()` from a closure to push a new frame on the
    // stack, and use negative indexing to get the previous frame.
    ParseStatus status;
    SEXP code = PROTECT(Rf_mkString("sys.frame(-1)"));
    SEXP parsed = PROTECT(R_ParseVector(code, -1, &status, R_NilValue));
    SEXP body = VECTOR_ELT(parsed, 0);

    SEXP fn = PROTECT(Rf_allocSExp(CLOSXP));
    SET_FORMALS(fn, R_NilValue);
    SET_BODY(fn, body);
    SET_CLOENV(fn, R_BaseEnv);

    call = Rf_lang1(fn);
    R_PreserveObject(call);

    UNPROTECT(3);
  }

  return Rf_eval(call, R_BaseEnv);
}

SEXP get_current_call(void) {
  static SEXP call = NULL;

  if (!call) {
    ParseStatus status;
    SEXP code = PROTECT(Rf_mkString("sys.call(-1)"));
    SEXP parsed = PROTECT(R_ParseVector(code, -1, &status, R_NilValue));
    SEXP body = VECTOR_ELT(parsed, 0);

    SEXP fn = PROTECT(Rf_allocSExp(CLOSXP));
    SET_FORMALS(fn, R_NilValue);
    SET_BODY(fn, body);
    SET_CLOENV(fn, R_BaseEnv);

    call = Rf_lang1(fn);
    R_PreserveObject(call);

    UNPROTECT(3);
  }

  return Rf_eval(call, R_BaseEnv);
}

SEXP get_r_trace(bool maybe_use_cached = false) {
  static SEXP get_r_trace_s = NULL;
  static SEXP reticulate_ns = NULL;

  if (!get_r_trace_s) {
    reticulate_ns = R_FindNamespace(Rf_mkString("reticulate"));
    get_r_trace_s =  Rf_install("get_r_trace");
  }

  SEXP maybe_use_cached_ = PROTECT(Rf_ScalarLogical(maybe_use_cached));
  SEXP trim_tail_ = PROTECT(Rf_ScalarInteger(1));
  SEXP call = PROTECT(Rf_lang3(get_r_trace_s, maybe_use_cached_, trim_tail_));
  SEXP result = PROTECT(Rf_eval(call, reticulate_ns));
  UNPROTECT(4);
  return result;
}

SEXP py_fetch_error(bool maybe_reuse_cached_r_trace) {

  // TODO: we need to add a guardrail to catch cases when
  // this is being invoked from not the main thread

  // check whether this error was signaled via an interrupt.
  // the intention here is to catch cases where reticulate is running
  // Python code, an interrupt is signaled and caught by that code,
  // and then the associated error is returned. in such a case, we
  // want to forward that interrupt back to R so that the user is then
  // returned back to the top level.
  if (reticulate::signals::getPythonInterruptsPending()) {
    PyErr_Clear();
    reticulate::signals::setInterruptsPending(false);
    reticulate::signals::setPythonInterruptsPending(false);
    throw Rcpp::internal::InterruptedException();
  }

  PyObject *excType, *excValue, *excTraceback;
  PyErr_Fetch(&excType, &excValue, &excTraceback);  // we now own the PyObjects

  if (!excType) {
    Rcpp::stop("Unknown Python error.");
  }

  PyErr_NormalizeException(&excType, &excValue, &excTraceback);

  if (excTraceback != NULL && excValue != NULL && s_isPython3) {
    PyException_SetTraceback(excValue, excTraceback);
    Py_DecRef(excTraceback);
  }

  PyObjectPtr pExcType(excType);  // decref on exit

  if (!PyObject_HasAttrString(excValue, "call")) {
    // check if this exception originated in python using the `raise from`
    // statement with an exception that we've already augmented with the full
    // r_trace. (or similarly, raised a new exception inside an `except:` block
    // while it is catching an Exception that contains an r_trace). If we find
    // r_trace/r_call in a __context__ Exception, pull them forward to this
    // topmost exception.
    PyObject *context = NULL, *r_call = NULL, *r_trace = NULL;
    PyObject *excValue_tmp = excValue;

    while ((context = PyObject_GetAttrString(excValue_tmp, "__context__"))) {
      if ((r_call = PyObject_GetAttrString(context, "call"))) {
          PyObject_SetAttrString(excValue, "call", r_call);
          Py_DecRef(r_call);
      }
      if ((r_trace = PyObject_GetAttrString(context, "trace"))) {
          PyObject_SetAttrString(excValue, "trace", r_trace);
          Py_DecRef(r_trace);
      }
      excValue_tmp = context;
      Py_DecRef(context);
      if(r_call || r_trace) {
        break;
      }
    }
  }



  // make sure the exception object has some some attrs: call, trace
  if (!PyObject_HasAttrString(excValue, "trace")) {
    SEXP r_trace = PROTECT(get_r_trace(maybe_reuse_cached_r_trace));
    PyObject* r_trace_capsule(py_capsule_new(r_trace));
    PyObject_SetAttrString(excValue, "trace", r_trace_capsule);
    Py_DecRef(r_trace_capsule);
    UNPROTECT(1);
  }

  // Otherwise, try to capture the current call.

  // A first draft of this tried using: SEXP r_call = get_last_call();
  // with get_last_call() defined in Rcpp headers. Unfortunately, that would
  // skip over the actual call of interest, and frequently return NULL
  // for shallow call stacks. So we fetch the call directly
  // using the R API.
  if (!PyObject_HasAttrString(excValue, "call")) {
    SEXP r_call = get_current_call();
    PyObject *r_call_capsule(py_capsule_new(r_call));
    PyObject_SetAttrString(excValue, "call", r_call_capsule);
    Py_DecRef(r_call_capsule);
    UNPROTECT(1);
  }


  // get the cppstack, r_cppstack
  // FIXME: this doesn't seem to work, always returns NULL
  // SEXP r_cppstack = PROTECT(rcpp_get_stack_trace());
  // PyObject* r_cppstack_capsule(py_capsule_new(r_cppstack));
  // UNPROTECT(1);
  // PyObject_SetAttrString(excValue, "r_cppstack", r_cppstack_capsule);
  // Py_DecRef(r_cppstack_capsule);

  PyObjectRef cond(py_ref(excValue, true));

  Environment pkg_globals(
      Environment::namespace_env("reticulate").get(".globals"));
  pkg_globals.assign("py_last_exception", cond);

  if (flush_std_buffers() == -1)
    warning(
        "Error encountered when flushing python buffers sys.stderr and "
        "sys.stdout");

  return cond;
}

// [[Rcpp::export]]
SEXP py_flush_output() {
  if(s_is_python_initialized)
    flush_std_buffers();
  return R_NilValue;
}

// [[Rcpp::export]]
std::string conditionMessage_from_py_exception(PyObjectRef exc) {
  // invoke 'traceback.format_exception_only(<traceback>)'
  PyObjectPtr tb_module(py_import("traceback"));
  if (tb_module.is_null())
    return "<unknown python exception, traceback module not found>";

  PyObjectPtr format_exception_only(
      PyObject_GetAttrString(tb_module, "format_exception_only"));
  if (format_exception_only.is_null())
    return "<unknown python exception, traceback format fn not found>";

  PyObjectPtr formatted(PyObject_CallFunctionObjArgs(
      format_exception_only, Py_TYPE(exc.get()), exc.get(), NULL));
  if (formatted.is_null())
    return "<unknown python exception, traceback format fn returned NULL>";

  // build error text
  std::ostringstream oss;

  // PyList_GetItem() returns a borrowed reference, no need to decref.
  for (Py_ssize_t i = 0, n = PyList_Size(formatted); i < n; i++)
    oss << as_std_string(PyList_GetItem(formatted, i));

  static std::string hint;

  if (hint.empty()) {
    Environment pkg_env(Environment::namespace_env("reticulate"));
    Function hint_fn = pkg_env[".py_last_error_hint"];
    CharacterVector r_result = hint_fn();
    hint = Rcpp::as<std::string>(r_result[0]);
  }

  oss << hint;
  std::string error = oss.str();

  SEXP max_msg_len_s = PROTECT(Rf_GetOption1(Rf_install("warning.length")));
  std::size_t max_msg_len(Rf_asInteger(max_msg_len_s));
  UNPROTECT(1);

  if (error.size() > max_msg_len) {
    // R has a modest byte size limit for error messages, default 1000, user
    // adjustable up to 8170. Error messages beyond the limit are silently
    // truncated. If the message will be truncated, we truncate it a little
    // better here and include a useful hint in the error message.

    std::string trunc("<...truncated...>");

    // Tensorflow since ~2.6 has been including a currated traceback as part of
    // the formatted exception message, with the most user-actionable content
    // towards the tail. Since the tail is the most useful part of the message,
    // truncate from the middle of the exception by default, after including the
    // first two lines.
    int over(error.size() - max_msg_len);
    int first_line_end_pos(error.find("\n"));
    int second_line_start_pos(error.find("\n", first_line_end_pos + 1));
    std::string head(error.substr(0, second_line_start_pos + 1));
    std::string tail(
        error.substr(over + head.size() + trunc.size() + 20,
                     std::string::npos));
    // +20 to accommodate "Error: " and similar accruals from R signal handlers.
    error = head + trunc + tail;
  }

  return error;
}

// check whether the PyObject can be mapped to an R scalar type
int r_scalar_type(PyObject* x) {

  if (PyBool_Check(x))
    return LGLSXP;

  // integer
  else if (PyInt_Check(x) || PyLong_Check(x))
    return INTSXP;

  // double
  else if (PyFloat_Check(x))
    return REALSXP;

  // complex
  else if (PyComplex_Check(x))
    return CPLXSXP;

  else if (is_python_str(x))
    return STRSXP;

  // not a scalar
  else
    return NILSXP;
}

// check whether the PyObject is a list of a single R scalar type
int scalar_list_type(PyObject* x) {

  Py_ssize_t len = PyList_Size(x);
  if (len == 0)
    return NILSXP;

  PyObject* first = PyList_GetItem(x, 0);
  int scalarType = r_scalar_type(first);
  if (scalarType == NILSXP)
    return NILSXP;

  for (Py_ssize_t i = 1; i<len; i++) {
    PyObject* next = PyList_GetItem(x, i);
    if (r_scalar_type(next) != scalarType)
      return NILSXP;
  }

  return scalarType;
}

bool py_equal(PyObject* x, const std::string& str) {

  PyObjectPtr pyStr(as_python_str(str));
  if (pyStr.is_null())
    throw PythonException(py_fetch_error());

  return PyObject_RichCompareBool(x, pyStr, Py_EQ) == 1;

}

bool is_pandas_na(PyObject* x) {

  // retrieve class object
  PyObjectPtr pyClass(py_get_attr(x, "__class__"));
  if (pyClass.is_null())
    return false;

  PyObjectPtr pyModule(py_get_attr(pyClass, "__module__"));
  if (pyModule.is_null())
    return false;

  // check for expected module name
  if (!py_equal(pyModule, "pandas._libs.missing"))
    return false;

  // retrieve class name
  PyObjectPtr pyName(py_get_attr(pyClass, "__name__"));
  if (pyName.is_null())
    return false;

  // check for expected names
  return py_equal(pyName, "NAType") ||
    py_equal(pyName, "C_NAType");

}

#define STATIC_MODULE(module)                                      \
  const static PyObjectPtr mod(PyImport_ImportModule(module));     \
  if (mod.is_null()) {                                             \
    throw PythonException(py_fetch_error());                       \
  }                                                                \
  return mod;

PyObject* numpy () {
  STATIC_MODULE("numpy")
}

PyObject* pandas_arrays () {
  STATIC_MODULE("pandas.arrays")
}

bool is_pandas_na_like(PyObject* x) {
  const static PyObjectPtr np_nan(PyObject_GetAttrString(numpy(), "nan"));
  return is_pandas_na(x) || (x == Py_None) || (x == (PyObject*)np_nan);
}

void set_string_element(SEXP rArray, int i, PyObject* pyStr) {
  if (is_pandas_na_like(pyStr)) {
    SET_STRING_ELT(rArray, i, NA_STRING);
    return;
  }
  std::string str = as_std_string(pyStr);
  cetype_t ce = PyUnicode_Check(pyStr) ? CE_UTF8 : CE_NATIVE;
  SEXP strSEXP = Rf_mkCharCE(str.c_str(), ce);
  SET_STRING_ELT(rArray, i, strSEXP);
}

bool py_is_callable(PyObject* x) {
  return PyCallable_Check(x) == 1 || PyObject_HasAttrString(x, "__call__");
}

// [[Rcpp::export]]
PyObjectRef py_none_impl() {
  Py_IncRef(Py_None);
  return py_ref(Py_None, false);
}

// [[Rcpp::export]]
bool py_is_callable(PyObjectRef x) {
  if (x.is_null_xptr())
    return false;
  else
    return py_is_callable(x.get());
}

// caches np.nditer function so we don't need to obtain it everytime we want to
// cast numpy string arrays into R objects.
PyObject* get_np_nditer () {
  const static PyObjectPtr np_nditer(PyObject_GetAttrString(numpy(), "nditer"));
  if (np_nditer.is_null()) {
    throw PythonException(py_fetch_error());
  }
  return np_nditer;
}

// convert a python object to an R object
SEXP py_to_r(PyObject* x, bool convert) {

  // NULL for Python None
  if (py_is_none(x))
    return R_NilValue;

  // check for scalars
  int scalarType = r_scalar_type(x);
  if (scalarType != NILSXP) {

    // logical
    if (scalarType == LGLSXP)
      return LogicalVector::create(x == Py_True);

    // integer
    else if (scalarType == INTSXP)
      return IntegerVector::create(PyInt_AsLong(x));

    // double
    else if (scalarType == REALSXP)
      return NumericVector::create(PyFloat_AsDouble(x));

    // complex
    else if (scalarType == CPLXSXP) {
      Rcomplex cplx;
      cplx.r = PyComplex_RealAsDouble(x);
      cplx.i = PyComplex_ImagAsDouble(x);
      return ComplexVector::create(cplx);
    }

    // string
    else if (scalarType == STRSXP)
      return CharacterVector::create(as_utf8_r_string(x));

    else
      return R_NilValue; // keep compiler happy
  }

  // list
  else if (PyList_CheckExact(x)) {

    Py_ssize_t len = PyList_Size(x);
    int scalarType = scalar_list_type(x);
    if (scalarType == REALSXP) {
      Rcpp::NumericVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyFloat_AsDouble(PyList_GetItem(x, i));
      return vec;
    } else if (scalarType == INTSXP) {
      Rcpp::IntegerVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyInt_AsLong(PyList_GetItem(x, i));
      return vec;
    } else if (scalarType == CPLXSXP) {
      Rcpp::ComplexVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++) {
        PyObject* item = PyList_GetItem(x, i);
        Rcomplex cplx;
        cplx.r = PyComplex_RealAsDouble(item);
        cplx.i = PyComplex_ImagAsDouble(item);
        vec[i] = cplx;
      }
      return vec;
    } else if (scalarType == LGLSXP) {
      Rcpp::LogicalVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyList_GetItem(x, i) == Py_True;
      return vec;
    } else if (scalarType == STRSXP) {
      Rcpp::CharacterVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = as_utf8_r_string(PyList_GetItem(x, i));
      return vec;
    } else { // not a homegenous list of scalars, return a list
      Rcpp::List list(len);
      for (Py_ssize_t i = 0; i<len; i++)
        list[i] = py_to_r(PyList_GetItem(x, i), convert);
      return list;
    }
  }

  // tuple (but don't convert namedtuple as it's often a custom class)
  else if (PyTuple_CheckExact(x) && !PyObject_HasAttrString(x, "_fields")) {
    Py_ssize_t len = PyTuple_Size(x);
    Rcpp::List list(len);
    for (Py_ssize_t i = 0; i<len; i++)
      list[i] = py_to_r(PyTuple_GetItem(x, i), convert);
    return list;
  }

  // dict
  else if (PyDict_CheckExact(x)) {

    // copy the dict and allocate
    PyObjectPtr dict(PyDict_Copy(x));
    Py_ssize_t size = PyDict_Size(dict);
    std::vector<std::string> names(size);
    Rcpp::List list(size);

    // iterate over dict
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    Py_ssize_t idx = 0;
    while (PyDict_Next(dict, &pos, &key, &value)) {
      if (is_python_str(key)) {
        names[idx] = as_utf8_r_string(key);
      } else {
        PyObjectPtr str(PyObject_Str(key));
        names[idx] = as_utf8_r_string(str);
      }
      list[idx] = py_to_r(value, convert);
      idx++;
    }
    list.names() = names;
    return list;

  }

  // numpy array
  else if (isPyArray(x)) {

    // R array to return
    RObject rArray = R_NilValue;

    // get the array
    PyArrayObject* array = (PyArrayObject*) x;

    // get the dimensions -- treat 0-dim array (numpy scalar) as
    // a 1-dim for conversion to R (will end up with a single
    // element R vector)
    npy_intp len = PyArray_SIZE(array);
    int nd = PyArray_NDIM(array);
    IntegerVector dimsVector(nd);
    if (nd > 0) {
      npy_intp *dims = PyArray_DIMS(array);
      for (int i = 0; i<nd; i++)
        dimsVector[i] = dims[i];
    } else {
      dimsVector.push_back(1);
    }

    // determine the target type of the array
    int typenum = narrow_array_typenum(array);

    // cast it to a fortran array (PyArray_CastToType steals the descr)
    // (note that we will decref the copied array below)
    PyArray_Descr* descr = PyArray_DescrFromType(typenum);
    array = (PyArrayObject*) PyArray_CastToType(array, descr, NPY_ARRAY_FARRAY);
    if (array == NULL)
      throw PythonException(py_fetch_error());

    // ensure we release it within this scope
    PyObjectPtr ptrArray((PyObject*)array);

    // copy the data as required per-type
    switch(typenum) {

      case NPY_BOOL: {
        npy_bool* pData = (npy_bool*)PyArray_DATA(array);
        rArray = Rf_allocArray(LGLSXP, dimsVector);
        for (int i=0; i<len; i++)
          LOGICAL(rArray)[i] = pData[i];
        break;
      }

      case NPY_LONG: {
        npy_long* pData = (npy_long*)PyArray_DATA(array);
        rArray = Rf_allocArray(INTSXP, dimsVector);
        for (int i=0; i<len; i++)
          INTEGER(rArray)[i] = pData[i];
        break;
      }

      case NPY_DOUBLE: {
        npy_double* pData = (npy_double*)PyArray_DATA(array);
        rArray = Rf_allocArray(REALSXP, dimsVector);
        for (int i=0; i<len; i++)
          REAL(rArray)[i] = pData[i];
        break;
      }

      case NPY_CDOUBLE: {
        npy_complex128* pData = (npy_complex128*)PyArray_DATA(array);
        rArray = Rf_allocArray(CPLXSXP, dimsVector);
        for (int i=0; i<len; i++) {
          npy_complex128 data = pData[i];
          Rcomplex cpx;
          cpx.r = data.real;
          cpx.i = data.imag;
          COMPLEX(rArray)[i] = cpx;
        }
        break;
      }

      case NPY_STRING:
      case NPY_UNICODE: {

        PyObjectPtr nditerArgs(PyTuple_New(1));
        // PyTuple_SetItem steals reference the array, but it's already wraped
        // into PyObjectPtr earlier (so it gets deleted after the scope of this function)
        // To avoid trying to delete it twice, we need to increase its ref count here.
        PyTuple_SetItem(nditerArgs, 0, (PyObject*)array);
        Py_IncRef((PyObject*)array);

        PyObjectPtr iter(PyObject_Call(get_np_nditer(), nditerArgs, NULL));

        if (iter.is_null()) {
          throw PythonException(py_fetch_error());
        }

        rArray = Rf_allocArray(STRSXP, dimsVector);
        RObject protectArray(rArray);


        for (int i=0; i<len; i++) {
          PyObjectPtr el(PyIter_Next(iter)); // returns an scalar array.
          PyObjectPtr pyStr(PyObject_CallMethod(el, "item", NULL));
          if (pyStr.is_null()) {
            throw PythonException(py_fetch_error());
          }
          set_string_element(rArray, i, pyStr);
        }
        break;
      }

      case NPY_OBJECT: {

        // get python objects
        PyObject** pData = (PyObject**)PyArray_DATA(array);

        // check for all strings
        bool allStrings = true;
        for (npy_intp i=0; i<len; i++) {
          auto el = pData[i];
          if (!is_python_str(el) && !is_pandas_na_like(el)) {
            allStrings = false;
            break;
          }
        }

        // return a character vector if it's all strings
        if (allStrings) {
          rArray = Rf_allocArray(STRSXP, dimsVector);
          RObject protectArray(rArray);
          for (npy_intp i = 0; i < len; i++)
            set_string_element(rArray, i, pData[i]);
          break;
        }

        // otherwise return a list of objects
        rArray = Rf_allocArray(VECSXP, dimsVector);
        RObject protectArray(rArray);
        for (npy_intp i = 0; i < len; i++) {
          SEXP data = py_to_r(pData[i], convert);
          SET_VECTOR_ELT(rArray, i, data);
        }

        break;
      }
    }

    // return the R Array
    return rArray;

  }

  // check for numpy scalar
  else if (isPyArrayScalar(x)) {

    // determine the type to convert to
    PyArray_DescrPtr descrPtr(PyArray_DescrFromScalar(x));
    int typenum = narrow_array_typenum(descrPtr);
    PyArray_DescrPtr toDescr(PyArray_DescrFromType(typenum));

    // convert to R type (guaranteed to by NPY_BOOL, NPY_LONG, or NPY_DOUBLE
    // as per the contract of narrow_arrow_typenum)
    switch(typenum) {

    case NPY_BOOL:
    {
      npy_bool value;
      PyArray_CastScalarToCtype(x, (void*)&value, toDescr);
      return LogicalVector::create(value);
    }

    case NPY_LONG:
    {
      npy_long value;
      PyArray_CastScalarToCtype(x, (void*)&value, toDescr);
      return IntegerVector::create(value);
    }

    case NPY_DOUBLE:
    {
      npy_double value;
      PyArray_CastScalarToCtype(x, (void*)&value, toDescr);
      return NumericVector::create(value);
    }

    case NPY_CDOUBLE:
    {
      npy_complex128 value;
      PyArray_CastScalarToCtype(x, (void*)&value, toDescr);
      Rcomplex cpx;
      cpx.r = value.real;
      cpx.i = value.imag;
      return ComplexVector::create(cpx);
    }

    default:
    {
      stop("Unsupported array conversion from %d", typenum);
    }

    }

  }

  else if (PyList_Check(x)) {
    // didn't pass PyList_CheckExact(), but does pass PyList_Check()
    // so it's an object that subclasses list.
    // (This type of subclassed list is used by tensorflow for lists of layers
    // attached to a keras model, tensorflow.python.training.tracking.data_structures.List,
    // https://github.com/rstudio/reticulate/issues/1226 )
    // if needed, consider changing this check from PyList_Check(x) to either:
    //  - PySequence_Check(x), which just checks for existence of __getitem__ and __len__ methods,
    //  - PyObject_IsInstance(x, Py_ListClass) for wrapt.ProxyObject wrapping a list.

    // Since it's a subclassed list.
    // We can't depend on the the PyList_* API working,
    // and must instead fallback to the generic PyObject_* API or PySequence_API.
    // (PyList_*() function do not work for tensorflow.python.training.tracking.data_structures.List)
    long len = PyObject_Size(x);
    Rcpp::List list(len);
    for (long i = 0; i < len; i++) {
      PyObject *pi = PyLong_FromLong(i);
      list[i] = py_to_r(PyObject_GetItem(x, pi), convert);
      Py_DecRef(pi);
    }
    return list;
  }

  else if (PyObject_IsInstance(x, Py_DictClass)) {
    // This check is kind of slow since it calls back into evaluating Python code instead of
    // merely consulting the object header, but it is the only reliable way that works
    // for tensorflow._DictWrapper,
    // which in actually is a wrapt.ProxyObject pretending to be a dict.
    // ProxyObject goes to great lenghts to pretend to be the underlying object,
    // to the point that x.__class__ is __builtins__.dict,
    // but it fails PyDict_CheckExact(x) and PyDict_Check(x).
    // Registering a custom S3 r_to_py() method here isn't straighforward either,
    // since the object presents as a plain dict when inspecting __class__,
    // despite the fact that none of the PyDict_* C API functions work with it.

    // PyMapping_Items returns a list of (key, value) tuples.
    PyObjectPtr items(PyMapping_Items(x));

    Py_ssize_t size = PyObject_Size(items);
    std::vector<std::string> names(size);
    Rcpp::List list(size);

    for (Py_ssize_t idx = 0; idx < size; idx++) {
      PyObjectPtr item(PySequence_GetItem(items, idx));
      PyObject *key = PyTuple_GetItem(item, 0); // borrowed ref
      PyObject *value = PyTuple_GetItem(item, 1); // borrowed ref

      if (is_python_str(key)) {
        names[idx] = as_utf8_r_string(key);
      } else {
        PyObjectPtr str(PyObject_Str(key));
        names[idx] = as_utf8_r_string(str);
      }
      list[idx] = py_to_r(value, convert);
    }
    list.names() = names;
    return list;
  }

  // callable
  else if (py_is_callable(x)) {

    // reference to underlying python object
    Py_IncRef(x);
    PyObjectRef pyFunc = py_ref(x, convert);

    // create an R function wrapper
    Rcpp::Environment pkgEnv = Rcpp::Environment::namespace_env("reticulate");
    Rcpp::Function py_callable_as_function = pkgEnv["py_callable_as_function"];
    Rcpp::Function f = py_callable_as_function(pyFunc, convert);

    // forward classes
    f.attr("class") = pyFunc.attr("class");

    // save reference to underlying py_object
    f.attr("py_object") = pyFunc;

    // return the R function
    return f;
  }

  // iterator/generator
  else if (PyObject_HasAttrString(x, "__iter__") &&
           (PyObject_HasAttrString(x, "next") ||
            PyObject_HasAttrString(x, "__next__"))) {

    // return it raw but add a class so we can create S3 methods for it
    Py_IncRef(x);
    return py_ref(x, true, std::string("python.builtin.iterator"));
  }

  // bytearray
  else if (PyByteArray_Check(x)) {

    if (PyByteArray_Size(x) == 0)
      return RawVector();

    return RawVector(
      PyByteArray_AsString(x),
      PyByteArray_AsString(x) + PyByteArray_Size(x));

  }

  // pandas array
  else if (is_pandas_na(x)) {
    return NumericVector::create(R_NaReal);
  }

  else if (is_r_object_capsule(x)) {
    return py_capsule_read(x);
  }

  // default is to return opaque wrapper to python object. we pass convert = true
  // because if we hit this code then conversion has been either implicitly
  // or explicitly requested.
  else {
    Py_IncRef(x);
    return py_ref(x, true);
  }

}

/* stretchy list, modified from R sources
   CAR of the list points to the last cons-cell
   CDR points to the first.
*/

SEXP NewList(void) {
  SEXP s = Rf_cons(R_NilValue, R_NilValue);
  SETCAR(s, s);
  return s;
}

/* Add named element to the end of a stretchy list */
void GrowList(SEXP args_list, SEXP tag, SEXP dflt) {
  PROTECT(dflt);
  SEXP tmp = PROTECT(Rf_cons(dflt, R_NilValue));
  SET_TAG(tmp, tag);

  SETCDR(CAR(args_list), tmp); // set cdr on the last cons-cell
  SETCAR(args_list, tmp);      // update pointer to last cons cell
  UNPROTECT(2);
}

// [[Rcpp::export]]
SEXP py_get_formals(PyObjectRef callable)
{

  static PyObject *inspect_module = NULL;
  static PyObject *inspect_signature = NULL;
  static PyObject *inspect_Parameter = NULL;
  static PyObject *inspect_Parameter_VAR_KEYWORD = NULL;
  static PyObject *inspect_Parameter_VAR_POSITIONAL = NULL;
  static PyObject *inspect_Parameter_KEYWORD_ONLY = NULL;
  static PyObject *inspect_Parameter_empty = NULL;

  if (!inspect_Parameter_empty)
  {
    // initialize static variables to avoid repeat lookups
    inspect_module = PyImport_ImportModule("inspect");
    if (!inspect_module) throw PythonException(py_fetch_error());

    inspect_signature = PyObject_GetAttrString(inspect_module, "signature");
    if (!inspect_signature) throw PythonException(py_fetch_error());

    inspect_Parameter = PyObject_GetAttrString(inspect_module, "Parameter");
    if (!inspect_Parameter) throw PythonException(py_fetch_error());

    inspect_Parameter_VAR_KEYWORD = PyObject_GetAttrString(inspect_Parameter, "VAR_KEYWORD");
    if (!inspect_Parameter_VAR_KEYWORD) throw PythonException(py_fetch_error());

    inspect_Parameter_VAR_POSITIONAL = PyObject_GetAttrString(inspect_Parameter, "VAR_POSITIONAL");
    if (!inspect_Parameter_VAR_POSITIONAL) throw PythonException(py_fetch_error());

    inspect_Parameter_KEYWORD_ONLY = PyObject_GetAttrString(inspect_Parameter, "KEYWORD_ONLY");
    if (!inspect_Parameter_KEYWORD_ONLY) throw PythonException(py_fetch_error());

    inspect_Parameter_empty = PyObject_GetAttrString(inspect_Parameter, "empty");
    if (!inspect_Parameter_empty) throw PythonException(py_fetch_error());
  }

  PyObjectPtr sig(PyObject_CallFunctionObjArgs(inspect_signature, callable.get(), NULL));
  if (sig.is_null())
  {
    // inspect.signature() can error on builtins in cpython,
    // or python functions built in C from modules
    // fallback to returning formals of `...`.
    PyErr_Clear();
    SEXP out = Rf_cons(R_MissingArg, R_NilValue);
    SET_TAG(out, Rf_install("..."));
    return out;
  }

  PyObjectPtr parameters(PyObject_GetAttrString(sig, "parameters"));
  if (parameters.is_null()) throw PythonException(py_fetch_error());

  PyObjectPtr items_method(PyObject_GetAttrString(parameters, "items"));
  if (items_method.is_null()) throw PythonException(py_fetch_error());

  PyObjectPtr parameters_items(PyObject_CallFunctionObjArgs(items_method, NULL));
  if (parameters_items.is_null()) throw PythonException(py_fetch_error());

  PyObjectPtr parameters_iterator(PyObject_GetIter(parameters_items));
  if (parameters_iterator.is_null()) throw PythonException(py_fetch_error());

  RObject r_args(NewList());
  PyObject *item;
  bool has_dots = false;

  while ((item = PyIter_Next(parameters_iterator))) // new ref
  {
    PyObjectPtr item_(item); // auto-decref
    PyObject *name = PyTuple_GetItem(item, 0);  // borrowed reference
    PyObject *param = PyTuple_GetItem(item, 1); // borrowed reference

    PyObjectPtr kind_(PyObject_GetAttrString(param, "kind")); // new ref
    if (kind_.is_null()) throw PythonException(py_fetch_error());
    PyObject *kind = kind_.get();

    if (kind == inspect_Parameter_VAR_KEYWORD ||
        kind == inspect_Parameter_VAR_POSITIONAL)
    {
      if (!has_dots)
      {
          GrowList(r_args, Rf_install("..."), R_MissingArg);
          has_dots = true;
      }
      continue;
    }

    if (!has_dots && kind == inspect_Parameter_KEYWORD_ONLY)
    {
      GrowList(r_args, Rf_install("..."), R_MissingArg);
      has_dots = true;
    }

    SEXP arg_default = R_MissingArg;
    PyObjectPtr param_default(PyObject_GetAttrString(param, "default")); // new ref
    if (param_default.is_null())
      throw PythonException(py_fetch_error());

    if (param_default.get() != inspect_Parameter_empty)
      arg_default = py_to_r(param_default, true);

    const char *name_char = PyUnicode_AsUTF8(name);
    if (name_char == NULL) throw PythonException(py_fetch_error());

    SEXP name_sym = Rf_installChar(Rf_mkCharCE(name_char, CE_UTF8));
    GrowList(r_args, name_sym, arg_default);
  }

  if (PyErr_Occurred())
    throw PythonException(py_fetch_error());

  return CDR(r_args);
}

bool is_convertible_to_numpy(RObject x) {

  if (!haveNumPy())
    return false;

  int type = TYPEOF(x);

  return
    type == INTSXP  ||
    type == REALSXP ||
    type == LGLSXP  ||
    type == CPLXSXP ||
    type == STRSXP;
}

PyObject* r_to_py_numpy(RObject x, bool convert) {

  int type = x.sexp_type();
  SEXP sexp = x.get__();

  // figure out dimensions for resulting array
  IntegerVector dimensions = x.hasAttribute("dim")
    ? x.attr("dim")
    : IntegerVector::create(Rf_xlength(x));

  int nd = dimensions.length();
  std::vector<npy_intp> dims(nd);
  for (int i = 0; i < nd; i++)
    dims[i] = dimensions[i];

  // get pointer + type for underlying data
  int typenum;
  void* data;
  if (type == INTSXP) {
    if (sizeof(long) == 4)
      typenum = NPY_LONG;
    else
      typenum = NPY_INT;
    data = &(INTEGER(sexp)[0]);
  } else if (type == REALSXP) {
    typenum = NPY_DOUBLE;
    data = &(REAL(sexp)[0]);
  } else if (type == LGLSXP) {
    typenum = NPY_BOOL;
    data = &(LOGICAL(sexp)[0]);
  } else if (type == CPLXSXP) {
    typenum = NPY_CDOUBLE;
    data = &(COMPLEX(sexp)[0]);
  } else if (type == STRSXP) {
    typenum = NPY_OBJECT;
    data = NULL;
  } else {
    stop("Matrix type cannot be converted to python (only integer, "
           "numeric, complex, logical, and character matrixes can be "
           "converted");
  }

  int flags = NPY_ARRAY_FARRAY_RO;

  // because R logical vectors are just ints under the
  // hood, we need to explicitly construct a boolean
  // vector for our Python array. note that the created
  // array will own the data so we do not free it after
  if (typenum == NPY_BOOL) {
    R_xlen_t n = XLENGTH(sexp);
    bool* converted = (bool*) PyArray_malloc(n * sizeof(bool));
    for (R_xlen_t i = 0; i < n; i++)
      converted[i] = LOGICAL(sexp)[i];
    data = converted;
    flags |= NPY_ARRAY_OWNDATA;
  }

  // create the matrix
  PyObject* array = PyArray_New(&PyArray_Type,
                                nd,
                                &(dims[0]),
                                typenum,
                                NULL,
                                data,
                                0,
                                flags,
                                NULL);

  // check for error
  if (array == NULL)
    throw PythonException(py_fetch_error());

  // if this is a character vector we need to convert and set the elements,
  // otherwise the memory is shared with the underlying R vector
  if (type == STRSXP) {
    void** pData = (void**)PyArray_DATA((PyArrayObject*)array);
    R_xlen_t len = Rf_xlength(x);
    for (R_xlen_t i = 0; i<len; i++) {
      PyObject* pyStr = as_python_str(STRING_ELT(x, i), /*handle_na=*/true);
      pData[i] = pyStr;
    }

  } else {
    // wrap the R object in a capsule that's tied to the lifetime of the matrix
    // (so the R doesn't deallocate the memory while python is still pointing to it)
    PyObjectPtr capsule(py_capsule_new(x));

    // set base object using correct version of the API (detach since this
    // effectively steals a reference to the provided base object)
    if (PyArray_GetNDArrayCFeatureVersion() >= NPY_1_7_API_VERSION) {
      int res = PyArray_SetBaseObject((PyArrayObject *)array, capsule.detach());
      if (res != 0)
        throw PythonException(py_fetch_error());
    } else {
      PyArray_BASE(array) = capsule.detach();
    }
  }

  // return it
  return array;

}

PyObject* r_to_py_cpp(RObject x, bool convert);

PyObject* r_to_py(RObject x, bool convert) {
  // if the object bit is not set, we can skip R dispatch
  if (OBJECT(x) == 0)
    return r_to_py_cpp(x, convert);

  // get a reference to the R version of r_to_py
  Rcpp::Environment pkgEnv = Rcpp::Environment::namespace_env("reticulate");
  Rcpp::Function r_to_py_fn = pkgEnv["r_to_py"];

  // call the R version and hold the return value in a PyObjectRef (SEXP wrapper)
  // this object will be released when the function returns
  PyObjectRef ref(r_to_py_fn(x, convert));

  // get the underlying Python object and call Py_IncRef before returning it
  // this allows this function to provide the same memory semantics as the
  // previous C++ version of r_to_py (which is now r_to_py_cpp), which always
  // calls Py_IncRef on Python objects before returning them
  PyObject* obj = ref.get();
  Py_IncRef(obj);

  // return the Python object
  return obj;
}

// Python capsule wrapping an R's external pointer object
static void free_r_extptr_capsule(PyObject* capsule) {
  SEXP sexp = (SEXP)PyCapsule_GetContext(capsule);
  Rcpp_precious_remove_main_thread(sexp);
}

static PyObject* r_extptr_capsule(SEXP sexp) {
  // underlying pointer
  void* ptr = R_ExternalPtrAddr(sexp);
  if (ptr == NULL)
    stop("Invalid pointer");

  sexp = Rcpp_precious_preserve(sexp);

  PyObject* capsule = PyCapsule_New(ptr, NULL, free_r_extptr_capsule);
  PyCapsule_SetContext(capsule, (void*)sexp);
  return capsule;

}

// convert an R object to a python object (the returned object
// will have an active reference count on it)
PyObject* r_to_py_cpp(RObject x, bool convert) {
  int type = x.sexp_type();
  SEXP sexp = x.get__();

  // NULL becomes python None
  // (Py_IncRef since PyTuple_SetItem will steal the passed reference)
  if (x.isNULL()) {
    Py_IncRef(Py_None);
    return Py_None;
  }

  // use py_object attribute if we have it
  if (x.hasAttribute("py_object")) {
    Rcpp::RObject py_object = x.attr("py_object");
    PyObjectRef obj = as<PyObjectRef>(py_object);
    Py_IncRef(obj.get());
    return obj.get();
  }

  // pass python objects straight through (Py_IncRef since returning this
  // creates a new reference from the caller)
  if (x.inherits("python.builtin.object")) {
    PyObjectRef obj = as<PyObjectRef>(sexp);
    Py_IncRef(obj.get());
    return obj.get();
  }

  // convert arrays and matrixes to numpy (throw error if numpy not available)
  if (x.hasAttribute("dim") && requireNumPy()) {
    return r_to_py_numpy(x, convert);
  }

  // integer (pass length 1 vectors as scalars, otherwise pass list)
  if (type == INTSXP) {

    // handle scalars
    if (LENGTH(sexp) == 1) {
      int value = INTEGER(sexp)[0];
      return PyInt_FromLong(value);
    }

    PyObjectPtr list(PyList_New(LENGTH(sexp)));
    for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
      int value = INTEGER(sexp)[i];
      // NOTE: reference to added value is "stolen" by the list
      int res = PyList_SetItem(list, i, PyInt_FromLong(value));
      if (res != 0)
        throw PythonException(py_fetch_error());
    }

    return list.detach();

  }

  // numeric (pass length 1 vectors as scalars, otherwise pass list)
  if (type == REALSXP) {

    // handle scalars
    if (LENGTH(sexp) == 1) {
      double value = REAL(sexp)[0];
      return PyFloat_FromDouble(value);
    }

    PyObjectPtr list(PyList_New(LENGTH(sexp)));
    for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
      double value = REAL(sexp)[i];
      // NOTE: reference to added value is "stolen" by the list
      int res = PyList_SetItem(list, i, PyFloat_FromDouble(value));
      if (res != 0)
        throw PythonException(py_fetch_error());
    }

    return list.detach();

  }

  // complex (pass length 1 vectors as scalars, otherwise pass list)
  if (type == CPLXSXP) {

    // handle scalars
    if (LENGTH(sexp) == 1) {
      Rcomplex cplx = COMPLEX(sexp)[0];
      return PyComplex_FromDoubles(cplx.r, cplx.i);
    }

    PyObjectPtr list(PyList_New(LENGTH(sexp)));
    for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
      Rcomplex cplx = COMPLEX(sexp)[i];
      // NOTE: reference to added value is "stolen" by the list
      int res = PyList_SetItem(list, i, PyComplex_FromDoubles(cplx.r, cplx.i));
      if (res != 0)
        throw PythonException(py_fetch_error());
    }

    return list.detach();

  }

  // logical (pass length 1 vectors as scalars, otherwise pass list)
  if (type == LGLSXP) {

    // handle scalars
    if (LENGTH(sexp) == 1) {
      int value = LOGICAL(sexp)[0];
      return PyBool_FromLong(value);
    }

    PyObjectPtr list(PyList_New(LENGTH(sexp)));
    for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
      int value = LOGICAL(sexp)[i];
      // NOTE: reference to added value is "stolen" by the list
      int res = PyList_SetItem(list, i, PyBool_FromLong(value));
      if (res != 0)
        throw PythonException(py_fetch_error());
    }

    return list.detach();

  }

  // character (pass length 1 vectors as scalars, otherwise pass list)
  if (type == STRSXP) {

    // handle scalars
    if (LENGTH(sexp) == 1) {
      return as_python_str(STRING_ELT(sexp, 0));
    }

    PyObjectPtr list(PyList_New(LENGTH(sexp)));
    for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
      // NOTE: reference to added value is "stolen" by the list
      int res = PyList_SetItem(list, i, as_python_str(STRING_ELT(sexp, i)));
      if (res != 0)
        throw PythonException(py_fetch_error());
    }

    return list.detach();
  }

  // bytes
  if (type == RAWSXP) {

    Rcpp::RawVector raw(sexp);
    if (raw.size() == 0)
      return PyByteArray_FromStringAndSize(NULL, 0);

    return PyByteArray_FromStringAndSize(
      (const char*) RAW(raw),
      raw.size());

  }

  // list
  if (type == VECSXP) {

    // create a dict for names
    if (x.hasAttribute("names")) {
      PyObjectPtr dict(PyDict_New());
      CharacterVector names = x.attr("names");
      SEXP namesSEXP = names;
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        const char* name = Rf_translateChar(STRING_ELT(namesSEXP, i));
        PyObjectPtr item(r_to_py(RObject(VECTOR_ELT(sexp, i)), convert));
        int res = PyDict_SetItemString(dict, name, item);
        if (res != 0)
          throw PythonException(py_fetch_error());
      }

      return dict.detach();

    }

    // create a list if there are no names
    PyObjectPtr list(PyList_New(LENGTH(sexp)));
    for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
      PyObject* item = r_to_py(RObject(VECTOR_ELT(sexp, i)), convert);
      // NOTE: reference to added value is "stolen" by the list
      int res = PyList_SetItem(list, i, item);
      if (res != 0)
        throw PythonException(py_fetch_error());
    }

    return list.detach();

  }

  if (type == CLOSXP) {

    // create an R object capsule for the R function
    PyObjectPtr capsule(py_capsule_new(x));
    PyCapsule_SetContext(capsule, (void*)convert);

    // check for a py_function_name attribute
    PyObjectPtr pyFunctionName(r_to_py(x.attr("py_function_name"), convert));

    // create the python wrapper function
    PyObjectPtr module(py_import("rpytools.call"));
    if (module.is_null())
      throw PythonException(py_fetch_error());

    PyObjectPtr func(PyObject_GetAttrString(module, "make_python_function"));
    if (func.is_null())
      throw PythonException(py_fetch_error());

    PyObjectPtr wrapper(
        PyObject_CallFunctionObjArgs(
          func,
          capsule.get(),
          pyFunctionName.get(),
          NULL));

    if (wrapper.is_null())
      throw PythonException(py_fetch_error());

    // return the wrapper
    return wrapper.detach();

  }

  // externalptr
  if (type == EXTPTRSXP) {
    return r_extptr_capsule(sexp);
  }

  // default fallback, wrap the r object in a py capsule
  return py_capsule_new(sexp);

}

// [[Rcpp::export]]
PyObjectRef r_to_py_impl(RObject object, bool convert) {
  return py_ref(r_to_py_cpp(object, convert), convert);
}

// custom module used for calling R functions from python wrappers



extern "C" PyObject* call_r_function(PyObject *self, PyObject* args, PyObject* keywords)
{
  // the first argument is always the capsule containing the R function to call
  PyObject* capsule = PyTuple_GetItem(args, 0);
  RObject rFunction = py_capsule_read(capsule);

  bool convert = (bool)PyCapsule_GetContext(capsule);

  // convert remainder of positional arguments to R list
  PyObjectPtr funcArgs(PyTuple_GetSlice(args, 1, PyTuple_Size(args)));
  List rArgs;
  if (convert) {
    rArgs = py_to_r(funcArgs, convert);
  } else {
    Py_ssize_t len = PyTuple_Size(funcArgs);
    for (Py_ssize_t index = 0; index<len; index++) {
      PyObject* item = PyTuple_GetItem(funcArgs, index); // borrowed
      Py_IncRef(item);
      rArgs.push_back(py_ref(item, convert));
    }
  }

  // get keyword arguments
  List rKeywords;

  if (keywords != NULL) {

    if (convert) {
      rKeywords = py_to_r(keywords, convert);
    } else {

      PyObject *key, *value;
      Py_ssize_t pos = 0;

      // NOTE: PyDict_Next uses borrowed references,
      // so anything we return should be Py_IncRef'd
      while (PyDict_Next(keywords, &pos, &key, &value)) {
        PyObjectPtr str(PyObject_Str(key));
        Py_IncRef(value);
        rKeywords[as_std_string(str)] = py_ref(value, convert);
      }
    }

  }

  static SEXP call_r_function_s = NULL;
  if(call_r_function_s == NULL) {
    // Use an expression that deparses nicely for traceback printing purposes
    call_r_function_s = Rf_lang3(Rf_install(":::"), Rf_install("reticulate"), Rf_install("call_r_function"));
    R_PreserveObject(call_r_function_s);
  }

  RObject call_r_func_call(Rf_lang4(call_r_function_s, rFunction, rArgs, rKeywords));

  PyObject *out = PyTuple_New(2);
  try {
    // use current_env() here so that in case of error, rlang::trace_back()
    // prints this frame as a node of the parent rather than a top-level call.
    Rcpp::List result(Rf_eval(call_r_func_call, current_env()));
    // result is either
    // (return_value, NULL) or
    // (NULL, Exception object converted from r_error_condition_object)
    PyTuple_SetItem(out, 0, r_to_py(result[0], convert)); // value (or NULL)
    PyTuple_SetItem(out, 1, r_to_py(result[1], true));   // Exception (or NULL)
  } catch(const Rcpp::internal::InterruptedException& e) {
    PyTuple_SetItem(out, 0, r_to_py(R_NilValue, true));
    PyTuple_SetItem(out, 1, as_python_str("KeyboardInterrupt"));
  } catch(const std::exception& e) {
    PyTuple_SetItem(out, 0, r_to_py(R_NilValue, true));
    PyTuple_SetItem(out, 1, as_python_str(e.what()));
  } catch(...) {
    PyTuple_SetItem(out, 0, r_to_py(R_NilValue, true));
    PyTuple_SetItem(out, 1, as_python_str("(Unknown exception occurred)"));
  }

  return out;
}

struct PythonCall {
  PythonCall(PyObject* func, PyObject* data) : func(func), data(data) {
    Py_IncRef(func);
    Py_IncRef(data);
  }
  ~PythonCall() {
    Py_DecRef(func);
    Py_DecRef(data);
  }
  PyObject* func;
  PyObject* data;
private:
  PythonCall(const PythonCall& other);
  PythonCall& operator=(const PythonCall&);
};

int call_python_function(void* data) {

  // cast to call
  PythonCall* call = (PythonCall*)data;

  // call the function
  PyObject* arg = py_is_none(call->data) ? NULL : call->data;
  PyObjectPtr res(PyObject_CallFunctionObjArgs(call->func, arg, NULL));

  // delete the call object (will decref the members)
  delete call;

  // return status as per https://docs.python.org/3/c-api/init.html#c.Py_AddPendingCall
  if (!res.is_null())
    return 0;
  else
    return -1;
}


extern "C" PyObject* call_python_function_on_main_thread(
                PyObject *self, PyObject* args, PyObject* keywords) {


  // arguments are the python function to call and an optional data argument
  // capture them and then incref them so they survive past this call (we'll
  // decref them in the call_python_function callback)
  PyObject* func = PyTuple_GetItem(args, 0);
  PyObject* data = PyTuple_GetItem(args, 1);

  // create the call object (the func and data will be automaticlaly incref'd then
  // decrefed when the call object is destroyed)
  PythonCall* call = new PythonCall(func, data);

  // Schedule calling the function. Note that we have at least one report of Py_AddPendingCall
  // returning -1, the source code for Py_AddPendingCall is here:
  // https://github.com/python/cpython/blob/faa135acbfcd55f79fb97f7525c8aa6f5a5b6a22/Python/ceval.c#L321-L361
  // From this it looks like it can fail if:
  //
  //   (a) It can't acquire the _PyRuntime.ceval.pending.lock after 100 tries; or
  //   (b) There are more than NPENDINGCALLS already queued
  //
  // As a result we need to check for failure and then sleep and retry in that case.
  //
  // This could in theory result in waiting "forever" but note that if we never successfully
  // add the pending call then we will wait forever anyway as the result queue will
  // never be signaled, i.e. see this code which waits on the call:
  // https://github.com/rstudio/reticulate/blob/b507f954dc08c16710f0fb39328b9770175567c0/inst/python/rpytools/generator.py#L27-L36)
  //
  // As a diagnostic for perverse failure to schedule the call, print a message to stderr
  // every 60 seconds
  //
  const size_t wait_ms = 100;
  size_t waited_ms = 0;
  while(true) {

    // try to schedule the pending call (exit loop on success)
    if (Py_AddPendingCall(call_python_function, call) == 0)
      break;

    // otherwise sleep for wait_ms

    tthread::this_thread::sleep_for(tthread::chrono::milliseconds(wait_ms));

    // increment total wait time and print a warning every 60 seconds
    waited_ms += wait_ms;
    if ((waited_ms % 60000) == 0)
      PySys_WriteStderr("Waiting to schedule call on main R interpeter thread...\n");
  }

  // return none
  Py_IncRef(Py_None);
  return Py_None;
}


PyMethodDef RPYCallMethods[] = {
  { "call_r_function", (PyCFunction)call_r_function,
    METH_VARARGS | METH_KEYWORDS, "Call an R function" },
  { "call_python_function_on_main_thread", (PyCFunction)call_python_function_on_main_thread,
    METH_VARARGS | METH_KEYWORDS, "Call a Python function on the main thread" },
  { NULL, NULL, 0, NULL }
};

static struct PyModuleDef RPYCallModuleDef = {
  PyModuleDef_HEAD_INIT,
  "rpycall",
  NULL,
  -1,
  RPYCallMethods,
  NULL,
  NULL,
  NULL,
  NULL
};

extern "C" PyObject* initializeRPYCall(void) {
  return PyModule_Create(&RPYCallModuleDef, _PYTHON3_ABI_VERSION);
}


// [[Rcpp::export]]
void py_activate_virtualenv(const std::string& script)
{

  // get main dict
  PyObject* main = PyImport_AddModule("__main__");
  PyObject* mainDict = PyModule_GetDict(main);

  // inject __file__
  PyObjectPtr file(as_python_str(script));
  int res = PyDict_SetItemString(mainDict, "__file__", file);
  if (res != 0)
    throw PythonException(py_fetch_error());

  // read the code in the script
  std::ifstream ifs(script.c_str());
  if (!ifs)
    stop("Unable to open file '%s' (does it exist?)", script);
  std::string code((std::istreambuf_iterator<char>(ifs)),
                   (std::istreambuf_iterator<char>()));

  // run string
  PyObjectPtr runRes(PyRun_StringFlags(code.c_str(), Py_file_input, mainDict, NULL, NULL));
  if (runRes.is_null())
    throw PythonException(py_fetch_error());
}

void trace_print(int threadId, PyFrameObject *frame) {
  std::string tracemsg = "";
  while (NULL != frame) {
    std::string filename = as_std_string(frame->f_code->co_filename);
    std::string funcname = as_std_string(frame->f_code->co_name);
    tracemsg = funcname + " " + tracemsg;

    frame = frame->f_back;
  }

  tracemsg = "THREAD: [" + tracemsg + "]\n";
  PySys_WriteStderr(tracemsg.c_str());
}

void trace_thread_main(void* aArg) {


  int* tracems = (int*)aArg;

  while (true) {
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    PyThreadState* pState = PyGILState_GetThisThreadState();

    while (pState != NULL) {
      trace_print(pState->thread_id, pState->frame);
      pState = PyThreadState_Next(pState);
    }

    PyGILState_Release(gstate);

    tthread::this_thread::sleep_for(tthread::chrono::milliseconds(*tracems));
  }
}

tthread::thread* ptrace_thread;
void trace_thread_init(int tracems) {
  ptrace_thread = new tthread::thread(trace_thread_main, &tracems);
}

namespace {

#ifdef _WIN32

SEXP main_process_python_info_win32() {
  // NYI
  return R_NilValue;
}

#else

SEXP main_process_python_info_unix() {

  // bail early if we already know that Python symbols are not available
  // (initialize as true to first assume symbols are available)
  static bool py_symbols_available = true;
  if (!py_symbols_available)
    return R_NilValue;

  // attempt to load some required Python symbols
  void* pLib = NULL;
  pLib = ::dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);

  if (Py_IsInitialized == NULL)
    loadSymbol(pLib, "Py_IsInitialized", (void**) &Py_IsInitialized);

  if (Py_GetVersion == NULL)
    loadSymbol(pLib, "Py_GetVersion", (void**) &Py_GetVersion);

  ::dlclose(pLib);

  // check and see if loading of these symbols failed
  if (Py_IsInitialized == NULL || Py_GetVersion == NULL) {
    py_symbols_available = false;
    return R_NilValue;
  }

  // retrieve DLL info
  Dl_info dinfo;
  if (dladdr((void*) Py_IsInitialized, &dinfo) == 0) {
    py_symbols_available = false;
    return R_NilValue;
  }

  List info;

  if (PyGILState_Ensure == NULL)
    loadSymbol(pLib, "PyGILState_Ensure", (void**)&PyGILState_Ensure);

  if (PyGILState_Release == NULL)
    loadSymbol(pLib, "PyGILState_Release", (void**)&PyGILState_Release);

  GILScope scope(true);

  // read Python program path
  std::string python_path;
  if (Py_GetVersion()[0] >= '3') {
    loadSymbol(pLib, "Py_GetProgramFullPath", (void**) &Py_GetProgramFullPath);
    const std::wstring wide_python_path(Py_GetProgramFullPath());
    python_path = to_string(wide_python_path);
    info["python"] = python_path;
  } else {
    loadSymbol(pLib, "Py_GetProgramFullPath", (void**) &Py_GetProgramFullPath_v2);
    python_path = Py_GetProgramFullPath_v2();
    info["python"] = python_path;
  }

  // read libpython file path
  if (strcmp(dinfo.dli_fname, python_path.c_str()) == 0 ||
      strcmp(dinfo.dli_fname, "python") == 0) {
    // if the library is the same as the executable, it's probably a PIE.
    // Any consequent dlopen on the PIE may fail, return NA to indicate this.
    // when R is embedded by rpy2, dli_fname can be 'python'
    info["libpython"] = Rf_ScalarString(R_NaString);
  } else {
    info["libpython"] = dinfo.dli_fname;
  }

  return info;

}

#endif

} // end anonymous namespace

// [[Rcpp::export]]
SEXP main_process_python_info() {

#ifdef _WIN32
  return main_process_python_info_win32();
#else
  return main_process_python_info_unix();
#endif

}


// [[Rcpp::export]]
void py_clear_error() {
  DBG("Clearing Python errors.");
  PyErr_Clear();
}

bool s_is_python_initialized = false;
bool s_was_python_initialized_by_reticulate = false;

// [[Rcpp::export]]
bool was_python_initialized_by_reticulate() {
  return s_was_python_initialized_by_reticulate;
}

// [[Rcpp::export]]
void py_initialize(const std::string& python,
                   const std::string& libpython,
                   const std::string& pythonhome,
                   const std::string& virtualenv_activate,
                   bool python3,
                   bool interactive,
                   const std::string& numpy_load_error) {

  // set python3 and interactive flags
  s_isPython3 = python3;
  s_isInteractive = interactive;

  if(!s_isPython3)
    warning("Python 2 reached EOL on January 1, 2020. Python 2 compatability will be removed in an upcoming reticulate release.");

  // load the library
  std::string err;
  if (!libPython().load(libpython, is_python3(), &err))
    stop(err);

  if (is_python3()) {

    if (Py_IsInitialized()) {
      // if R is embedded in a python environment, rpycall has to be loaded as a regular
      // module.
      GILScope scope(true);
      PyImport_AddModule("rpycall");
      PyDict_SetItemString(PyImport_GetModuleDict(), "rpycall", initializeRPYCall());

    } else {

      // set program name
      s_python_v3 = to_wstring(python);
      Py_SetProgramName_v3(const_cast<wchar_t*>(s_python_v3.c_str()));

      // set program home
      s_pythonhome_v3 = to_wstring(pythonhome);
      Py_SetPythonHome_v3(const_cast<wchar_t*>(s_pythonhome_v3.c_str()));

      // add rpycall module
      PyImport_AppendInittab("rpycall", &initializeRPYCall);

      // initialize python
      Py_Initialize();
      s_was_python_initialized_by_reticulate = true;
      const wchar_t *argv[1] = {s_python_v3.c_str()};
      PySys_SetArgv_v3(1, const_cast<wchar_t**>(argv));

    }

  } else { // python2

    // set program name
    s_python = python;
    Py_SetProgramName(const_cast<char*>(s_python.c_str()));

    // set program home
    s_pythonhome = pythonhome;
    Py_SetPythonHome(const_cast<char*>(s_pythonhome.c_str()));

    if (!Py_IsInitialized()) {
      // initialize python
      Py_Initialize();
      s_was_python_initialized_by_reticulate = true;
    }

    // add rpycall module
    Py_InitModule4("rpycall", RPYCallMethods, (char *)NULL, (PyObject *)NULL,
                      _PYTHON_API_VERSION);

    const char *argv[1] = {s_python.c_str()};
    PySys_SetArgv(1, const_cast<char**>(argv));
  }

  s_main_thread = tthread::this_thread::get_id();
  s_is_python_initialized = true;
  GILScope scope;

  // initialize type objects
  initialize_type_objects(is_python3());

  // execute activate_this.py script for virtualenv if necessary
  if (!virtualenv_activate.empty())
    py_activate_virtualenv(virtualenv_activate);

  // resovlve numpy
  if (numpy_load_error.empty())
    import_numpy_api(is_python3(), &s_numpy_load_error);
  else
    s_numpy_load_error = numpy_load_error;

  // initialize trace
  Function sysGetEnv("Sys.getenv");
  std::string tracems_env = as<std::string>(sysGetEnv("RETICULATE_DUMP_STACK_TRACE", 0));
  int tracems = ::atoi(tracems_env.c_str());
  if (tracems > 0)
    trace_thread_init(tracems);

  // poll for events while executing python code
  reticulate::event_loop::initialize();

}

// [[Rcpp::export]]
void py_finalize() {
  // We shouldn't call PyFinalize() if R is embedded in Python. https://github.com/rpy2/rpy2/issues/872
  // if(!s_is_python_initialized && !s_was_python_initialized_by_reticulate)
  //   return;
  //
  // ::Py_Finalize();
  // s_is_python_initialized = false;
  // s_was_python_initialized_by_reticulate = false;
}

// [[Rcpp::export]]
bool py_is_none(PyObjectRef x) {
  return py_is_none(x.get());
}

// [[Rcpp::export]]
bool py_compare_impl(PyObjectRef a, PyObjectRef b, const std::string& op) {

  int opcode;
  if (op == "==")
    opcode = Py_EQ;
  else if (op == "!=")
    opcode = Py_NE;
  else if (op == ">")
    opcode = Py_GT;
  else if (op == ">=")
    opcode = Py_GE;
  else if (op == "<")
    opcode = Py_LT;
  else if (op == "<=")
    opcode = Py_LE;
  else
    stop("Unexpected comparison operation " + op);

  // do the comparison
  int res = PyObject_RichCompareBool(a, b, opcode);
  if (res == -1)
    throw PythonException(py_fetch_error());
  else
    return res == 1;
}

// [[Rcpp::export]]
CharacterVector py_str_impl(PyObjectRef x) {

  if (!is_python_str(x)) {

    PyObjectPtr str(PyObject_Str(x));
    if (str.is_null())
      throw PythonException(py_fetch_error());

    return CharacterVector::create(as_utf8_r_string(str));

  }

  return CharacterVector::create(as_utf8_r_string(x));

}


//' @export
//' @rdname py_str
// [[Rcpp::export]]
SEXP py_repr(PyObjectRef object) {

  if(py_is_null_xptr(object))
    return CharacterVector::create(String("<pointer: 0x0>"));

  PyObjectPtr repr(PyObject_Repr(object));

  if (repr.is_null())
    throw PythonException(py_fetch_error());

  return  CharacterVector::create(as_utf8_r_string(repr));
}


// [[Rcpp::export]]
void py_print(PyObjectRef x) {
  CharacterVector out = py_str_impl(x);
  Rf_PrintValue(out);
  Rcout << std::endl;
}

// [[Rcpp::export]]
bool py_is_function(PyObjectRef x) {
  return PyFunction_Check(x) == 1;
}




// [[Rcpp::export]]
bool py_numpy_available_impl() {
  return haveNumPy();
}


// [[Rcpp::export]]
std::vector<std::string> py_list_attributes_impl(PyObjectRef x) {
  std::vector<std::string> attributes;
  PyObjectPtr attrs(PyObject_Dir(x));
  if (attrs.is_null())
    throw PythonException(py_fetch_error());

  Py_ssize_t len = PyList_Size(attrs);
  for (Py_ssize_t index = 0; index<len; index++) {
    PyObject* item = PyList_GetItem(attrs, index);
    attributes.push_back(as_std_string(item));
  }

  return attributes;
}

// [[Rcpp::export]]
bool py_has_attr_impl(PyObjectRef x, const std::string& name) {
  if (py_is_null_xptr(x))
    return false;
  return PyObject_HasAttrString(x, name.c_str());
}

class PyErrorScopeGuard {
private:
  PyObject *er_type, *er_value, *er_traceback;

public:
  PyErrorScopeGuard() {
    PyErr_Fetch(&er_type, &er_value, &er_traceback);
  }

  ~PyErrorScopeGuard() {
    PyErr_Restore(er_type, er_value, er_traceback);
  }
};

// [[Rcpp::export]]
PyObjectRef py_get_attr_impl(PyObjectRef x,
                             const std::string& key,
                             bool silent = false)
{

  PyObject *attr;

  if (silent) {
    PyErrorScopeGuard _g;

    attr = PyObject_GetAttrString(x, key.c_str());
    if (attr == NULL)
      return PyObjectRef(R_EmptyEnv);

  } else {

    attr = PyObject_GetAttrString(x, key.c_str());
    if (attr == NULL)
      throw PythonException(py_fetch_error());

  }

  return py_ref(attr, x.convert());
}

// [[Rcpp::export]]
PyObjectRef py_get_item_impl(PyObjectRef x, RObject key, bool silent = false)
{

  PyObjectPtr py_key(r_to_py(key, x.convert()));
  PyObject *item;

  if (silent) {
    PyErrorScopeGuard _g;

    item = PyObject_GetItem(x, py_key);
    if (item == NULL)
      return PyObjectRef(R_EmptyEnv);

  } else {

    item = PyObject_GetItem(x, py_key);
    if (item == NULL)
      throw PythonException(py_fetch_error());

  }

  return py_ref(item, x.convert());
}

// [[Rcpp::export]]
void py_set_attr_impl(PyObjectRef x,
                      const std::string& name,
                      RObject value)
{
  PyObjectPtr converted(r_to_py(value, x.convert()));
  int res = PyObject_SetAttrString(x, name.c_str(), converted);
  if (res != 0)
    throw PythonException(py_fetch_error());
}

// [[Rcpp::export]]
void py_del_attr_impl(PyObjectRef x,
                      const std::string& name)
{
  int res = PyObject_SetAttrString(x, name.c_str(), NULL);
  if (res != 0)
    throw PythonException(py_fetch_error());
}

// [[Rcpp::export]]
void py_set_item_impl(PyObjectRef x,
                      RObject key,
                      RObject val)
{
  PyObjectPtr py_key(r_to_py(key, x.convert()));
  PyObjectPtr py_val(r_to_py(val, x.convert()));

  int res = PyObject_SetItem(x, py_key, py_val);
  if (res != 0)
    throw PythonException(py_fetch_error());
}


// [[Rcpp::export]]
IntegerVector py_get_attr_types_impl(
    PyObjectRef x,
    const std::vector<std::string>& attrs,
    bool resolve_properties)
{
  const int UNKNOWN     =  0;
  const int VECTOR      =  1;
  const int ARRAY       =  2;
  const int LIST        =  4;
  const int ENVIRONMENT =  5;
  const int FUNCTION    =  6;
  PyErrorScopeGuard _g;
  PyObjectPtr type( PyObject_GetAttrString(x, "__class__") );

  std::size_t n = attrs.size();
  IntegerVector types = no_init(n);
  for (std::size_t i = 0; i < n; i++) {
    const std::string& name = attrs[i];

    // check if this is a property; if so, avoid resolving it unless
    // requested as this could imply running arbitrary Python code
    if (!resolve_properties) {
      PyObjectPtr attr(PyObject_GetAttrString(type, name.c_str()));
      if (attr.is_null())
        PyErr_Clear();
      else if (PyObject_TypeCheck(attr, PyProperty_Type)) {
        types[i] = UNKNOWN;
        continue;
      }
    }

    PyObjectPtr attr(PyObject_GetAttrString(x, name.c_str()));

    if(attr.is_null()) {
      PyErr_Clear();
      types[i] = UNKNOWN;
    }
    else if (attr.get() == Py_None)
      types[i] = UNKNOWN;
    else if (PyType_Check(attr))
      types[i] = UNKNOWN;
    else if (PyCallable_Check(attr))
      types[i] = FUNCTION;
    else if (PyList_Check(attr)  ||
             PyTuple_Check(attr) ||
             PyDict_Check(attr))
      types[i] = LIST;
    else if (isPyArray(attr))
      types[i] = ARRAY;
    else if (PyBool_Check(attr)   ||
             PyInt_Check(attr)    ||
             PyLong_Check(attr)   ||
             PyFloat_Check(attr)  ||
             is_python_str(attr))
      types[i] = VECTOR;
    else if (PyObject_IsInstance(attr, (PyObject*)PyModule_Type))
      types[i] = ENVIRONMENT;
    else
      // presume that other types are objects
      types[i] = LIST;
  }

  return types;
}


// [[Rcpp::export]]
SEXP py_ref_to_r_with_convert(PyObjectRef x, bool convert) {
  return py_to_r(x, convert);
}

// [[Rcpp::export]]
SEXP py_ref_to_r(PyObjectRef x) {
  return py_ref_to_r_with_convert(x, x.convert());
}




// [[Rcpp::export]]
SEXP py_call_impl(PyObjectRef x, List args = R_NilValue, List keywords = R_NilValue) {

  // unnamed arguments
  PyObjectPtr pyArgs(PyTuple_New(args.length()));
  if (args.length() > 0) {
    for (R_xlen_t i = 0; i<args.size(); i++) {
      PyObject* arg = r_to_py(args.at(i), x.convert());
      // NOTE: reference to arg is "stolen" by the tuple
      int res = PyTuple_SetItem(pyArgs, i, arg);
      if (res != 0)
        throw PythonException(py_fetch_error());
    }
  }

  // named arguments
  PyObjectPtr pyKeywords(PyDict_New());
  if (keywords.length() > 0) {
    CharacterVector names = keywords.names();
    SEXP namesSEXP = names;
    for (R_xlen_t i = 0; i<keywords.length(); i++) {
      const char* name = Rf_translateChar(STRING_ELT(namesSEXP, i));
      PyObjectPtr arg(r_to_py(keywords.at(i), x.convert()));
      int res = PyDict_SetItemString(pyKeywords, name, arg);
      if (res != 0)
        throw PythonException(py_fetch_error());
    }
  }

  // call the function
  PyObjectPtr res(PyObject_Call(x, pyArgs, pyKeywords));

  // check for error
  if (res.is_null())
    throw PythonException(py_fetch_error(true));

  // return
  return py_ref(res.detach(), x.convert());
}

// [[Rcpp::export]]
PyObjectRef py_dict_impl(const List& keys, const List& items, bool convert) {

  PyObject* dict = PyDict_New();

  for (R_xlen_t i = 0; i < keys.length(); i++) {
    PyObjectPtr key(r_to_py(keys.at(i), convert));
    PyObjectPtr val(r_to_py(items.at(i), convert));
    PyDict_SetItem(dict, key, val);
  }

  return py_ref(dict, convert);

}


// [[Rcpp::export]]
SEXP py_dict_get_item(PyObjectRef dict, RObject key) {

  if (!PyDict_Check(dict))
    return py_get_item_impl(dict, key, false);

  PyObjectPtr pyKey(r_to_py(key, dict.convert()));

  // NOTE: returns borrowed reference
  PyObject* item = PyDict_GetItem(dict, pyKey);
  if (item == NULL) {
    Py_IncRef(Py_None);
    return py_ref(Py_None, false);
  }

  Py_IncRef(item);
  return py_ref(item, dict.convert());

}

// [[Rcpp::export]]
void py_dict_set_item(PyObjectRef dict, RObject key, RObject val) {

  if (!PyDict_Check(dict))
    return py_set_item_impl(dict, key, val);

  PyObjectPtr py_key(r_to_py(key, dict.convert()));
  PyObjectPtr py_val(r_to_py(val, dict.convert()));
  PyDict_SetItem(dict, py_key, py_val);

}

// [[Rcpp::export]]
int py_dict_length(PyObjectRef dict) {

  if (!PyDict_Check(dict))
    return PyObject_Size(dict);

  return PyDict_Size(dict);

}

namespace {

PyObject* py_dict_get_keys_impl(PyObject* dict) {

  PyObject* keys = PyDict_Keys(dict);

  if (keys == NULL) {
    PyErr_Clear();
    keys = PyObject_CallMethod(dict, "keys", NULL);
    if (keys == NULL)
      throw PythonException(py_fetch_error());
  }

  return keys;

}

} // end anonymous namespace

// [[Rcpp::export]]
PyObjectRef py_dict_get_keys(PyObjectRef dict) {
  PyObject* keys = py_dict_get_keys_impl(dict);
  return py_ref(keys, dict.convert());
}

// [[Rcpp::export]]
CharacterVector py_dict_get_keys_as_str(PyObjectRef dict) {

  // get the dictionary keys
  PyObjectPtr py_keys(py_dict_get_keys_impl(dict));

  // iterate over keys and convert to string
  std::vector<std::string> keys;

  PyObjectPtr it(PyObject_GetIter(py_keys));
  if (it.is_null())
    throw PythonException(py_fetch_error());

  for (PyObject* item = PyIter_Next(it);
       item != NULL;
       item = PyIter_Next(it))
  {
    // decref on scope exit
    PyObjectPtr scope(item);

    // check for python string and use directly
    if (is_python_str(item)) {
      keys.push_back(as_utf8_r_string(item));
      continue;
    }

    // if we don't have a python string, try to create one
    PyObjectPtr str(PyObject_Str(item));
    if (str.is_null())
      throw PythonException(py_fetch_error());

    keys.push_back(as_utf8_r_string(str));

  }

  if (PyErr_Occurred())
    throw PythonException(py_fetch_error());

  return CharacterVector(keys.begin(), keys.end());

}


// [[Rcpp::export]]
PyObjectRef py_tuple(const List& items, bool convert) {

  R_xlen_t n = items.length();
  PyObject* tuple = PyTuple_New(n);
  for (R_xlen_t i = 0; i < n; i++) {
    PyObject* item = r_to_py(items.at(i), convert);
    // NOTE: reference to arg is "stolen" by the tuple
    int res = PyTuple_SetItem(tuple, i, item);
    if (res != 0)
      throw PythonException(py_fetch_error());
  }

  return py_ref(tuple, convert);

}

// [[Rcpp::export]]
int py_tuple_length(PyObjectRef tuple) {

  if (!PyTuple_Check(tuple))
    return PyObject_Size(tuple);

  return PyTuple_Size(tuple);

}


// [[Rcpp::export]]
PyObjectRef py_module_import(const std::string& module, bool convert) {

  PyObject* pModule = py_import(module);
  if (pModule == NULL)
    throw PythonException(py_fetch_error());

  return py_ref(pModule, convert);

}

// [[Rcpp::export]]
void py_module_proxy_import(PyObjectRef proxy) {
  if (proxy.exists("module")) {
    Rcpp::RObject r_module = proxy.getFromEnvironment("module");
    std::string module = as<std::string>(r_module);
    PyObject* pModule = py_import(module);
    if (pModule == NULL)
      throw PythonException(py_fetch_error());
    proxy.set(pModule);
    proxy.remove("module");
  } else {
    stop("Module proxy does not contain module name");
  }
}



// [[Rcpp::export]]
CharacterVector py_list_submodules(const std::string& module) {

  std::vector<std::string> modules;

  PyObject* modulesDict = PyImport_GetModuleDict();
  PyObject *key, *value;
  Py_ssize_t pos = 0;
  std::string prefix = module + ".";
  while (PyDict_Next(modulesDict, &pos, &key, &value)) {
    if (PyString_Check(key) && !py_is_none(value)) {
      std::string name = as_std_string(key);
      if (name.find(prefix) == 0) {
        std::string submodule = name.substr(prefix.length());
        if (submodule.find('.') == std::string::npos)
          modules.push_back(submodule);
      }
    }
  }

  return wrap(modules);
}

// Traverse a Python iterator or generator

// [[Rcpp::export]]
List py_iterate(PyObjectRef x, Function f) {

  // List to return
  std::vector<RObject> list;

  // get the iterator
  PyObjectPtr iterator(PyObject_GetIter(x));
  if (iterator.is_null())
    throw PythonException(py_fetch_error());

  // loop over it
  while (true) {

    // check next item
    PyObjectPtr item(PyIter_Next(iterator));
    if (item.is_null()) {
      // null return means either iteration is done or
      // that there is an error
      if (PyErr_Occurred())
        throw PythonException(py_fetch_error());
      else
        break;
    }

    // call the function
    SEXP param = x.convert()
      ? py_to_r(item, x.convert())
      : py_ref(item.detach(), false);

    list.push_back(f(param));
  }

  // return the list
  List rList(list.size());
  for (size_t i = 0; i < list.size(); i++)
    rList[i] = list[i];
  return rList;
}

// [[Rcpp::export]]
SEXP py_iter_next(PyObjectRef iterator, RObject completed) {

  PyObjectPtr item(PyIter_Next(iterator));
  if (item.is_null()) {

    // null could mean that iteraton is done so we check to
    // ensure that an error actually occrred
    if (PyErr_Occurred())
      throw PythonException(py_fetch_error());

    // if there wasn't an error then return the 'completed' sentinel
    return completed;

  } else {

    // return R object
    return iterator.convert()
      ? py_to_r(item, true)
      : py_ref(item.detach(), false);

  }
}


// [[Rcpp::export]]
SEXP py_run_string_impl(const std::string& code,
                        bool local = false,
                        bool convert = true)
{
  // retrieve reference to main module dictionary
  // note: both PyImport_AddModule() and PyModule_GetDict()
  // return borrowed references
  PyObject* main = PyImport_AddModule("__main__");
  PyObject* globals = PyModule_GetDict(main);

  if (local) {

    // create dictionary to capture locals
    PyObjectPtr locals(PyDict_New());

    // run the requested code
    PyObjectPtr res(PyRun_StringFlags(code.c_str(), Py_file_input, globals, locals, NULL));
    if (res.is_null())
      throw PythonException(py_fetch_error());

    // return locals dictionary (detach so we don't decref on scope exit)
    return py_ref(locals.detach(), convert);

  } else {

    // run the requested code
    PyObjectPtr res(PyRun_StringFlags(code.c_str(), Py_file_input, globals, globals, NULL));
    if (res.is_null())
      throw PythonException(py_fetch_error());

    // because globals is borrowed, we need to incref here
    Py_IncRef(globals);
    return py_ref(globals, convert);

  }

}

// [[Rcpp::export]]
PyObjectRef py_run_file_impl(const std::string& file,
                      bool local = false,
                      bool convert = true) {
  FILE* fp = fopen(file.c_str(), "rb");
  if (fp == NULL) stop("Unable to open file '%s'", file);

  PyObject* main = PyImport_AddModule("__main__");  // borrowed reference
  PyObject* globals = PyModule_GetDict(main);       // borrowed reference
  PyObject* locals;

  if (local)
    locals = PyDict_New();  // new reference
  else {
    locals = globals;
    Py_IncRef(locals);
  }

  PyObjectPtr locals_w_finalizer(locals);  // ensure decref on early return

  if (PyDict_SetItemString(locals, "__file__", as_python_str(file)) < 0)
    throw PythonException(py_fetch_error());

  if (PyDict_SetItemString(locals, "__cached__", Py_None) < 0)
    throw PythonException(py_fetch_error());

  PyObjectPtr res(PyRun_FileEx(fp, file.c_str(), Py_file_input, globals,
                               locals, 1));  // 1 here closes fp before it returns

  if (res.is_null())
    throw PythonException(py_fetch_error());

  // try delete dunders; mimic PyRun_SimpleFile() behavior
  if (PyDict_DelItemString(locals, "__file__"))   PyErr_Clear();
  if (PyDict_DelItemString(locals, "__cached__")) PyErr_Clear();

  if (flush_std_buffers() == -1)
    warning(
        "Error encountered when flushing python buffers sys.stderr and "
        "sys.stdout");

  return py_ref(locals_w_finalizer.detach(), convert);
}

// [[Rcpp::export]]
SEXP py_eval_impl(const std::string& code, bool convert = true) {
  // compile the code
  PyObjectPtr compiledCode;
  if (Py_CompileStringExFlags != NULL)
    compiledCode.assign(Py_CompileStringExFlags(code.c_str(), "reticulate_eval", Py_eval_input, NULL, 0));
  else
    compiledCode.assign(Py_CompileString(code.c_str(), "reticulate_eval", Py_eval_input));


  if (compiledCode.is_null())
    throw PythonException(py_fetch_error());

  // execute the code
  PyObject* main = PyImport_AddModule("__main__");
  PyObject* dict = PyModule_GetDict(main);
  PyObjectPtr local_dict(PyDict_New());
  PyObjectPtr res(PyEval_EvalCode(compiledCode, dict, local_dict));
  if (res.is_null())
    throw PythonException(py_fetch_error());

  // return (convert to R if requested)
  RObject result = convert
    ? py_to_r(res, convert)
    : py_ref(res.detach(), convert);

  return result;
}

template <int RTYPE>
RObject pandas_nullable_collect_values (PyObject* series) {
  size_t size;
  {
    PyObjectPtr _size(PyObject_GetAttrString(series, "size"));
    if (_size.is_null()) {
      throw PythonException(py_fetch_error());
    }
    size = PyLong_AsLong(_size);
  }

  PyObjectPtr iter(PyObject_GetIter(series));
  if (iter.is_null()) {
    throw PythonException(py_fetch_error());
  }

  Vector<RTYPE> output(size, Rcpp::traits::get_na<RTYPE>());
  for(size_t i=0; i<size; i++) {
    PyObjectPtr item(PyIter_Next(iter));

    if (item.is_null()) {
      throw PythonException(py_fetch_error());
    }

    if (!is_pandas_na(item)) {
      output[i] = Rcpp::as<Vector<RTYPE>>(py_to_r(item, true))[0];
    }
  }

  return output;
}

#define NULLABLE_INTEGERS                                      \
"Int8",                                                        \
"Int16",                                                       \
"Int32",                                                       \
"Int64",                                                       \
"UInt8",                                                       \
"UInt16",                                                      \
"UInt32",                                                      \
"UInt64"


SEXPTYPE nullable_typename_to_sexptype (const std::string& name) {
  const static std::set<std::string> nullable_integers({NULLABLE_INTEGERS});

  if (nullable_integers.find(name) != nullable_integers.end()) {
    return INTSXP;
  } else if (name == "Float32" || name == "Float64") {
    return REALSXP;
  } else if (name == "string") {
    return STRSXP;
  } else if (name == "boolean") {
    return LGLSXP;
  }

  Rcpp::stop("Can't cast column with type name: " + name);
}

// [[Rcpp::export]]
SEXP py_convert_pandas_series(PyObjectRef series) {

  // extract dtype
  PyObjectPtr dtype(PyObject_GetAttrString(series, "dtype"));
  const auto name = as_std_string(PyObjectPtr(PyObject_GetAttrString(dtype, "name")));

  const static std::set<std::string> nullable_dtypes({
    NULLABLE_INTEGERS,
    "boolean",
    "Float32",
    "Float64",
    "string"
  });

  RObject R_obj;

  // special treatment for pd.Categorical
  if (name == "category") {

    // get actual values and convert to R
    PyObjectPtr cat(PyObject_GetAttrString(series, "cat"));
    PyObjectPtr codes(PyObject_GetAttrString(cat, "codes"));
    PyObjectPtr code_values(PyObject_GetAttrString(codes, "values"));
    RObject R_values = py_to_r(code_values, true);

    // get levels and convert to R
    PyObjectPtr categories(PyObject_GetAttrString(dtype, "categories"));
    PyObjectPtr category_values(PyObject_GetAttrString(categories, "values"));
    RObject R_levels = py_to_r(category_values, true);

    // get "ordered" attribute
    PyObjectPtr ordered(PyObject_GetAttrString(dtype, "ordered"));


    // populate integer vector to hold factor values
    // note that we need to convert 0->1 indexing, and handle NAs
    int* codes_int = INTEGER(R_values);
    int n = Rf_xlength(R_values);

    // values need to start at 1
    IntegerVector factor(n);
    for (int i = 0; i < n; ++i) {
      int code = codes_int[i];
      factor[i] = code == -1 ? NA_INTEGER : code + 1;
    }

    // populate character vector to hold levels
    CharacterVector factor_levels(R_levels);
    factor_levels.attr("dim") = R_NilValue;

    factor.attr("levels") = factor_levels;
    if (PyObject_IsTrue(ordered))
      factor.attr("class") = CharacterVector({"ordered", "factor"});
    else
      factor.attr("class") = "factor";

    R_obj = factor;

  // special treatment for pd.TimeStamp
  // if available, time zone information will be respected,
  // but values returned to R will be in UTC
  } else if (name == "datetime64[ns]" ||

    // if a time zone is present, dtype is "object"
    PyObject_HasAttrString(series, "dt")) {

    // pd.Series.items() returns an iterator over (index, value) pairs
    PyObjectPtr items(PyObject_CallMethod(series, "items", NULL));

    std::vector<double> posixct;

    while (true) {

      // get next tuple
      PyObjectPtr tuple(PyIter_Next(items));
      if (tuple.is_null()) {
        if (PyErr_Occurred())
          throw PythonException(py_fetch_error());
        else
          break;
      }

     // access value in slot 1
     PyObjectPtr values(PySequence_GetItem(tuple, 1));
     // convert to POSIX timestamp, taking into account time zone (if set)
     PyObjectPtr timestamp(PyObject_CallMethod(values, "timestamp", NULL));

     Datetime R_timestamp;

     // NaT will have thrown "NaTType does not support timestamp"
     if (PyErr_Occurred()) {
       R_timestamp = R_NaN;
       PyErr_Clear();
     } else {
       R_timestamp = py_to_r(timestamp, true);
     }

     posixct.push_back(R_timestamp);

    }

    DatetimeVector R_posixct(posixct.size());
    for (std::size_t i = 0; i < posixct.size(); ++i) {
      R_posixct[i] = posixct[i];
    }

    return R_posixct;


  // Data types starting with Capitalized case are used as the nullable datatypes in
  // Pandas. They use pd.NA to represent missing values and we preserve them in the R
  // arrays.
  } else if (nullable_dtypes.find(name) != nullable_dtypes.end()) {

    // IIFE pattern
    R_obj = [&]() {
      switch (nullable_typename_to_sexptype(name)) {
      case INTSXP: return pandas_nullable_collect_values<INTSXP>(series);
      case REALSXP: return pandas_nullable_collect_values<REALSXP>(series);
      case LGLSXP: return pandas_nullable_collect_values<LGLSXP>(series);
      case STRSXP: return pandas_nullable_collect_values<STRSXP>(series);
      }
      Rcpp::stop("Unsupported data type name: " + name);
    }();

  // default case
  } else {

    PyObjectPtr values(PyObject_GetAttrString(series, "values"));
    R_obj = py_to_r(values, series.convert());

  }

  return R_obj;

}

// [[Rcpp::export]]
SEXP py_convert_pandas_df(PyObjectRef df) {

  // pd.DataFrame.items() returns an iterator over (column name, Series) pairs
  PyObjectPtr items(PyObject_CallMethod(df, "items", NULL));
  if (! (PyObject_HasAttrString(items, "__next__") || PyObject_HasAttrString(items, "next")))
    stop("Cannot iterate over object");

  std::vector<RObject> list;

  while (true) {

    // get next tuple
    PyObjectPtr tuple(PyIter_Next(items));
    if (tuple.is_null()) {
      if (PyErr_Occurred())
        throw PythonException(py_fetch_error());
      else
        break;
    }

    // access Series in slot 1
    PyObjectPtr series(PySequence_GetItem(tuple, 1));

    // delegate to py_convert_pandas_series
    PyObjectRef series_ref(series.detach(), df.convert());
    RObject R_obj = py_convert_pandas_series(series_ref);

    list.push_back(R_obj);

  }

  return List(list.begin(), list.end());

}

PyObject* na_mask (SEXP x) {

  const size_t n(LENGTH(x));
  npy_intp dims(n);

  PyObject* mask(PyArray_SimpleNew(1, &dims, NPY_BOOL));
  if (!mask) throw PythonException(py_fetch_error());

  // Instead of using R's Logical
  // data points to mask 'owned' memory, so we don't need to free it.
  bool* data = (bool*) PyArray_DATA((PyArrayObject*) mask);
  if (!data) throw PythonException(py_fetch_error());

  size_t i;

  // This is modified from R primitive do_isna - backing the `is.na()`:
  // https://github.com/wch/r-source/blob/6b5d4ca5d1e3b4b9e4bbfb8f75577aff396a378a/src/main/coerce.c#L2221
  // Unfortunately couldn't find a simple way to find NA's for whichever atomic type.
  switch (TYPEOF(x)) {
  case LGLSXP:
    for (i = 0; i < n; i++)
      data[i] = (LOGICAL_ELT(x, i) == NA_LOGICAL);
    break;
  case INTSXP:
    for (i = 0; i < n; i++)
      data[i] = (INTEGER_ELT(x, i) == NA_INTEGER);
    break;
  case REALSXP:
    for (i = 0; i < n; i++)
      data[i] = ISNAN(REAL_ELT(x, i));
    break;
  case CPLXSXP:
    for (i = 0; i < n; i++) {
      Rcomplex v = COMPLEX_ELT(x, i);
      data[i] = (ISNAN(v.r) || ISNAN(v.i));
    }
    break;
  case STRSXP:
    for (i = 0; i < n; i++)
      data[i] = (STRING_ELT(x, i) == NA_STRING);
    break;
  }

  return mask;
}

PyObject* r_to_py_pandas_nullable_series (const RObject& column, const bool convert) {

  PyObject* constructor;
  switch (TYPEOF(column)) {
  case INTSXP:
    const static PyObjectPtr IntArray(
        PyObject_GetAttrString(pandas_arrays(), "IntegerArray")
    );
    constructor = IntArray.get();
    break;
  case REALSXP:
    const static PyObjectPtr FloatArray(
        PyObject_GetAttrString(pandas_arrays(), "FloatingArray")
    );
    constructor = FloatArray.get();
    break;
  case LGLSXP:
    const static PyObjectPtr BoolArray(
        PyObject_GetAttrString(pandas_arrays(), "BooleanArray")
    );
    constructor = BoolArray.get();
    break;
  case STRSXP:
    const static PyObjectPtr StringArray(
        PyObject_GetAttrString(pandas_arrays(), "StringArray")
    );
    constructor = StringArray.get();
    break;
  default:
    Rcpp::stop("R type not handled. Please supply one of int, double, logical or character");
  }

  if (!constructor) {
    // if the constructor is not available it means that the user doesn't have
    // the minimum pandas version.
    // we show a warning and force the numpy construction.
    Rcpp::warning(
      "Nullable data types require pandas version >= 1.2.0. "
      "Forcing numpy cast. Use `options(reticulate.pandas_use_nullable_dtypes = FALSE)` "
      "to disable this warning."
    );

    return r_to_py_numpy(column, convert);
  }

  // strings are not built using np array + mask. Instead they take a
  // np array with OBJECT type, with None's in the place of NA's
  if (TYPEOF(column) == STRSXP) {
    PyObjectPtr args(PyTuple_New(2));
    PyTuple_SetItem(args, 0, (PyObject*)r_to_py_numpy(column, convert));
    PyTuple_SetItem(args, 1, Py_False);

    PyObject* pd_col(PyObject_Call(constructor, args, NULL));

    if (!pd_col) {
      // it's likely that the error is caused by using an old version of pandas
      // that don't accept `None` as a `NA` value.
      // we force the old cast method after a warning.
      Rcpp::warning(
        "String nullable data types require pandas version >= 1.5.0. "
        "Forcing numpy cast. Use `options(reticulate.pandas_use_nullable_dtypes = FALSE)` "
        "to disable this warning."
      );

      return r_to_py_numpy(column, convert);
    }

    return pd_col;
  }

  // tuples own the objects - thus we don't leak the value and mask
  PyObjectPtr args(PyTuple_New(3));
  PyTuple_SetItem(args, 0, (PyObject*)r_to_py_numpy(column, convert)); // value
  PyTuple_SetItem(args, 1, (PyObject*)na_mask(column));                // mask
  PyTuple_SetItem(args, 2, Py_False);                                  // copy=False

  PyObject* pd_col(PyObject_Call(constructor, args, NULL));
  return pd_col;
}

// [[Rcpp::export]]
PyObjectRef r_convert_dataframe(RObject dataframe, bool convert) {

  Function r_convert_dataframe_column =
    Environment::namespace_env("reticulate")["r_convert_dataframe_column"];

  PyObjectPtr dict(PyDict_New());

  CharacterVector names = dataframe.attr("names");
  // when this is set we cast R atomic vectors to numpy arrays and don't
  // use pandas dtypes that can handle missing values.
  bool nullable_dtypes = option_is_true("reticulate.pandas_use_nullable_dtypes");

  for (R_xlen_t i = 0, n = Rf_xlength(dataframe); i < n; i++)
  {
    RObject column = VECTOR_ELT(dataframe, i);

    // ensure name is converted to appropriate encoding
    PyObjectPtr name(as_python_str(names[i]));

    int status = 0;

    if (OBJECT(column) != 0) {
      // An object with a class attribute, we dispatch to the S3 method
      // and continue to the next column.
      PyObjectRef ref(r_convert_dataframe_column(column, convert));
      status = PyDict_SetItem(dict, name, ref.get());
      if (status != 0)
        throw PythonException(py_fetch_error());

      continue;
    }

    if (!is_convertible_to_numpy(column)) {
      // Not an atomic type supported by numpy, thus we use the default
      // cast engine and continue to the next column.
      PyObjectPtr value(r_to_py_cpp(column, convert));
      status = PyDict_SetItem(dict, name, value);

      if (status != 0)
        throw PythonException(py_fetch_error());

      continue;
    }

    // We are sure it's an atomic vector:
    // Atomic values STRSXP, INTSXP, REALSXP and CPLSXP
    if (!nullable_dtypes || TYPEOF(column) == CPLXSXP) {
      PyObjectPtr value(r_to_py_numpy(column, convert));
      status = PyDict_SetItem(dict, name, value);
    } else {
      // use Pandas nullable data types.
      PyObjectPtr value(r_to_py_pandas_nullable_series(column, convert));
      status = PyDict_SetItem(dict, name, value);
    }

    if (status != 0)
      throw PythonException(py_fetch_error());
  }

  return py_ref(dict.detach(), convert);
}

namespace {

PyObject* r_convert_date_impl(PyObject* datetime,
                              Date date)
{

  PyObjectPtr py_date(PyObject_CallMethod(
      datetime, "date", "iii",
      static_cast<int>(date.getYear()),
      static_cast<int>(date.getMonth()),
      static_cast<int>(date.getDay())));

  if (py_date == NULL)
    throw PythonException(py_fetch_error());

  return py_date.detach();
}

} // end anonymous namespace

// [[Rcpp::export]]
PyObjectRef r_convert_date(DateVector dates, bool convert) {

  PyObjectPtr datetime(PyImport_ImportModule("datetime"));

  // short path for n == 1
  R_xlen_t n = dates.size();
  if (n == 1) {
    Date date = dates[0];
    return py_ref(r_convert_date_impl(datetime, date), convert);
  }

  // regular path for n > 1
  PyObjectPtr list(PyList_New(n));

  for (R_xlen_t i = 0; i < n; ++i) {
    Date date = dates[i];
    PyList_SetItem(list, i, r_convert_date_impl(datetime, date));
  }

  return py_ref(list.detach(), convert);

}

// [[Rcpp::export]]
void py_set_interrupt_impl() {
  PyErr_SetInterrupt();
}

// [[Rcpp::export]]
SEXP py_list_length(PyObjectRef x) {
  Py_ssize_t value = PyList_Size(x);
  if (value <= static_cast<Py_ssize_t>(INT_MAX))
    return Rf_ScalarInteger((int) value);
  else
    return Rf_ScalarReal((double) value);
}

// [[Rcpp::export]]
SEXP py_len_impl(PyObjectRef x, SEXP defaultValue = R_NilValue) {

  PyObject *er_type, *er_value, *er_traceback;
  if (defaultValue != R_NilValue)
    PyErr_Fetch(&er_type, &er_value, &er_traceback);

  Py_ssize_t value = PyObject_Size(x);
  if (value == -1) {
   // object is missing a `__len__` method, or a `__len__` method that
   // intentionally raises an Exception
    if (defaultValue == R_NilValue) {
      throw PythonException(py_fetch_error());
    } else {
      PyErr_Restore(er_type, er_value, er_traceback);
      return defaultValue;
    }
  }

  if (value <= static_cast<Py_ssize_t>(INT_MAX))
    return Rf_ScalarInteger((int) value);
  else
    return Rf_ScalarReal((double) value);
}

// [[Rcpp::export]]
SEXP py_bool_impl(PyObjectRef x) {

  // evaluate Python `not not x`
  int result = PyObject_IsTrue(x);

  if (result == -1) {
  // Should only happen if the object has a `__bool__` method that
  // intentionally throws an exception.
    throw PythonException(py_fetch_error());
  }

  return Rf_ScalarLogical(result);
}


// [[Rcpp::export]]
SEXP py_has_method(PyObjectRef object, const std::string& name) {

  if (py_is_null_xptr(object))
    return Rf_ScalarLogical(false);

  if (!PyObject_HasAttrString(object, name.c_str()))
    return Rf_ScalarLogical(false);

  PyObjectPtr attr(PyObject_GetAttrString(object, name.c_str()));
  int result = PyMethod_Check(attr);

  return Rf_ScalarLogical(result);
}


//' Unique identifer for Python object
//'
//' Get a globally unique identifier for a Python object.
//'
//' @note In the current implementation of CPython this is the
//'  memory address of the object.
//'
//' @param object Python object
//'
//' @return Unique identifer (as string) or `NULL`
//'
//' @export
// [[Rcpp::export]]
SEXP py_id(PyObjectRef object) {
  if (py_is_null_xptr(object))
    return R_NilValue;

  std::stringstream id;
  id << (uintptr_t) object.get();

  return CharacterVector({id.str()});
}

void ensure_python_initialized() {
  if (s_is_python_initialized)
    return;

  Function initialize = Environment::namespace_env("reticulate")["ensure_python_initialized"];
  initialize();
}

// [[Rcpp::export]]
PyObjectRef py_capsule(SEXP x) {
  if(!s_is_python_initialized)
    ensure_python_initialized();

  return py_ref(py_capsule_new(x), false);
}


// [[Rcpp::export]]
PyObjectRef py_slice(SEXP start = R_NilValue, SEXP stop = R_NilValue, SEXP step = R_NilValue) {
  if(!s_is_python_initialized)
    ensure_python_initialized();

  PyObjectPtr start_, stop_, step_;

  if (start != R_NilValue)
    start_.assign(PyLong_FromLong(Rf_asInteger(start)));
  if (stop != R_NilValue)
    stop_.assign(PyLong_FromLong(Rf_asInteger(stop)));
  if (step != R_NilValue)
    step_.assign(PyLong_FromLong(Rf_asInteger(step)));

  PyObject* out(PySlice_New(start_, stop_, step_));
  if (out == NULL)
    throw PythonException(py_fetch_error());
  return py_ref(out, false);
}
