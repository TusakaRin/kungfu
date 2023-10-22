PROJECT(kungfu-deps)
include(CMakePrintHelpers)
cmake_print_variables(KF_EXTENSIONS_BUILD_DIR)
cmake_print_variables(KFC_INSTALL_RPATH)

############################################################

ADD_SUBDIRECTORY(pybind11-2.2.4)

ADD_SUBDIRECTORY(nanomsg-1.1.5)
ADD_SUBDIRECTORY(fmt-5.3.0)
ADD_SUBDIRECTORY(SQLiteCpp-2.3.0)

ADD_SUBDIRECTORY(googletest-1.9.0)