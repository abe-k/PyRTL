#include <Python.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32 // includes 32- and 64-bit Windows
#include <Windows.h>
#define get_shared_func GetProcAddress
#else
#include <dlfcn.h>
#define get_shared_func dlsym
#endif

// TODO check windows support

#define require(x) do {if (!(x)) {\
	if (PyErr_Occurred() == NULL) {\
		PyErr_SetNone(PyExc_RuntimeError);\
	}\
	goto cleanup;\
}} while (0)

static PyObject* sim_pyrun(PyObject* self, PyObject* args) {
	uint64_t steps;
	uint32_t ibufsz, obufsz;
	PyObject* data_in; // list of entries for each input:
		// pos in buffer, bitwidth, list of values,
	PyObject* data_out; // list of entries for each traced item:
		// is output (bool), pos in buffer, limb count, list to extend
	uint64_t dll_handle;
	
	if (!PyArg_ParseTuple(args, "KIIOOK", &steps, &ibufsz, &obufsz,
			&data_in, &data_out, &dll_handle)) {
		return NULL;
	}

	uint64_t* ibuf = PyMem_New(uint64_t, ibufsz*steps);
	uint64_t* obuf = PyMem_New(uint64_t, obufsz*steps);

	if (ibuf == NULL || obuf == NULL) {
		if (ibuf != NULL) {
			PyMem_Free(ibuf);
		}
		return PyErr_NoMemory();
	}

	PyObject* tmp = NULL;
	PyObject* tmp2 = NULL;
	PyObject* din_item = NULL;
	PyObject* dout_item = NULL;
	PyObject* val = NULL;
	PyObject* maxsize = NULL;
	PyObject* item_values = NULL;
	PyObject* trace = NULL;
	PyObject* built = NULL;
	PyObject* zero = PyLong_FromLong(0);
	PyObject* one = PyLong_FromLong(1);
	PyObject* sixtyfour = PyLong_FromLong(64);

	require(PySequence_Check(data_in));
	require(PySequence_Check(data_out));

	// Pack inputs into ibuf
	uint32_t din_len = (uint32_t) PySequence_Size(data_in);
	for (uint32_t i = 0; i < din_len; i++) {
		din_item = PySequence_ITEM(data_in, i);
		require(PySequence_Check(din_item));
		require(PySequence_Length(din_item) == 3);
		tmp = PySequence_ITEM(din_item, 0);
		tmp2 = PyNumber_Long(tmp);
		Py_CLEAR(tmp);
		require(tmp2 != NULL);
		uint64_t start = PyLong_AsUnsignedLong(tmp2);
		Py_CLEAR(tmp2);
		tmp = PySequence_ITEM(din_item, 1);
		tmp2 = PyNumber_Long(tmp);
		Py_CLEAR(tmp);
		require(tmp2 != NULL);
		uint32_t bw = PyLong_AsUnsignedLong(tmp2);
		maxsize = PyNumber_Lshift(one, tmp2);
		Py_CLEAR(tmp2);
		item_values = PySequence_ITEM(din_item, 2);
		require(PySequence_Check(item_values));
		Py_CLEAR(din_item);
		uint32_t limbs = (bw+63)/64;
		
		for (uint64_t j = 0; j < steps; j++) {
			tmp = PySequence_ITEM(item_values, j);
			val = PyNumber_Long(tmp);
			Py_CLEAR(tmp);
			require(val != NULL);
			if (PyObject_RichCompareBool(val, maxsize, Py_GE) ||
				PyObject_RichCompareBool(val, zero, Py_LT)) {
				PyErr_Format(PyExc_RuntimeError, "%u", i);
				goto cleanup;
			}
			for (uint64_t k = start; k < start+limbs; k++) {
				require(k < steps*ibufsz);
				ibuf[k] = PyLong_AsUnsignedLongLongMask(val);
				tmp = val;
				val = PyNumber_Rshift(tmp, sixtyfour);
				Py_CLEAR(tmp);
			}
			Py_CLEAR(val);
			start += ibufsz;
		}
		Py_CLEAR(maxsize);
		Py_CLEAR(item_values);
	}

	void (*sim_run_all)(uint64_t, uint64_t*, uint64_t*);
	sim_run_all = get_shared_func((void*) dll_handle, "sim_run_all");

	(*sim_run_all)(steps, ibuf, obuf);

	// Unpack outputs from obuf (and ibuf)
	uint32_t dout_len = (uint32_t) PySequence_Size(data_out);
	for (uint32_t i = 0; i < dout_len; i++) {
		dout_item = PySequence_ITEM(data_out, i);
		require(PySequence_Check(dout_item));
		require(PySequence_Length(dout_item) == 4);
		tmp = PySequence_ITEM(dout_item, 0);
		uint64_t* cbuf;
		uint32_t cbufsz;
		if (PyObject_IsTrue(tmp)) {
			cbuf = obuf;
			cbufsz = obufsz;
		} else {
			cbuf = ibuf;
			cbufsz = ibufsz;
		}
		Py_CLEAR(tmp);
		tmp = PySequence_ITEM(dout_item, 1);
		tmp2 = PyNumber_Long(tmp);
		Py_CLEAR(tmp);
		require(tmp2 != NULL);
		uint64_t start = PyLong_AsUnsignedLong(tmp2);
		Py_CLEAR(tmp2);
		tmp = PySequence_ITEM(dout_item, 2);
		tmp2 = PyNumber_Long(tmp);
		Py_CLEAR(tmp);
		require(tmp2 != NULL);
		uint32_t limbs = PyLong_AsUnsignedLong(tmp2);
		Py_CLEAR(tmp2);
		trace = PySequence_ITEM(dout_item, 3);
		require(PySequence_Check(trace));
		Py_CLEAR(dout_item);

		built = PyList_New(steps);
		for (uint64_t j = 0; j < steps; j++) {
			val = PyLong_FromLong(0);
			for (uint64_t k = start+limbs-1; k >= start && k != (uint64_t) -1; k--) {
				tmp = PyNumber_Lshift(val, sixtyfour);
				require(k < steps*cbufsz);
				tmp2 = PyLong_FromUnsignedLongLong(cbuf[k]);
				Py_CLEAR(val);
				val = PyNumber_Or(tmp, tmp2);
				Py_CLEAR(tmp);
				Py_CLEAR(tmp2);
			}
			PyList_SET_ITEM(built, j, val); // steals reference
			val = NULL;
			start += cbufsz;
		}
		tmp = PySequence_InPlaceConcat(trace, built);
		Py_CLEAR(tmp);
		Py_CLEAR(built);
		Py_CLEAR(trace);
	}

	cleanup:
	PyMem_Free(ibuf);
	PyMem_Free(obuf);
	Py_CLEAR(tmp);
	Py_CLEAR(tmp2);
	Py_CLEAR(din_item);
	Py_CLEAR(dout_item);
	Py_CLEAR(val);
	Py_CLEAR(maxsize);
	Py_CLEAR(item_values);
	Py_CLEAR(trace);
	Py_CLEAR(built);
	Py_CLEAR(zero);
	Py_CLEAR(one);
	Py_CLEAR(sixtyfour);

	if (PyErr_Occurred() != NULL) {
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyMethodDef mod_methods[] = {
	{"sim_pyrun", sim_pyrun, METH_VARARGS, ""},
	{NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION < 3

void init_pyrtlsimrunner(void) {
	Py_InitModule3("_pyrtlsimrunner", mod_methods, "");
}

#else

static struct PyModuleDef mod_def = {
	PyModuleDef_HEAD_INIT,
	"_pyrtlsimrunner",
	"",
	-1,
	mod_methods,
	NULL,
	NULL,
	NULL,
	NULL,
};

PyObject* PyInit__pyrtlsimrunner(void) {
	return PyModule_Create(&mod_def);
};

#endif
