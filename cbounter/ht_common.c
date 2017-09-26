//-----------------------------------------------------------------------------
// Author: Filip Stefanak <f.stefanak@rare-technologies.com>
// Copyright (C) 2017 Rare Technologies
//
// This code is distributed under the terms and conditions
// from the MIT License (MIT).

#define GLUE(x,y) x##y
#define GLUE_I(x,y) GLUE(x, y)
#define HT_VARIANT(suffix) GLUE_I(HT_TYPE, suffix)

#include <Python.h>
#include "structmember.h"
#include "murmur3.h"
#include "hll.h"
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

typedef struct {
    char* key;
    long long count;
} HT_VARIANT(_cell_t);

typedef struct {
    PyObject_HEAD
    uint32_t buckets;
    uint32_t hash_mask;
    uint64_t str_allocated;
    long long total;
    uint32_t size; // number of allocated buckets
    HT_VARIANT(_cell_t) * table;
    uint32_t * histo;
    long long max_prune;
    HyperLogLog hll;
} HT_TYPE;

#define ITER_RESULT_KEYS 1
#define ITER_RESULT_VALUES 2
#define ITER_RESULT_KV_PAIRS 3

typedef struct {
  PyObject_HEAD
  HT_TYPE * hashtable;
  uint32_t i;
  char result_type;
} HT_VARIANT(_ITER_TYPE);

/* Destructor invoked by python. */
static void
HT_VARIANT(_dealloc)(HT_TYPE* self)
{
    HT_VARIANT(_cell_t) * table = self->table;
    // free the strings
    uint32_t i;
    if (self->table)
    {
        for (i = 0; i < self->buckets; i++)
        {
            if (table[i].key)
            {
                free(table[i].key);
            }
        }
    }

    // free the hashtable and histogram
    free(table);
    free(self->histo);
    HyperLogLog_dealloc(&self->hll);

    // finally, destroy itself
    #if PY_MAJOR_VERSION >= 3
    Py_TYPE(self)->tp_free((PyObject*) self);
    #else
    self->ob_type->tp_free((PyObject*) self);
    #endif
}

static PyObject *
HT_VARIANT(_new)(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    HT_TYPE *self;
    self = (HT_TYPE *)type->tp_alloc(type, 0);
    return (PyObject *)self;
}

static int
HT_VARIANT(_init)(HT_TYPE *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"buckets", NULL};
    long long w;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L", kwlist,
				      &w)) {
        return -1;
    }
    if (w < 4)
    {
        char * msg = "The number of buckets must be at least 4!";
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }
    if (w > 0xFFFFFFFF)
    {
        char * msg = "The number of buckets is too large!";
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }

    short int hash_length = -1;
    while (0 != w)
        hash_length++, w >>= 1;
    if (hash_length < 0)
        hash_length = 0;
    self->buckets = 1 << hash_length;
    self->hash_mask = self->buckets - 1;

    self->table = (HT_VARIANT(_cell_t) *) calloc(self->buckets, sizeof(HT_VARIANT(_cell_t)));
    if (!self->table)
    {
        char * msg = "Unable to allocate a table with requested size!";
        PyErr_SetString(PyExc_MemoryError, msg);
        return -1;
    }

    self->histo = calloc(256, sizeof(uint32_t));
    self->total = 0;
    self->size = 0;
    self->max_prune = 0;

    HyperLogLog_init(&self->hll, 16);

    return 0;
}

static PyMemberDef HT_VARIANT(_members[]) = {
    {NULL} /* Sentinel */
};

static inline uint32_t HT_VARIANT(_bucket)(HT_TYPE * self, char * data, uint32_t dataLength, char store)
{
    uint32_t bucket;
    MurmurHash3_x86_32((void *) data, dataLength, 42, (void *) &bucket);
    if (store)
        HyperLogLog_add(&self->hll, bucket);
    bucket &= self->hash_mask;
    return bucket;
}

