// Copyright (c) 2017-2022, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/slate.hh"
#include "test.hh"

#include "scalapack_wrappers.hh"
#include "scalapack_support_routines.hh"
#include "scalapack_copy.hh"
#include "print_matrix.hh"
#include "grid_utils.hh"
#include "matrix_utils.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#define SLATE_HAVE_SCALAPACK

//------------------------------------------------------------------------------
template <typename matrix_type>
void test_scale_row_col_work( Params& params, bool run )
{
    using scalar_t = typename matrix_type::value_type;
    using real_t = blas::real_type<scalar_t>;
    using blas::real;
    using blas::imag;
    using slate::ceildiv;
    using slate::Equed;

    // Constants
    const scalar_t one = 1.0;

    // get & mark input values
    slate::Uplo uplo;
    if (std::is_same< matrix_type, slate::Matrix<scalar_t> >::value)
        uplo = slate::Uplo::General;
    else
        uplo = params.uplo();
    slate::Op trans = params.trans();
    slate::Equed equed = params.equed();
    slate::Diag diag = slate::Diag::NonUnit;
    int64_t m = params.dim.m();
    int64_t n;
    if (std::is_same< matrix_type, slate::TriangularMatrix<scalar_t> >::value
        || std::is_same< matrix_type, slate::SymmetricMatrix<scalar_t> >::value
        || std::is_same< matrix_type, slate::HermitianMatrix<scalar_t> >::value) {
        n = m;  // square
    }
    else {
        n = params.dim.n();
    }
    int64_t nb = params.nb();
    int64_t p = params.grid.m();
    int64_t q = params.grid.n();
    bool ref_only = params.ref() == 'o';
    bool ref = params.ref() == 'y' || ref_only;
    bool check = params.check() == 'y' && ! ref_only;
    bool trace = params.trace() == 'y';
    slate::Origin origin = params.origin();
    slate::Target target = params.target();
    params.matrix.mark();

    // mark non-standard output values
    params.time();
    params.ref_time();

    if (! run)
        return;

    slate::Options const opts =  {
        {slate::Option::Target, target}
    };

    // MPI variables
    int mpi_rank, myrow, mycol;
    MPI_Comm_rank( MPI_COMM_WORLD, &mpi_rank );
    gridinfo( mpi_rank, p, q, &myrow, &mycol );

    // Matrix A: figure out local size.
    int64_t mlocA = num_local_rows_cols( m, nb, myrow, p );
    int64_t nlocA = num_local_rows_cols( n, nb, mycol, q );
    int64_t lldA  = blas::max( 1, mlocA ); // local leading dimension of A
    std::vector<scalar_t> A_data;

    slate::Matrix<scalar_t> Afull;
    if (origin != slate::Origin::ScaLAPACK) {
        // SLATE allocates CPU or GPU tiles.
        slate::Target origin_target = origin2target( origin );
        Afull = slate::Matrix<scalar_t>( m, n, nb, p, q, MPI_COMM_WORLD);
        Afull.insertLocalTiles( origin_target );
    }
    else {
        // Allocate ScaLAPACK data.
        A_data.resize( lldA*nlocA );
        // Create SLATE matrix from the ScaLAPACK layout.
        Afull = slate::Matrix<scalar_t>::fromScaLAPACK(
                    m, n, &A_data[0], lldA, nb, p, q, MPI_COMM_WORLD );
    }
    slate::generate_matrix( params.matrix, Afull );

    // Cast to desired matrix type.
    matrix_type A = matrix_cast< matrix_type >( Afull, uplo, diag );

    // if reference run is required, copy test data
    std::vector<scalar_t> Aref_data;
    slate::Matrix<scalar_t> Aref_full;
    matrix_type Aref;
    if (check || ref) {
        // For simplicity, always use ScaLAPACK format for ref matrices.
        Aref_data.resize( lldA*nlocA );
        Aref_full = slate::Matrix<scalar_t>::fromScaLAPACK(
                        m,  n, &Aref_data[0], lldA, nb, p, q, MPI_COMM_WORLD);
        Aref = matrix_cast< matrix_type >( Afull, uplo, diag );
        slate::copy( Afull, Aref_full );
    }

    if (trans == slate::Op::Trans)
        A = transpose( A );
    else if (trans == slate::Op::ConjTrans)
        A = conj_transpose( A );

    print_matrix( "Afull", Afull, params );
    print_matrix( "A", A, params );

    // All ranks produce same random R and C scaling factors.
    // todo: test complex R, C. Needs 2nd datatype param?
    std::vector<real_t> R( m ), C( n );
    int64_t idist = 3;  // normal
    int64_t iseed[ 4 ] = { 0, 1, 2, 3 };
    lapack::larnv( idist, iseed, m, &R[ 0 ] );
    lapack::larnv( idist, iseed, n, &C[ 0 ] );

    print_vector( "R", R, params );
    print_vector( "C", C, params );

    if (! ref_only) {
        if (trace) slate::trace::Trace::on();
        else slate::trace::Trace::off();

        //==================================================
        // Run SLATE test.
        // Set A = diag(R) A diag(C)  for equed = Both,
        //       = diag(R) A,         for equed = Row,
        //       =         A diag(C)  for equed = Col.
        //==================================================
        double time = barrier_get_wtime( MPI_COMM_WORLD );

        slate::scale_row_col( equed, R, C, A, opts );

        time = barrier_get_wtime( MPI_COMM_WORLD ) - time;

        if (trace) slate::trace::Trace::finish();

        // compute and save timing/performance
        params.time() = time;

        print_matrix( "Afull_out", Afull, params );
        print_matrix( "A_out", A, params );
    }

    if (check || ref) {
        #ifdef SLATE_HAVE_SCALAPACK
            // comparison with reference routine from ScaLAPACK

            // BLACS/MPI variables
            int ictxt, p_, q_, myrow_, mycol_, info;
            int A_desc[9];
            int mpi_rank_ = 0, nprocs = 1;

            // initialize BLACS and ScaLAPACK
            Cblacs_pinfo( &mpi_rank_, &nprocs );
            slate_assert( mpi_rank_ == mpi_rank );
            slate_assert( p*q <= nprocs );
            Cblacs_get( -1, 0, &ictxt );
            Cblacs_gridinit( &ictxt, "Col", p, q );
            Cblacs_gridinfo( ictxt, &p_, &q_, &myrow_, &mycol_ );
            slate_assert( p == p_ );
            slate_assert( q == q_ );
            slate_assert( myrow == myrow_ );
            slate_assert( mycol == mycol_ );

            scalapack_descinit( A_desc, m, n, nb, nb, 0, 0, ictxt, lldA, &info );
            slate_assert( info == 0 );

            real_t A_max = slate::norm( slate::Norm::Max, A );

            std::vector<real_t> Rlocal( mlocA ), Clocal( nlocA );

            // Copy local part of R.
            int64_t ii = 0, iilocal = 0;
            for (int64_t i = 0; i < A.mt(); ++i) {
                int64_t mb_ = A.tileMb( i );
                if (i % p == myrow) {
                    std::copy( &R[ ii ], &R[ ii + mb_ ], &Rlocal[ iilocal ] );
                    iilocal += mb_;
                }
                ii += mb_;
            }
            assert( iilocal == int64_t( Rlocal.size() ) );

            // Copy local part of R.
            int64_t jj = 0, jjlocal = 0;
            for (int64_t j = 0; j < A.nt(); ++j) {
                int64_t nb_ = A.tileNb( j );
                if (j % q == mycol) {
                    std::copy( &C[ jj ], &C[ jj + nb_ ], &Clocal[ jjlocal ] );
                    jjlocal += nb_;
                }
                jj += nb_;
            }
            assert( jjlocal == int64_t( Clocal.size() ) );

            print_matrix( "Aref_full", Aref_full, params );

            //==================================================
            // Run ScaLAPACK reference routine.
            //==================================================
            double time = barrier_get_wtime( MPI_COMM_WORLD );

            // Use rowcnd = 0.0 to force row scaling, 1.0 to avoid row scaling.
            real_t rowcnd = 1.0;
            if (equed == Equed::Both || equed == Equed::Row)
                rowcnd = 0.0;

            // Use colcnd = 0.0 to force col scaling, 1.0 to avoid col scaling.
            real_t colcnd = 1.0;
            if (equed == Equed::Both || equed == Equed::Col)
                colcnd = 0.0;

            char equed_out;
            scalapack_plaqge( m, n, &Aref_data[0], 1, 1, A_desc,
                              Rlocal.data(), Clocal.data(),
                              rowcnd, colcnd, A_max, &equed_out );
            slate_assert( info == 0 );

            time = barrier_get_wtime( MPI_COMM_WORLD ) - time;
            params.ref_time() = time;

            slate_assert( tolower( equed_out ) == tolower( equed2char( equed ) ) );

            print_matrix( "Aref_full_out", Aref_full, params );

            // Get difference A = A - Aref.
            // Do this on full m-by-n matrix to detect if on, say,
            // a lower triangular matrix, the kernel accidentally modifies
            // the upper triangle.
            slate::add( -one, Aref_full, one, Afull );
            real_t A_diff_norm = slate::norm( slate::Norm::One, Afull );

            print_matrix( "A_diff_full", Afull, params );

            real_t error = A_diff_norm / (n * A_max);
            params.error() = error;
            params.okay() = (error == 0);  // Set should be exact.

            Cblacs_gridexit( ictxt );
            //Cblacs_exit(1) does not handle re-entering
        #endif
    }
}

