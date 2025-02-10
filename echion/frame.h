// This file is part of "echion" which is released under MIT.
//
// Copyright (c) 2023 Gabriele N. Tornetta <phoenix1987@gmail.com>.

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <frameobject.h>
#if PY_VERSION_HEX >= 0x030d0000
#include <internal/pycore_code.h>
#endif // PY_VERSION_HEX >= 0x030d0000
#if PY_VERSION_HEX >= 0x030b0000
#include <internal/pycore_frame.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>

#include <cxxabi.h>
#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include <echion/cache.h>
#include <echion/mojo.h>
#include <echion/strings.h>
#include <echion/vm.h>

// ----------------------------------------------------------------------------
#if PY_VERSION_HEX >= 0x030b0000
static inline int
_read_varint(unsigned char *table, ssize_t size, ssize_t *i)
{
    ssize_t guard = size - 1;
    if (*i >= guard)
        return 0;

    int val = table[++*i] & 63;
    int shift = 0;
    while (table[*i] & 64 && *i < guard)
    {
        shift += 6;
        val |= (table[++*i] & 63) << shift;
    }
    return val;
}

// ----------------------------------------------------------------------------
static inline int
_read_signed_varint(unsigned char *table, ssize_t size, ssize_t *i)
{
    int val = _read_varint(table, size, i);
    return (val & 1) ? -(val >> 1) : (val >> 1);
}
#endif

// ----------------------------------------------------------------------------
class Frame
{
public:
    using Ref = std::reference_wrapper<Frame>;
    using Ptr = std::unique_ptr<Frame>;
    using Key = uintptr_t;

    // ------------------------------------------------------------------------
    class Error : public std::exception
    {
    public:
        const char *what() const noexcept override
        {
            return "Cannot read frame";
        }
    };

    // ------------------------------------------------------------------------
    class LocationError : public Error
    {
    public:
        const char *what() const noexcept override
        {
            return "Cannot determine frame location information";
        }
    };

    // ------------------------------------------------------------------------

    Key cache_key = 0;
    StringTable::Key filename = 0;
    StringTable::Key name = 0;

    struct _location
    {
        int line = 0;
        int line_end = 0;
        int column = 0;
        int column_end = 0;
    } location;

#if PY_VERSION_HEX >= 0x030b0000
    bool is_entry = false;
#endif

    // ------------------------------------------------------------------------

    Frame(StringTable::Key name) : name(name){};

    static Frame &read(PyObject *frame_addr, PyObject **prev_addr);

    static Frame &get(PyCodeObject *code_addr, int lasti);
    static Frame &get(PyObject *frame);
    static Frame &get(unw_cursor_t &cursor);
    static Frame &get(StringTable::Key name);

    // ------------------------------------------------------------------------
    Frame(PyObject *frame)
    {
#if PY_VERSION_HEX >= 0x030b0000

#if PY_VERSION_HEX >= 0x030d0000
        _PyInterpreterFrame* iframe = (_PyInterpreterFrame *)frame;
        const int lasti = _PyInterpreterFrame_LASTI(iframe);
        PyCodeObject* code = (PyCodeObject*)iframe->f_executable;
#else
        const _PyInterpreterFrame *iframe = (_PyInterpreterFrame *)frame;
        const int lasti = _PyInterpreterFrame_LASTI(iframe);
        PyCodeObject *code = iframe->f_code;
#endif // PY_VERSION_HEX >= 0x030d0000
        PyCode_Addr2Location(code, lasti << 1, &location.line, &location.column, &location.line_end, &location.column_end);
        location.column++;
        location.column_end++;
        name = string_table.key_unsafe(code->co_qualname);
#if PY_VERSION_HEX >= 0x030c0000
        is_entry = (iframe->owner == FRAME_OWNED_BY_CSTACK); // Shim frame
#else
        is_entry = iframe->is_entry;
#endif

#else
        PyFrameObject *py_frame = (PyFrameObject *)frame;
        PyCodeObject *code = py_frame->f_code;

        location.line = PyFrame_GetLineNumber(py_frame);
        name = string_table.key_unsafe(code->co_name);
#endif
        filename = string_table.key_unsafe(code->co_filename);
    }