static inline HT_VARIANT(_cell_t) * HT_VARIANT(_find_cell)(HT_TYPE * self, char * data, uint32_t dataLength, char store)
{
    uint32_t bucket = HT_VARIANT(_bucket)(self, data, dataLength, store);
    const HT_VARIANT(_cell_t) * table = self->table;

    while (table[bucket].key && strcmp(table[bucket].key, data))
    {
        bucket = (bucket + 1) & self->hash_mask;
    }
    return &table[bucket];
}

static inline uint8_t HT_VARIANT(_histo_addr)(long long value)
{
    if (value < 0)
        return 0;
    if (value < 16)
        return value;
    if (value >= 0x3C0000000)
        return 255;

    uint8_t log_result = 1;
    long long h = value;
    while (h > 15)
    {
        log_result += 1;
        h = h >> 1;
    }
    return (log_result << 3) + (h & 7);
}

static void HT_VARIANT(_prune_int)(HT_TYPE *self, long long boundary);

static long long HT_VARIANT(_prune_size)(HT_TYPE * self)
{
    uint32_t required = self->size - (self->buckets >> 1);
    long long index = 0;
    uint32_t removing = 0;

    while (removing < required && index < 255)
    {
        removing += self->histo[index];
        index++;
    }
    long long boundary = (index < 16) ? index : (8 + (index & 7)) << ((index >> 3) - 1);
    return boundary - 1;
}


static inline HT_VARIANT(_cell_t) * HT_VARIANT(_allocate_cell)(HT_TYPE * self, char * data, uint32_t dataLength)
{
    HT_VARIANT(_cell_t) * cell = HT_VARIANT(_find_cell)(self, data, dataLength, 1);

    if (!cell->key)
    {
        if (self->size >= (self->buckets >> 2) * 3)
        {
            HT_VARIANT(_prune_int)(self, HT_VARIANT(_prune_size)(self));
            // After pruning, we have to look for the ideal spot again, since a better slot might have opened
            cell = HT_VARIANT(_find_cell)(self, data, dataLength, 0);
        }

        self->size += 1;
        self->str_allocated += dataLength + 1;
        char * key = malloc(dataLength + 1);
        memcpy(key, data, dataLength + 1);
        cell->key = key;
        cell->count = 0;
        self->histo[0] += 1;
    }
    return cell;
}

static void HT_VARIANT(_prune_int)(HT_TYPE *self, long long boundary)
{
    HT_VARIANT(_cell_t) * table = self->table;
    uint32_t * histo = self->histo;
    uint32_t size = 0;
    uint32_t start = 0;
    uint32_t mask = self->hash_mask;

    if (boundary > self->max_prune)
        self->max_prune = boundary;

    uint32_t i;
    for (i = 0; i < 256; i++)
        histo[i] = 0;

    // find first empty row and iterate from there
    // if we start from an empty row, hashes from all successive allocated buckets
    // are guaranteed to point "after" this row which ensures the invariant that
    // all processed buckets' hashes point to buckets which have already been processed
    while (table[start].key)
        start++;

    i = start;
    uint32_t last_free = start;
    do
    {
        i = (i + 1) & mask;
        char * current_key = table[i].key;
        if (current_key)
        {
            uint32_t data_length = strlen(current_key);
            long long current_count = table[i].count;

            if (current_count > boundary)
            {
                uint32_t replace = HT_VARIANT(_bucket)(self, current_key, data_length, 0);

                if (((i - last_free) & mask) > ((i - replace) & mask))
                    replace = i;

                while (replace != i && table[replace].key)
                    replace = (replace + 1) & mask;

                if (replace != i)
                {
                    table[replace].key = current_key;
                    table[replace].count = current_count;
                    table[i].key = NULL;
                    table[i].count = 0;
                    last_free = i;
                }

                histo[HT_VARIANT(_histo_addr)(current_count)] += 1;
                size++;
            }
            else
            {
                self->str_allocated -= data_length + 1;
                free(current_key);
                table[i].key = NULL;
                table[i].count = 0;
                last_free = i;
            }
        }
        else
        {
            last_free = i;
        }
    }
    while (i != start);

    self->size = size;

}

