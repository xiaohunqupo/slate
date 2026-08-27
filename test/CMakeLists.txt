# Can't build testers if ScaLAPACK or TestSweeper are not found.
message( "" )
message( "---------------------------------------- ScaLAPACK" )

# User or Spack can give us SCALAPACK_LIBRARIES;
# otherwise, try to figure it out to match BLAS and MPI.
# todo: more robust FindScaLAPACK package; several exist.
if (NOT DEFINED SCALAPACK_LIBRARIES)
    if ("${blaspp_defines}" MATCHES "HAVE_MKL")
        if ("${blaspp_defines}" MATCHES "BLAS_ILP64")
            set( int_ "ilp64" )
        else()
            set( int_ "lp64" )
        endif()
        if (MPI_CXX_LIBRARY_VERSION_STRING MATCHES "Intel|MPICH")
            # Intel MPI, based on MPICH
            set( SCALAPACK_LIBRARIES "mkl_scalapack_${int_};mkl_blacs_intelmpi_${int_}" )
        else()
            # Open MPI
            set( SCALAPACK_LIBRARIES "mkl_scalapack_${int_};mkl_blacs_openmpi_${int_}" )
        endif()
    else()
        # Try to find ScaLAPACK on the local machine.
        # First try find_package (some distributions provide a
        # FindScaLAPACK.cmake or ScaLAPACKConfig.cmake).
        find_package( ScaLAPACK QUIET )
        if (ScaLAPACK_FOUND)
            if (TARGET ScaLAPACK::ScaLAPACK)
                set( SCALAPACK_LIBRARIES ScaLAPACK::ScaLAPACK )
            elseif (DEFINED ScaLAPACK_LIBRARIES)
                set( SCALAPACK_LIBRARIES "${ScaLAPACK_LIBRARIES}" )
            endif()
        endif()
        if (NOT SCALAPACK_LIBRARIES)
            # Fall back to find_library for scalapack.
            find_library( SCALAPACK_LIBRARIES
                NAMES scalapack scalapack-openmpi scalapack-mpich
                HINTS ${SCALAPACK_ROOT} $ENV{SCALAPACK_ROOT}
                PATH_SUFFIXES lib lib64
            )
            if (SCALAPACK_LIBRARIES)
                # Also look for BLACS library, which may be separate on
                # some systems (e.g., older ScaLAPACK or custom builds).
                get_filename_component( scalapack_dir_ ${SCALAPACK_LIBRARIES} DIRECTORY )
                find_library( BLACS_LIBRARY
                    NAMES blacs blacs-openmpi blacs-mpich blacsCinit
                    HINTS ${scalapack_dir_}
                    NO_DEFAULT_PATH
                )
                if (BLACS_LIBRARY)
                    list( APPEND SCALAPACK_LIBRARIES ${BLACS_LIBRARY} )
                endif()
                unset( scalapack_dir_ )
                unset( BLACS_LIBRARY )
            else()
                set( SCALAPACK_LIBRARIES "none" )
            endif()
        endif()
    endif()
endif()
if (SCALAPACK_LIBRARIES STREQUAL "none")
    message( STATUS "Not using ScaLAPACK" )
else()
    message( STATUS "Using ScaLAPACK library: ${SCALAPACK_LIBRARIES}" )
endif()

# Search for TestSweeper library, if not already included (e.g., in SLATE).
message( "" )
message( "---------------------------------------- TestSweeper" )
message( STATUS "Checking for TestSweeper library" )
if (NOT TARGET testsweeper)
    find_package( testsweeper QUIET )
    if (testsweeper_FOUND)
        message( "   Found TestSweeper library: ${testsweeper_DIR}" )

    elseif (EXISTS "${CMAKE_SOURCE_DIR}/testsweeper/CMakeLists.txt")
        add_subdirectory( "${CMAKE_SOURCE_DIR}/testsweeper"
                          "${CMAKE_BINARY_DIR}/testsweeper" )

    else()
        set( url "https://github.com/icl-utk-edu/testsweeper.git" )
        message( "" )
        message( "---------- TestSweeper" )
        message( STATUS "Fetching TestSweeper from ${url}" )
        include( FetchContent )
        FetchContent_Declare( testsweeper GIT_REPOSITORY "${url}" )
        FetchContent_MakeAvailable( testsweeper )
        message( "---------- TestSweeper done" )
        message( "" )
    endif()
else()
    message( "   TestSweeper already included" )
endif()
message( STATUS "TestSweeper done" )
message( "" )

#-------------------------------------------------------------------------------
# In future, if these *.f are eliminated, may still need dummy.f so CMake links
# with Fortran (e.g., -lgfortran), which may be needed for ScaLAPACK.
# What is a better solution?
file(
    GLOB tester_src
    CONFIGURE_DEPENDS
    *.cc
)
if (NOT SCALAPACK_LIBRARIES STREQUAL "none")
    file(
        GLOB tester_f_src
        CONFIGURE_DEPENDS
        *.f
    )
endif()

set( tester "${slate_}tester" )
add_executable(
    ${tester}
    ${tester_src}
    ${tester_f_src}
)

# Use and export -std=c++17.
# CMake inexplicably allows gnu++17 or "decay" to c++11 or 14; prohibit those.
target_compile_features( ${tester} PUBLIC cxx_std_17 )
set_target_properties(
    ${tester} PROPERTIES
    CXX_STANDARD_REQUIRED true  # prohibit < c++17
    CXX_EXTENSIONS false        # prohibit gnu++17
)

# Avoid "comparison with NaN" warnings from the SYCL Intel-IntelLLVM
# compiler while compiling test/matrix_generator.cc (the compiler uses
# fast floating point mode by default).
if (gpu_backend STREQUAL "sycl" )
   target_compile_options( ${tester} PRIVATE -fp-model=precise )
endif()

target_link_libraries(
    ${tester}
    testsweeper
    slate
    slate_matgen
)

if (NOT SCALAPACK_LIBRARIES STREQUAL "none")
    target_compile_definitions( ${tester} PUBLIC "SLATE_HAVE_SCALAPACK" )
    target_link_libraries( ${tester} ${SCALAPACK_LIBRARIES} )
endif()

# todo: remove internal includes from testers (e.g., test_posv.cc).
target_include_directories(
    ${tester}
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

#-------------------------------------------------------------------------------
# Copy run_tests script to build directory.
add_custom_command(
    TARGET ${tester} POST_BUILD
    COMMAND
        cp ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
           ${CMAKE_CURRENT_SOURCE_DIR}/gpu_bind.sh
           ${CMAKE_CURRENT_SOURCE_DIR}/idle_gpus.py
           ${CMAKE_CURRENT_BINARY_DIR}/
)

if (slate_is_project)
    # Add 'make check' target.
    add_custom_target(
        "check"
        COMMAND
            python3 run_tests.py --quick gesv posv gels heev svd
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()