    // ------------------------------------------------------------------------
    Frame(PyCodeObject *code, int lasti)
    {
        try
        {
            filename = string_table.key(code->co_filename);
#if PY_VERSION_HEX >= 0x030b0000
            name = string_table.key(code->co_qualname);
#else
            name = string_table.key(code->co_name);
#endif
        }
        catch (StringTable::Error &)
        {
            throw Error();
        }

        infer_location(code, lasti);
    }

    // ------------------------------------------------------------------------
    Frame(unw_cursor_t &cursor, unw_word_t pc)
    {
        try
        {
            filename = string_table.key(pc);
            name = string_table.key(cursor);
        }
        catch (StringTable::Error &)
        {
            throw Error();
        }
    }

    // ------------------------------------------------------------------------
    void render_where(std::ostream &stream)
    {
        if ((string_table.lookup(filename)).rfind("native@", 0) == 0)
            stream << "          \033[38;5;248;1m" << string_table.lookup(name)
                   << "\033[0m \033[38;5;246m(" << string_table.lookup(filename)
                   << "\033[0m:\033[38;5;246m" << location.line
                   << ")\033[0m" << std::endl;
        else
            stream << "          \033[33;1m" << string_table.lookup(name)
                   << "\033[0m (\033[36m" << string_table.lookup(filename)
                   << "\033[0m:\033[32m" << location.line
                   << "\033[0m)" << std::endl;
    }

private:
    // ------------------------------------------------------------------------
    void inline infer_location(PyCodeObject *code, int lasti)
    {
        unsigned int lineno = code->co_firstlineno;
        Py_ssize_t len = 0;

#if PY_VERSION_HEX >= 0x030b0000
        auto table = pybytes_to_bytes_and_size(code->co_linetable, &len);
        if (table == nullptr)
            throw LocationError();

        auto table_data = table.get();

        for (Py_ssize_t i = 0, bc = 0; i < len; i++)
        {
            bc += (table[i] & 7) + 1;
            int code = (table[i] >> 3) & 15;
            unsigned char next_byte = 0;
            switch (code)
            {
            case 15:
                break;

            case 14: // Long form
                lineno += _read_signed_varint(table_data, len, &i);

                this->location.line = lineno;
                this->location.line_end = lineno + _read_varint(table_data, len, &i);
                this->location.column = _read_varint(table_data, len, &i);
                this->location.column_end = _read_varint(table_data, len, &i);

                break;

            case 13: // No column data
                lineno += _read_signed_varint(table_data, len, &i);

                this->location.line = lineno;
                this->location.line_end = lineno;
                this->location.column = this->location.column_end = 0;

                break;

            case 12: // New lineno
            case 11:
            case 10:
                if (i >= len - 2)
                    throw LocationError();

                lineno += code - 10;

                this->location.line = lineno;
                this->location.line_end = lineno;
                this->location.column = 1 + table[++i];
                this->location.column_end = 1 + table[++i];

                break;

            default:
                if (i >= len - 1)
                    throw LocationError();

                next_byte = table[++i];

                this->location.line = lineno;
                this->location.line_end = lineno;
                this->location.column = 1 + (code << 3) + ((next_byte >> 4) & 7);
                this->location.column_end = this->location.column + (next_byte & 15);
            }

            if (bc > lasti)
                break;
        }

#elif PY_VERSION_HEX >= 0x030a0000
        auto table = pybytes_to_bytes_and_size(code->co_linetable, &len);
        if (table == nullptr)
            throw LocationError();

        lasti <<= 1;
        for (int i = 0, bc = 0; i < len; i++)
        {
            int sdelta = table[i++];
            if (sdelta == 0xff)
                break;

            bc += sdelta;

            int ldelta = table[i];
            if (ldelta == 0x80)
                ldelta = 0;
            else if (ldelta > 0x80)
                lineno -= 0x100;

            lineno += ldelta;
            if (bc > lasti)
                break;
        }

#else
        auto table = pybytes_to_bytes_and_size(code->co_lnotab, &len);
        if (table == nullptr)
            throw LocationError();

        for (int i = 0, bc = 0; i < len; i++)
        {
            bc += table[i++];
            if (bc > lasti)
                break;

            if (table[i] >= 0x80)
                lineno -= 0x100;

            lineno += table[i];
        }

#endif

        this->location.line = lineno;
        this->location.line_end = lineno;
        this->location.column = 0;
        this->location.column_end = 0;
    }