static inline int HT_VARIANT(_checkString)(char * value, uint32_t length)
{
    if (!value || strlen(value) < length)
    {
        char * msg = "String contains null bytes!";
        PyErr_SetString(PyExc_ValueError, msg);
        return 0;
    }
    return 1;
}

/* Adds a string to the counter. */
static PyObject *
HT_VARIANT(_increment_obj)(HT_TYPE *self, char *data, uint32_t dataLength, long long increment)
{
    if (increment < 0)
    {
        char * msg = "Increment must be positive!";
        PyErr_SetString(PyExc_ValueError, msg);
        return NULL;
    }
    else if (increment == 0)
    {
        Py_INCREF(Py_None);
        return Py_None;
    }

    HT_VARIANT(_cell_t) * cell = HT_VARIANT(_allocate_cell)(self, data, dataLength);

    if (cell)
    {
        if (cell->count > LLONG_MAX - increment)
        {
            char * msg = "Counter overflow!";
            PyErr_SetString(PyExc_OverflowError, msg);
            return NULL;
        }

        self->total += increment;
        self->histo[HT_VARIANT(_histo_addr)(cell->count)] -= 1;
        cell->count += increment;
        self->histo[HT_VARIANT(_histo_addr)(cell->count)] += 1;
        Py_INCREF(Py_None);
        return Py_None;
    }

    return NULL;
}

/* Adds a string to the counter. */
static PyObject *
HT_VARIANT(_increment)(HT_TYPE *self, PyObject *args)
{
    const char *data;
    const uint32_t dataLength;

    long long increment = 1;

    if (!PyArg_ParseTuple(args, "s#|L", &data, &dataLength, &increment))
        return NULL;
    if (!HT_VARIANT(_checkString)(data, dataLength))
        return NULL;

    return HT_VARIANT(_increment_obj)(self, data, dataLength, increment);
}

/* Sets count for a single string. */
static int
HT_VARIANT(_setitem)(HT_TYPE *self, PyObject *pKey, PyObject *pValue)
{
    const char *data;
    const uint32_t dataLength;
    long long value;

    if (!PyArg_Parse(pKey, "s#", &data, &dataLength))
        return -1;
    if (!HT_VARIANT(_checkString)(data, dataLength))
        return -1;

    if (pValue) // set value
    {
        if (!PyArg_Parse(pValue, "L", &value))
            return -1;
        if (value < 0)
        {
            char * msg = "The counter only supports positive values!";
            PyErr_SetString(PyExc_ValueError, msg);
            return -1;
        }

        // don't bother allocating a new cell when setting 0
        HT_VARIANT(_cell_t) * cell = value
                ? HT_VARIANT(_allocate_cell)(self, data, dataLength)
                : HT_VARIANT(_find_cell)(self, data, dataLength, 0);

        if (cell)
        {
            self->histo[HT_VARIANT(_histo_addr)(cell->count)] -= 1;
            self->histo[HT_VARIANT(_histo_addr)(value)] += 1;
            self->total += value - cell->count;
            cell->count = value;
            return 0;
        }
    }
    else // delete value
    {
        HT_VARIANT(_cell_t) * cell = HT_VARIANT(_find_cell)(self, data, dataLength, 0);
        if (cell)
        {
            self->histo[HT_VARIANT(_histo_addr)(cell->count)] -= 1;
            self->histo[0] += 1;
            self->total -= cell->count;
            cell->count = 0;
        }
        return 0;
    }

    return -1;
}

/* Retrieves count for a single string. */
static PyObject *
HT_VARIANT(_getitem)(HT_TYPE *self, PyObject *key)
{
    const char *data;
    const uint32_t dataLength;

    if (!PyArg_Parse(key, "s#", &data, &dataLength))
        return NULL;
    if (!HT_VARIANT(_checkString)(data, dataLength))
        return NULL;

    HT_VARIANT(_cell_t) * cell = HT_VARIANT(_find_cell)(self, data, dataLength, 0);
    long long value = cell ? cell->count : 0;

    return Py_BuildValue("L", value);
}

