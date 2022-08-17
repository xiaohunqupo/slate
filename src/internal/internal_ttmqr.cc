// Copyright (c) 2017-2022, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/Matrix.hh"
#include "slate/types.hh"
#include "internal/Tile_tpmqrt.hh"
#include "internal/internal.hh"
#include "internal/internal_util.hh"

namespace slate {
namespace internal {

//------------------------------------------------------------------------------
/// Distributed multiply matrix by Q from QR triangle-triangle factorization of
/// column of tiles.
/// Dispatches to target implementations.
/// todo: This assumes A and T have already been communicated as needed.
/// However, it necesarily handles communication for C.
/// Tag is used in geqrf to differentiate communication for look-ahead panel
/// from rest of trailing matrix.
/// @ingroup geqrf_internal
///
template <Target target, typename scalar_t>
void ttmqr(Side side, Op op,
           Matrix<scalar_t>&& A,
           Matrix<scalar_t>&& T,
           Matrix<scalar_t>&& C,
           int tag)
{
    ttmqr(internal::TargetType<target>(),
          side, op, A, T, C, tag);
}

//------------------------------------------------------------------------------
/// Distributed multiply matrix by Q from QR triangle-triangle factorization of
/// column of tiles, host implementation.
/// @ingroup geqrf_internal
///
template <typename scalar_t>
void ttmqr(internal::TargetType<Target::HostTask>,
           Side side, Op op,
           Matrix<scalar_t>& A,
           Matrix<scalar_t>& T,
           Matrix<scalar_t>& C,
           int tag)
{
    // Assumes column major
    const Layout layout = Layout::ColMajor;

    int64_t A_mt = A.mt();
    assert(A.nt() == 1);
    if (side == Side::Left)
        assert(A_mt == C.mt());
    else
        assert(A_mt == C.nt());

    // Find ranks in this column of A.
    std::set<int> ranks_set;
    A.getRanks(&ranks_set);

    // Find each rank's first (top-most) row in this column of A,
    // which is the triangular tile resulting from local geqrf panel.
    std::vector< std::pair<int, int64_t> > rank_indices;
    rank_indices.reserve(ranks_set.size());
    for (int r: ranks_set) {
        for (int64_t i = 0; i < A_mt; ++i) {
            if (A.tileRank(i, 0) == r) {
                rank_indices.push_back({r, i});
                break;
            }
        }
    }
    // Sort rank_indices by index.
    std::sort(rank_indices.begin(), rank_indices.end(),
              compareSecond<int, int64_t>);

    int nranks = rank_indices.size();
    int nlevels = int( ceil( log2( nranks ) ) );

    // Apply reduction tree.
    // If Left, NoTrans or Right, Trans, apply descending from root to leaves,
    // i.e., in reverse order of how they were created.
    // If Left, Trans or Right, NoTrans, apply ascending from leaves to root,
    // i.e., in same order as they were created.
    // Example for A.mt == 8.
    // Leaves:
    //     ttqrt( a0, a1 )
    //     ttqrt( a2, a3 )
    //     ttqrt( a4, a5 )
    //     ttqrt( a6, a7 )
    // Next level:
    //     ttqrt( a0, a2 )
    //     ttqrt( a4, a6 )
    // Root:
    //     ttqrt( a0, a4 )
    bool descend = (side == Side::Left) == (op == Op::NoTrans);
    int step;
    if (descend)
        step = pow(2, nlevels - 1);
    else
        step = 1;

    int64_t k_end;
    int64_t i, j, i1, j1, i_dst, j_dst;

    if (side == Side::Left) {
        k_end = C.nt();
    }
    else {
        k_end = C.mt();
    }

    for (int level = 0; level < nlevels; ++level) {
        for (int index = 0; index < nranks; index += step) {
            int64_t rank_ind = rank_indices[ index ].second;
            // if (side == left), scan rows of C for local tiles;
            // if (side == right), scan cols of C for local tiles
            // Three for-loops: 1) send, receive 2) update 3) receive, send
            for (int64_t k = 0; k < k_end; ++k) {
                if (side == Side::Left) {
                    i = rank_ind;
                    j = k;
                }
                else {
                    i = k;
                    j = rank_ind;
                }
                if (C.tileIsLocal(i, j)) {
                    if (index % (2*step) == 0) {
                        if (index + step < nranks) {
                            // Send tile to dst.
                            int64_t k_dst = rank_indices[ index + step ].second;
                            if (side == Side::Left) {
                                i_dst = k_dst;
                                j_dst = k;
                            }
                            else {
                                i_dst = k;
                                j_dst = k_dst;
                            }
                            int dst = C.tileRank(i_dst, j_dst);
                            // GetForWriting because it will be received in the later loop
                            C.tileGetForWriting(i, j, LayoutConvert(layout));
                            C.tileSend(i, j, dst, tag);
                        }
                    }
                    else {
                        // Receive tile from src.
                        int64_t k_src = rank_indices[ index - step ].second;
                        if (side == Side::Left) {
                            i1 = k_src;
                            j1 = k;
                        }
                        else {
                            i1 = k;
                            j1 = k_src;
                        }

                        int     src   = C.tileRank(i1, j1);
                        C.tileRecv(i1, j1, src, layout, tag);
                    }
                }
            }

            #pragma omp taskgroup
            for (int64_t k = 0; k < k_end; ++k) {
                if (side == Side::Left) {
                    i = rank_ind;
                    j = k;
                }
                else {
                    i = k;
                    j = rank_ind;
                }
                if (C.tileIsLocal(i, j)) {
                    if (!(index % (2*step) == 0)) {
                        int64_t k_src = rank_indices[ index - step ].second;
                        if (side == Side::Left) {
                            i1 = k_src;
                            j1 = k;
                        }
                        else {
                            i1 = k;
                            j1 = k_src;
                        }

                        #pragma omp task slate_omp_default_none \
                            shared( A, T, C ) \
                            firstprivate(i, j, layout, rank_ind, side, op, i1, j1)
                        {
                            A.tileGetForReading(rank_ind, 0, LayoutConvert(layout));
                            T.tileGetForReading(rank_ind, 0, LayoutConvert(layout));
                            C.tileGetForWriting(i, j, LayoutConvert(layout));

                            // Apply Q.
                            tpmqrt(side, op, std::min(A.tileMb(rank_ind), A.tileNb(0)),
                                   A(rank_ind, 0), T(rank_ind, 0),
                                   C(i1, j1), C(i, j));

                            // todo: should tileRelease()?
                            A.tileTick(rank_ind, 0);
                            T.tileTick(rank_ind, 0);
                        }
                    }
                }
            }

            for (int64_t k = 0; k < k_end; ++k) {
                if (side == Side::Left) {
                    i = rank_ind;
                    j = k;
                }
                else {
                    i = k;
                    j = rank_ind;
                }
                if (C.tileIsLocal(i, j)) {
                    if (index % (2*step) == 0) {
                        if (index + step < nranks) {
                            // Receive updated tile back.
                            int64_t k_dst = rank_indices[ index + step ].second;
                            if (side == Side::Left) {
                                i_dst = k_dst;
                                j_dst = k;
                            }
                            else {
                                i_dst = k;
                                j_dst = k_dst;
                            }
                            int dst = C.tileRank(i_dst, j_dst);
                            assert( (C.tileState( i, j, HostNum ) & MOSI::Modified) != 0 );
                            C.tileRecv(i, j, dst, layout, tag);
                        }
                    }
                    else {
                        int64_t k_src = rank_indices[ index - step ].second;
                        if (side == Side::Left) {
                            i1 = k_src;
                            j1 = k;
                        }
                        else {
                            i1 = k;
                            j1 = k_src;
                        }
                        int     src   = C.tileRank(i1, j1);
                        // Send updated tile back.
                        C.tileSend(i1, j1, src, tag);
                        C.tileTick(i1, j1);
                    }
                }
            }
        }
        if (descend)
            step /= 2;
        else
            step *= 2;
    }
}

//------------------------------------------------------------------------------
// Explicit instantiations.
// ----------------------------------------
template
void ttmqr<Target::HostTask, float>(
    Side side, Op op,
    Matrix<float>&& A,
    Matrix<float>&& T,
    Matrix<float>&& C,
    int tag);

// ----------------------------------------
template
void ttmqr<Target::HostTask, double>(
    Side side, Op op,
    Matrix<double>&& A,
    Matrix<double>&& T,
    Matrix<double>&& C,
    int tag);

// ----------------------------------------
template
void ttmqr< Target::HostTask, std::complex<float> >(
    Side side, Op op,
    Matrix< std::complex<float> >&& A,
    Matrix< std::complex<float> >&& T,
    Matrix< std::complex<float> >&& C,
    int tag);

// ----------------------------------------
template
void ttmqr< Target::HostTask, std::complex<double> >(
    Side side, Op op,
    Matrix< std::complex<double> >&& A,
    Matrix< std::complex<double> >&& T,
    Matrix< std::complex<double> >&& C,
    int tag);

} // namespace internal
} // namespace slate