    // ------------------------------------------------------------------------
    static inline Key key(PyCodeObject *code, int lasti)
    {
        return (((uintptr_t)(((uintptr_t)code) & MOJO_INT32) << 16) | lasti);
    }

    // ------------------------------------------------------------------------
    static inline Key key(PyObject *frame)
    {

#if PY_VERSION_HEX >= 0x030d0000
        _PyInterpreterFrame *iframe = (_PyInterpreterFrame *)frame;
        const int lasti = _PyInterpreterFrame_LASTI(iframe);
        PyCodeObject* code = (PyCodeObject*)iframe->f_executable;
#elif PY_VERSION_HEX >= 0x030b0000
        const _PyInterpreterFrame *iframe = (_PyInterpreterFrame *)frame;
        const int lasti = _PyInterpreterFrame_LASTI(iframe);
        PyCodeObject *code = iframe->f_code;
#else
        const PyFrameObject *py_frame = (PyFrameObject *)frame;
        const int lasti = py_frame->f_lasti;
        PyCodeObject *code = py_frame->f_code;
#endif
        return key(code, lasti);
    }
};

// ----------------------------------------------------------------------------

static auto INVALID_FRAME = Frame(StringTable::INVALID);
static auto UNKNOWN_FRAME = Frame(StringTable::UNKNOWN);

// We make this a raw pointer to prevent its destruction on exit, since we
// control the lifetime of the cache.
static LRUCache<uintptr_t, Frame> *frame_cache = nullptr;

// ----------------------------------------------------------------------------
static void init_frame_cache(size_t capacity)
{
    frame_cache = new LRUCache<uintptr_t, Frame>(capacity);
}

// ----------------------------------------------------------------------------
static void reset_frame_cache()
{
    delete frame_cache;
    frame_cache = nullptr;
}

// ------------------------------------------------------------------------
Frame &Frame::read(PyObject *frame_addr, PyObject **prev_addr)
{
#if PY_VERSION_HEX >= 0x030b0000
    _PyInterpreterFrame iframe;
#if PY_VERSION_HEX >= 0x030d0000
    // From Python versions 3.13, f_executable can have objects other than
    // code objects for an internal frame. We need to skip some frames if
    // its f_executable is not code as suggested here:
    // https://github.com/python/cpython/issues/100987#issuecomment-1485556487
    PyObject f_executable;

    while (frame_addr != NULL) {
        if (copy_type((_PyInterpreterFrame*)frame_addr, iframe) ||
            copy_type(iframe.f_executable, f_executable)) {
            throw Frame::Error();
        }
        if (f_executable.ob_type == &PyCode_Type) {
            break;
        }
        frame_addr = (PyObject*)((_PyInterpreterFrame *)frame_addr)->previous;
    }

    if (frame_addr == NULL) {
        throw Frame::Error();
    }

#endif // PY_VERSION_HEX >= 0x030d0000

    if (copy_type(frame_addr, iframe))
        throw Error();

    // We cannot use _PyInterpreterFrame_LASTI because _PyCode_CODE reads
    // from the code object.
#if PY_VERSION_HEX >= 0x030d0000
    const int lasti = ((int)(iframe.instr_ptr - 1 - (_Py_CODEUNIT *)((PyCodeObject*)iframe.f_executable))) - offsetof(PyCodeObject, co_code_adaptive) / sizeof(_Py_CODEUNIT);
    auto &frame = Frame::get((PyCodeObject*)iframe.f_executable, lasti);
#else
    const int lasti = ((int)(iframe.prev_instr - (_Py_CODEUNIT *)(iframe.f_code))) - offsetof(PyCodeObject, co_code_adaptive) / sizeof(_Py_CODEUNIT);
    auto &frame = Frame::get(iframe.f_code, lasti);
#endif // PY_VERSION_HEX >= 0x030d0000
    if (&frame != &INVALID_FRAME)
    {
#if PY_VERSION_HEX >= 0x030c0000
        frame.is_entry = (iframe.owner == FRAME_OWNED_BY_CSTACK); // Shim frame
#else
        frame.is_entry = iframe.is_entry;
#endif
    }

    *prev_addr = &frame == &INVALID_FRAME ? NULL : (PyObject *)iframe.previous;

#else // Python < 3.11
    // Unwind the stack from leaf to root and store it in a stack. This way we
    // can print it from root to leaf.
    PyFrameObject py_frame;

    if (copy_type(frame_addr, py_frame))
        throw Error();

    auto &frame = Frame::get(py_frame.f_code, py_frame.f_lasti);

    *prev_addr = (&frame == &INVALID_FRAME) ? NULL : (PyObject *)py_frame.f_back;
#endif

    return frame;
}