static PyObject *
HT_VARIANT(_total)(HT_TYPE *self)
{
    return Py_BuildValue("L", self->total);
}

static Py_ssize_t
HT_VARIANT(_size)(HT_TYPE *self)
{
    return self->size - self->histo[0];
}

static PyObject *
HT_VARIANT(_cardinality)(HT_TYPE *self)
{
    if (!self->max_prune)
        return Py_BuildValue("n", HT_VARIANT(_size)(self));

    double cardinality = HyperLogLog_cardinality(&self->hll);
    return Py_BuildValue("L", (long long) cardinality);
}

static PyObject *
HT_VARIANT(_quality)(HT_TYPE *self)
{
    uint32_t limit = (self->buckets >> 2) * 3;

    double size = (self->max_prune)
            ? HyperLogLog_cardinality(&self->hll)
            : (double) HT_VARIANT(_size)(self);

    double quality = size / (double) limit;

    return Py_BuildValue("d", quality);
}

/* Serialization function for pickling. */
static PyObject *
HT_VARIANT(_reduce)(HT_TYPE *self)
{
    PyObject *args = Py_BuildValue("(I)", self->buckets);
    HT_VARIANT(_cell_t) * table = self->table;

    PyObject * hashtable_row = PyByteArray_FromStringAndSize(table, self->buckets * sizeof(HT_VARIANT(_cell_t)));
    PyObject * histo_row = PyByteArray_FromStringAndSize(self->histo, 256 * sizeof(uint32_t));

    PyByteArrayObject * strings_row = (PyByteArrayObject *) PyByteArray_FromStringAndSize(NULL, self->str_allocated);

    char * result_index = strings_row->ob_bytes;
    uint32_t i;
    for (i = 0; i < self->buckets; i++)
    {
        char* key = table[i].key;
        if (key)
        {
            int length = strlen(key) + 1;
            memcpy(result_index, key, length);
            result_index += length;
        }
    }

    PyObject * hll_row = PyByteArray_FromStringAndSize(self->hll.registers, self->hll.size);

    PyObject *state = Py_BuildValue("(LLILOOOO)",
        self->total, self->str_allocated, self->size, self->max_prune, hashtable_row, strings_row, histo_row, hll_row);
    return Py_BuildValue("(OOO)", Py_TYPE(self), args, state);
}

