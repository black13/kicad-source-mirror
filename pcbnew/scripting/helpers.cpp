#include <Python.h>
#include <wx/intl.h>
#include <wx/string.h>

#define WX_DEFAULTENCODING_SIZE 64

static char wxPythonEncoding[WX_DEFAULTENCODING_SIZE] = "ascii";

wxString* newWxStringFromPy(PyObject* src) {

    bool must_unref_str=false;

    wxString* result = NULL;
    PyObject *obj=src;

#if wxUSE_UNICODE
    bool must_unref_obj=false;    
    // Unicode string to python unicode string 
    PyObject* uni_str = src;

    // if not an str or unicode, try to str(src)
    if (!PyString_Check(src) && !PyUnicode_Check(src)) 
    {
        obj = PyObject_Str(src);
	must_unref_obj = true;
        if (PyErr_Occurred()) return NULL;
    }

    if (PyString_Check(obj)) 
    {
        uni_str = PyUnicode_FromEncodedObject(obj, wxPythonEncoding, "strict");
        must_unref_str = true;
        if (PyErr_Occurred()) return NULL;
    }

    result = new wxString();
    size_t len = PyUnicode_GET_SIZE(uni_str);
    if (len) 
    {
        PyUnicode_AsWideChar((PyUnicodeObject*)uni_str,
                             wxStringBuffer(*result, len),len);
    }

    if (must_unref_str) Py_DECREF(uni_str);
    if (must_unref_obj) Py_DECREF(obj);
#else
    // normal string (or object) to normal python string 
    PyObject* str = src;

    if (PyUnicode_Check(src)) // if it's unicode convert to normal string
    {
        str = PyUnicode_AsEncodedString(src, wxPythonEncoding, "strict");
        if (PyErr_Occurred()) return NULL;
    }
    else if (!PyString_Check(src)) // if it's not a string, str(obj)
    {
        str = PyObject_Str(src);
 	must_unref_str = true;
        if (PyErr_Occurred()) return NULL;
    }

    // get the string pointer and size
    char* str_ptr; 
    Py_ssize_t str_size;
    PyString_AsStringAndSize(str, &str_ptr, &str_size);

    // build the wxString from our pointer / size
    result = new wxString(str_ptr, str_size);

    if (must_unref_str) Py_DECREF(str);
#endif 

    return result;
}


wxString Py2wxString(PyObject* src)
{
    wxString result;
    wxString* resPtr = newWxStringFromPy(src);    
    
    // In case of exception clear it and return an empty string  
    if (resPtr==NULL) 
    {
	PyErr_Clear();
	return wxEmptyString;
    }
    
    result = *resPtr;

    delete resPtr;    

    return result;
}


PyObject* wx2PyString(const wxString& src)
{
    PyObject* str;
#if wxUSE_UNICODE
    str = PyUnicode_FromWideChar(src.c_str(), src.Len());
#else
    str = PyString_FromStringAndSize(src.c_str(), src.Len());
#endif
    return str;
}



void wxSetDefaultPyEncoding(const char* encoding)
{
    strncpy(wxPythonEncoding, encoding, WX_DEFAULTENCODING_SIZE);
}

const char* wxGetDefaultPyEncoding()
{
    return wxPythonEncoding;
}