// ----------------------------------------------------------------------------
Frame &Frame::get(PyCodeObject *code_addr, int lasti)
{
    PyCodeObject code;
    if (copy_type(code_addr, code))
        return INVALID_FRAME;

    auto frame_key = Frame::key(code_addr, lasti);

    try
    {
        return frame_cache->lookup(frame_key);
    }
    catch (LRUCache<uintptr_t, Frame>::LookupError &)
    {
        try
        {
            auto new_frame = std::make_unique<Frame>(&code, lasti);
            new_frame->cache_key = frame_key;
            auto &f = *new_frame;
            mojo.frame(
                frame_key,
                new_frame->filename,
                new_frame->name,
                new_frame->location.line, new_frame->location.line_end,
                new_frame->location.column, new_frame->location.column_end);
            frame_cache->store(frame_key, std::move(new_frame));
            return f;
        }
        catch (Frame::Error &)
        {
            return INVALID_FRAME;
        }
    }
}

// ----------------------------------------------------------------------------
Frame &Frame::get(PyObject *frame)
{
    auto frame_key = Frame::key(frame);

    try
    {
        return frame_cache->lookup(frame_key);
    }
    catch (LRUCache<uintptr_t, Frame>::LookupError &)
    {
        auto new_frame = std::make_unique<Frame>(frame);
        new_frame->cache_key = frame_key;
        auto &f = *new_frame;
        mojo.frame(
            frame_key,
            new_frame->filename,
            new_frame->name,
            new_frame->location.line, new_frame->location.line_end,
            new_frame->location.column, new_frame->location.column_end);
        frame_cache->store(frame_key, std::move(new_frame));
        return f;
    }
}

// ----------------------------------------------------------------------------
Frame &Frame::get(unw_cursor_t &cursor)
{
    unw_word_t pc;
    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    if (pc == 0)
        throw Error();

    uintptr_t frame_key = (uintptr_t)pc;
    try
    {
        return frame_cache->lookup(frame_key);
    }
    catch (LRUCache<uintptr_t, Frame>::LookupError &)
    {
        try
        {
            auto frame = std::make_unique<Frame>(cursor, pc);
            frame->cache_key = frame_key;
            auto &f = *frame;
            mojo.frame(
                frame_key,
                frame->filename,
                frame->name,
                frame->location.line, frame->location.line_end,
                frame->location.column, frame->location.column_end);
            frame_cache->store(frame_key, std::move(frame));
            return f;
        }
        catch (Frame::Error &)
        {
            return UNKNOWN_FRAME;
        }
    }
}

// ----------------------------------------------------------------------------
Frame &Frame::get(StringTable::Key name)
{
    uintptr_t frame_key = (uintptr_t)name;
    try
    {
        return frame_cache->lookup(frame_key);
    }
    catch (LRUCache<uintptr_t, Frame>::LookupError &)
    {
        auto frame = std::make_unique<Frame>(name);
        frame->cache_key = frame_key;
        auto &f = *frame;
        mojo.frame(
            frame_key,
            frame->filename,
            frame->name,
            frame->location.line, frame->location.line_end,
            frame->location.column, frame->location.column_end);
        frame_cache->store(frame_key, std::move(frame));
        return f;
    }
}
