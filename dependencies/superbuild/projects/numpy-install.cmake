# Runs after numpy's own install (numpy.cmake).
#
#   cmake -DPYTHON_PROGRAM=<carried interpreter> -DNUMPY_VERSION=<x.y.z> -P numpy-install.cmake
#
# Stops the build unless the carried interpreter, started isolated as Bloom starts it, imports the
# numpy just installed, at the locked version, built without a BLAS or LAPACK library and without
# code for particular instruction sets.
if(NOT PYTHON_PROGRAM OR NOT NUMPY_VERSION)
    message(FATAL_ERROR "numpy-install.cmake needs PYTHON_PROGRAM and NUMPY_VERSION")
endif()
# (Newlines, not semicolons, separate the statements: a semicolon would split the argument.)
execute_process(COMMAND "${PYTHON_PROGRAM}" -I -c
    "import numpy
build = numpy.show_config(mode='dicts')['Build Dependencies']
from numpy._core._multiarray_umath import __cpu_baseline__, __cpu_dispatch__
print(numpy.__version__, build['blas'].get('name', ''), build['lapack'].get('name', ''),
      len(__cpu_baseline__) + len(__cpu_dispatch__))
a = numpy.arange(12.0).reshape(3, 4)
assert float((a @ a.T).trace()) == 506.0
assert abs(float(numpy.linalg.det(numpy.eye(3) * 2.0)) - 8.0) < 1e-12"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The carried interpreter does not import numpy: ${output}${error}")
endif()
# numpy's build record names the library it was built with; "none" is its word for none.
# The last number counts the instruction sets numpy was built for or can choose when it starts.
if(NOT output STREQUAL "${NUMPY_VERSION} none none 0")
    message(FATAL_ERROR "numpy reports '${output}': expected version ${NUMPY_VERSION}, no BLAS "
                        "or LAPACK library, and no code for particular instruction sets")
endif()
message(STATUS "Bloom Python: numpy ${NUMPY_VERSION}, no BLAS or LAPACK library, portable loops")