/* De-serialization function for pickling. */
static PyObject *
HT_VARIANT(_set_state)(HT_TYPE * self, PyObject * args)
{
    PyObject * hashtable_row_o;
    PyObject * strings_row_o;
    PyObject * histo_row_o;
    PyObject * hll_row_o;

    if (!PyArg_ParseTuple(args, "(LLILOOOO)",
            &self->total, &self->str_allocated, &self->size, &self->max_prune,
            &hashtable_row_o, &strings_row_o, &histo_row_o, &hll_row_o))
        return NULL;

    char * hashtable_row = PyByteArray_AsString(hashtable_row_o);
    if (!hashtable_row)
        return NULL;

    HT_VARIANT(_cell_t) * table = self->table;

    memcpy(table, hashtable_row, self->buckets * sizeof(HT_VARIANT(_cell_t)));

    char * string_row = PyByteArray_AsString(strings_row_o);
    uint64_t total_length = PyByteArray_Size(strings_row_o);
    char * current_word = string_row;
    uint32_t i;
    for (i = 0; i < self->buckets; i++)
    {
        if (table[i].key) // the imported key is garbage, we replace it with a real pointer
        {
            if (current_word >= string_row + total_length)
            {
                return NULL; // overflow!
            }

            int current_length = strlen(current_word) + 1;
            char * current_target = malloc(current_length);
            table[i].key = current_target;
            memcpy(current_target, current_word, current_length);
            current_word += current_length;
        }
    }

    uint32_t * histo_row = PyByteArray_AsString(histo_row_o);
    if (!histo_row)
        return NULL;
    memcpy(self->histo, histo_row, 256 * sizeof(uint32_t));

    hll_cell_t * hll_row = PyByteArray_AsString(hll_row_o);
    if (!hll_row)
        return NULL;
    memcpy(self->hll.registers, hll_row, self->hll.size);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
HT_VARIANT(_print_histo)(HT_TYPE * self)
{
    long long i;
    for (i = 0; i < 255; i++)
    {
        long long min = (i < 16) ? i : (8 + (i & 7)) << ((i >> 3) - 1);
        long long max = (i < 16) ? i : ((8 + ((i + 1) & 7)) << (((i + 1) >> 3) - 1)) - 1;
        printf("%lld - %lld: %lld\n", min, max, self->histo[i]);
    }

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
HT_VARIANT(_print_alloc)(HT_TYPE * self)
{
    long long mem = sizeof(HT_VARIANT(_cell_t)) * self->buckets;
    mem += self->str_allocated;
    mem += sizeof(uint32_t) * 256;

    return Py_BuildValue("L", mem);
}

static PyObject *
HT_VARIANT(_prune)(HT_TYPE * self, PyObject *args)
{
    long long boundary;

    if (!PyArg_ParseTuple(args, "L", &boundary))
        return NULL;

    HT_VARIANT(_prune_int)(self, boundary);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
HT_VARIANT(_buckets)(HT_TYPE * self)
{
    return Py_BuildValue("I", self->buckets);
}

static PyObject *
HT_VARIANT(_update)(HT_TYPE * self, PyObject *args)
{
    PyObject * arg;
    PyObject * should_dealloc = NULL;

    if (!PyArg_ParseTuple(args, "O", &arg))
        return NULL;

    if (PyDict_Check(arg) || PyObject_TypeCheck(arg, ((PyObject *) self)->ob_type))
    {
        arg = PyMapping_Items(arg);
        should_dealloc = arg;
    }

    PyObject * iterator = PyObject_GetIter(arg);
    if (iterator)
    {
        PyObject *item;
        char *data;
        uint32_t dataLength;
        while (item = PyIter_Next(iterator))
        {
            if (PyTuple_Check(item))
            {
                if (!HT_VARIANT(_increment)(self, item))
                {
                    Py_DECREF(item);
                    Py_DECREF(iterator);
                    if (should_dealloc)
                        Py_DECREF(should_dealloc);
                    return NULL;
                }
            }
            else
            {
                data = NULL;
                #if PY_MAJOR_VERSION >= 3
                if (PyUnicode_Check(item)) {
                    data = PyUnicode_AsUTF8AndSize(item, &dataLength);
                }
                #else
                if (PyString_Check(item)) {
                    if (PyString_AsStringAndSize(item, &data, &dataLength))
                        data = NULL;
                }
                #endif
                else { /* read-only bytes-like object */
                    PyBufferProcs *pb = Py_TYPE(item)->tp_as_buffer;
                    Py_buffer view;
                    char release_failure = -1;

                    if ((pb == NULL || pb->bf_releasebuffer == NULL)
                           && ((release_failure = PyObject_GetBuffer(item, &view, PyBUF_SIMPLE)) == 0)
                           && PyBuffer_IsContiguous(&view, 'C'))
                    {
                        data = view.buf;
                        dataLength = view.len;
                    }
                    if (!release_failure)
                        PyBuffer_Release(&view);
                }
                if (!data
                    || !HT_VARIANT(_checkString)(data, dataLength)
                    || !HT_VARIANT(_increment_obj)(self, data, dataLength, 1))
                {
                    Py_DECREF(item);
                    Py_DECREF(iterator);
                    if (should_dealloc)
                        Py_DECREF(should_dealloc);
                    return NULL;
                }
                Py_DECREF(item);
            }
        }
        Py_DECREF(iterator);
    }

    if (should_dealloc)
        Py_DECREF(should_dealloc);

    if (!iterator)
    {
        char * msg = "Unsupported argument type!";
        PyErr_SetString(PyExc_TypeError, msg);
        return NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* HT_VARIANT(_ITER_iter)(PyObject *self)
{
  Py_INCREF(self);
  return self;
}

#define ITER_RESULT_KEYS 1
#define ITER_RESULT_VALUES 2
#define ITER_RESULT_KV_PAIRS 3


PyObject* HT_VARIANT(_ITER_iternext)(HT_VARIANT(_ITER_TYPE) *self)
{
    HT_VARIANT(_cell_t) * table = self->hashtable->table;
    uint32_t buckets = self->hashtable->buckets;
    uint32_t i = self->i;
    while (i < buckets && table[i].count == 0)
        i++;

    if (i < buckets)
    {
        PyObject * result;
        if (self->result_type == ITER_RESULT_KEYS)
            result = Py_BuildValue("s", table[i].key);
        else if (self->result_type == ITER_RESULT_KV_PAIRS)
            result = Py_BuildValue("(sl)", table[i].key, table[i].count);
        else
        {
            char * msg = "Invalid iteration type!";
            PyErr_SetString(PyExc_SystemError, msg);
            return NULL;
        }
        self->i = i + 1;
        return result;
    }

    /* Raising of standard StopIteration exception with empty value. */
    self->i = buckets;
    PyErr_SetNone(PyExc_StopIteration);
    return NULL;
}

/* Destructor invoked by python. */
static void
HT_VARIANT(_ITER_dealloc)(HT_VARIANT(_ITER_TYPE) * self)
{
    Py_DECREF(self->hashtable);

    // finally, destroy itself
    #if PY_MAJOR_VERSION >= 3
    Py_TYPE(self)->tp_free((PyObject*) self);
    #else
    self->ob_type->tp_free((PyObject*) self);
    #endif
}

static PyTypeObject HT_VARIANT(_ITER_TYPE_Type) = {
    #if PY_MAJOR_VERSION >= 3
    PyVarObject_HEAD_INIT(NULL, 0)
    #else
    PyObject_HEAD_INIT(NULL)
    0,                               /* ob_size */
    #endif
    "htc." HT_TYPE_STRING "_iter",             /*tp_name*/
    sizeof(HT_VARIANT(_ITER_TYPE)), /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)HT_VARIANT(_ITER_dealloc), /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    #if PY_MAJOR_VERSION >= 3
    Py_TPFLAGS_DEFAULT,
    #else
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_ITER,
      /* tp_flags: Py_TPFLAGS_HAVE_ITER tells python to
         use tp_iter and tp_iternext fields. */
    #endif
    "Internal HTC iterator object.",           /* tp_doc */
    0,  /* tp_traverse */
    0,  /* tp_clear */
    0,  /* tp_richcompare */
    0,  /* tp_weaklistoffset */
    HT_VARIANT(_ITER_iter),  /* tp_iter: __iter__() method */
    HT_VARIANT(_ITER_iternext)  /* tp_iternext: next() method */
};

static inline HT_VARIANT(_ITER_TYPE) *
HT_VARIANT(_make_iterator)(HT_TYPE *self, char result_type)
{
    HT_VARIANT(_ITER_TYPE) * iterator = PyObject_New(HT_VARIANT(_ITER_TYPE), &HT_VARIANT(_ITER_TYPE_Type));
    if (!iterator)
        return NULL;

    if (!PyObject_Init((PyObject *)iterator, &HT_VARIANT(_ITER_TYPE_Type))) {
        Py_DECREF(iterator);
        return NULL;
    }

    iterator->i = 0;
    iterator->hashtable = self;
    iterator->result_type = result_type;
    Py_INCREF(self);
    return iterator;
}

static HT_VARIANT(_ITER_TYPE) *
HT_VARIANT(_HT_iter_KV)(HT_TYPE *self)
{
    return HT_VARIANT(_make_iterator)(self, ITER_RESULT_KV_PAIRS);
}

static HT_VARIANT(_ITER_TYPE) *
HT_VARIANT(_HT_iter_K)(HT_TYPE *self)
{
    return HT_VARIANT(_make_iterator)(self, ITER_RESULT_KEYS);
}


static PyMethodDef HT_VARIANT(_methods)[] = {
    {"increment", (PyCFunction)HT_VARIANT(_increment), METH_VARARGS,
     "Adds a string to the counter."
    },
    {"cardinality", (PyCFunction)HT_VARIANT(_cardinality), METH_NOARGS,
     "Returns an estimate for the number of distinct items inserted into the counter. Does not work correctly when values are deleted!"
    },
    {"total", (PyCFunction)HT_VARIANT(_total), METH_NOARGS,
     "Returns sum of all counts in the counter."
    },
    {"items", (PyCFunction)HT_VARIANT(_HT_iter_KV), METH_NOARGS,
     "Iterates over all key-value pairs."
    },
    {"update", (PyCFunction)HT_VARIANT(_update), METH_VARARGS,
     "Adds all pairs from another counter, or adds all items from an iterable."
    },
    {"quality", (PyCFunction)HT_VARIANT(_quality), METH_NOARGS,
     "Returns the current estimated overflow rating of the structure, calculated as (cardinality / available buckets)."
    },
    {"histo", (PyCFunction)HT_VARIANT(_print_histo), METH_NOARGS,
     "Iterates over all key-value pairs."
    },
    {"prune", (PyCFunction)HT_VARIANT(_prune), METH_VARARGS,
     "Removes all entries with count X or less."
    },
    {"buckets", (PyCFunction)HT_VARIANT(_buckets), METH_NOARGS,
     "Returns the total number of buckets in the hashtable."
    },
    {"mem", (PyCFunction)HT_VARIANT(_print_alloc), METH_NOARGS,
     "Returns allocated memory on the heap in bytes."
    },
    {"__reduce__", (PyCFunction)HT_VARIANT(_reduce), METH_NOARGS,
     "Serialization function for pickling."
    },
    {"__setstate__", (PyCFunction)HT_VARIANT(_set_state), METH_VARARGS,
    "De-serialization function for pickling."
    },
    {NULL}  /* Sentinel */
};

static PyMappingMethods HT_VARIANT(_map_methods) = {
    (lenfunc) HT_VARIANT(_size),
    (binaryfunc) HT_VARIANT(_getitem),
    (objobjargproc) HT_VARIANT(_setitem),
};

static PyTypeObject HT_VARIANT(Type) = {
    #if PY_MAJOR_VERSION >= 3
    PyVarObject_HEAD_INIT(NULL, 0)
    #else
    PyObject_HEAD_INIT(NULL)
    0,                               /* ob_size */
    #endif
    "htc." HT_TYPE_STRING,      /* tp_name */
    sizeof(HT_TYPE),        /* tp_basicsize */
    0,                               /* tp_itemsize */
    (destructor)HT_VARIANT(_dealloc), /* tp_dealloc */
    0,                               /* tp_print */
    0,                               /* tp_getattr */
    0,                               /* tp_setattr */
    0,                               /* tp_compare */
    0,                               /* tp_repr */
    0,                               /* tp_as_number */
    0,                               /* tp_as_sequence */
    &HT_VARIANT(_map_methods),        /* tp_as_mapping */
    0,                               /* tp_hash */
    0,                               /* tp_call */
    0,                               /* tp_str */
    0,                               /* tp_getattro */
    0,                               /* tp_setattro */
    0,                               /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,         /* tp_flags */
    HT_TYPE_STRING " object",            /* tp_doc */
    0,		                     /* tp_traverse */
    0,		                     /* tp_clear */
    0,		                     /* tp_richcompare */
    0,		                     /* tp_weaklistoffset */
    HT_VARIANT(_HT_iter_K),       /* tp_iter */
    0,		                     /* tp_iternext */
    HT_VARIANT(_methods),             /* tp_methods */
    HT_VARIANT(_members),             /* tp_members */
    0,                               /* tp_getset */
    0,                               /* tp_base */
    0,                               /* tp_dict */
    0,                               /* tp_descr_get */
    0,                               /* tp_descr_set */
    0,                               /* tp_dictoffset */
    (initproc)HT_VARIANT(_init),      /* tp_init */
    0,                               /* tp_alloc */
    HT_VARIANT(_new),                 /* tp_new */
};