// -----------------------------------------------------------------------------
template <typename scalar_t>
void test_scale_row_col_dispatch(Params& params, bool run )
{
    std::string routine = params.routine;
    if (routine == "scale_row_col") {
        test_scale_row_col_work< slate::Matrix<scalar_t> >( params, run );
    }
    // todo: other matrix types
    // else if (routine == "tzscale_row_col") {
    //     test_scale_row_col_work< slate::TrapezoidMatrix<scalar_t> >( params, run );
    // }
    // else if (routine == "trscale_row_col") {
    //     test_scale_row_col_work< slate::TriangularMatrix<scalar_t> >( params, run );
    // }
    // else if (routine == "syscale_row_col") {
    //     test_scale_row_col_work< slate::SymmetricMatrix<scalar_t> >( params, run );
    // }
    // else if (routine == "hescale_row_col") {
    //     test_scale_row_col_work< slate::HermitianMatrix<scalar_t> >( params, run );
    // }
    else {
        throw slate::Exception( "unknown routine: " + routine );
    }
}

// -----------------------------------------------------------------------------
void test_scale_row_col( Params& params, bool run )
{
    switch (params.datatype()) {
        case testsweeper::DataType::Single:
            test_scale_row_col_dispatch< float >( params, run );
            break;

        case testsweeper::DataType::Double:
            test_scale_row_col_dispatch< double >( params, run );
            break;

        case testsweeper::DataType::SingleComplex:
            test_scale_row_col_dispatch< std::complex<float> >( params, run );
            break;

        case testsweeper::DataType::DoubleComplex:
            test_scale_row_col_dispatch< std::complex<double> >( params, run );
            break;

        default:
            throw std::exception();
            break;
    }
}
