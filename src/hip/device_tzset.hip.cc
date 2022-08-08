#include "hip/hip_runtime.h"
// Copyright (c) 2017-2022, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/Exception.hh"
#include "slate/internal/device.hh"

#include "device_util.hip.hh"

namespace slate {
namespace device {

//------------------------------------------------------------------------------
/// Device function implementing element-wise tile set.
/// Each thread block deals with one tile. gridDim.x == batch_count.
/// Each thread deals with one row.
/// Called by tzset_kernel and tzset_batch_kernel.
///
/// @copydoc tzset
///
template <typename scalar_t>
__device__ void tzset_func(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    scalar_t offdiag_value, scalar_t diag_value,
    scalar_t* A, int64_t lda )
{
    // thread per row, if more rows than threads, loop by blockDim.x
    for (int i = threadIdx.x; i < m; i += blockDim.x) {
        scalar_t* rowA = &A[ i ];

        if (uplo == lapack::Uplo::Lower) {
            for (int64_t j = 0; j <= i && j < n; ++j) { // lower
                rowA[ j*lda ] = i == j ? diag_value : offdiag_value;
            }
        }
        else {
            for (int64_t j = n-1; j >= i; --j) { // upper
                rowA[ j*lda ] = i == j ? diag_value : offdiag_value;
            }
        }
    }
}

//------------------------------------------------------------------------------
/// Kernel implementing element-wise tile set.
/// @copydoc tzset
template <typename scalar_t>
__global__ void tzset_kernel(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    scalar_t offdiag_value, scalar_t diag_value,
    scalar_t* A, int64_t lda )
{
    tzset_func( uplo, m, n, offdiag_value, diag_value, A, lda );
}

//------------------------------------------------------------------------------
/// Kernel implementing element-wise tile set.
/// @copydoc tzset_batch
template <typename scalar_t>
__global__ void tzset_batch_kernel(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    scalar_t offdiag_value, scalar_t diag_value,
    scalar_t** Aarray, int64_t lda )
{
    tzset_func( uplo, m, n, offdiag_value, diag_value,
                Aarray[ blockIdx.x ], lda );
}

//------------------------------------------------------------------------------
/// Element-wise trapezoidal tile set.
/// Sets upper or lower part of Aarray[k] to
/// diag_value on the diagonal and offdiag_value on the off-diagonals.
///
/// @param[in] uplo
///     Whether each Aarray[k] is upper or lower trapezoidal.
///
/// @param[in] m
///     Number of rows of A. m >= 0.
///
/// @param[in] n
///     Number of columns of A. n >= 0.
///
/// @param[in] offdiag_value
///     Constant to set offdiagonal entries to.
///
/// @param[in] diag_value
///     Constant to set diagonal entries to.
///
/// @param[out] A
///     An m-by-n matrix stored in an lda-by-n array in GPU memory.
///
/// @param[in] lda
///     Leading dimension of A. lda >= m.
///
/// @param[in] queue
///     BLAS++ queue to execute in.
///
template <typename scalar_t>
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    scalar_t offdiag_value, scalar_t diag_value,
    scalar_t* A, int64_t lda,
    blas::Queue& queue )
{
    hipSetDevice( queue.device() );

    // Max threads/block=1024 for current CUDA compute capability (<= 7.5)
    int nthreads = std::min( int64_t( 1024 ), m );

    hipLaunchKernelGGL(tzset_kernel, dim3(1), dim3(nthreads), 0, queue.stream(),
        uplo, m, n,
        offdiag_value, diag_value, A, lda );

    hipError_t error = hipGetLastError();
    slate_assert( error == hipSuccess );
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    float offdiag_value, float diag_value,
    float* A, int64_t lda,
    blas::Queue& queue );

template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    double offdiag_value, double diag_value,
    double* A, int64_t lda,
    blas::Queue& queue );

template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    hipFloatComplex offdiag_value, hipFloatComplex diag_value,
    hipFloatComplex* A, int64_t lda,
    blas::Queue& queue );

template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    hipDoubleComplex offdiag_value, hipDoubleComplex diag_value,
    hipDoubleComplex* A, int64_t lda,
    blas::Queue& queue );

//==============================================================================
namespace batch {

//------------------------------------------------------------------------------
/// Batched routine for element-wise trapezoidal tile set.
/// Sets upper or lower part of Aarray[k] to
/// diag_value on the diagonal and offdiag_value on the off-diagonals.
///
/// @param[in] m
///     Number of rows of each tile. m >= 0.
///
/// @param[in] n
///     Number of columns of each tile. n >= 0.
///
/// @param[in] offdiag_value
///     Constant to set offdiagonal entries to.
///
/// @param[in] diag_value
///     Constant to set diagonal entries to.
///
/// @param[out] Aarray
///     Array in GPU memory of dimension batch_count, containing
///     pointers to tiles, where each Aarray[k] is an m-by-n matrix
///     stored in an lda-by-n array in GPU memory.
///
/// @param[in] lda
///     Leading dimension of each tile in Aarray. lda >= m.
///
/// @param[in] batch_count
///     Size of Aarray. batch_count >= 0.
///
/// @param[in] queue
///     BLAS++ queue to execute in.
///
template <typename scalar_t>
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    scalar_t offdiag_value, scalar_t diag_value,
    scalar_t** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue )
{
    // quick return
    if (batch_count == 0)
        return;

    hipSetDevice( queue.device() );

    // Max threads/block=1024 for current CUDA compute capability (<= 7.5)
    int nthreads = std::min( int64_t( 1024 ), m );

    hipLaunchKernelGGL(tzset_batch_kernel, dim3(batch_count), dim3(nthreads), 0, queue.stream(),
        uplo, m, n,
        offdiag_value, diag_value, Aarray, lda );

    hipError_t error = hipGetLastError();
    slate_assert( error == hipSuccess );
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    float offdiag_value, float diag_value,
    float** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue );

template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    double offdiag_value, double diag_value,
    double** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue );

template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    hipFloatComplex offdiag_value, hipFloatComplex diag_value,
    hipFloatComplex** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue );

template
void tzset(
    lapack::Uplo uplo,
    int64_t m, int64_t n,
    hipDoubleComplex offdiag_value, hipDoubleComplex diag_value,
    hipDoubleComplex** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue );

} // namespace batch
} // namespace device
} // namespace slate
