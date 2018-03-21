
#include "slate.hh"
#include "slate_Debug.hh"
#include "slate_trace_Trace.hh"

#include "test.hh"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef SLATE_WITH_MPI
    #include <mpi.h>
#else
    #include "slate_NoMpi.hh"
#endif

#ifdef _OPENMP
    #include <omp.h>
#else
    #include "slate_NoOpenmp.hh"
#endif

//------------------------------------------------------------------------------
template <typename scalar_t>
void test_trsm(
    blas::Side side, blas::Uplo uplo, blas::Op opA, blas::Diag diag,
    int64_t m, int64_t n, int64_t nb, int p, int q, int64_t lookahead,
    slate::Target target, bool test, bool verbose, bool trace )
{
    using real_t = blas::real_type<scalar_t>;
    using blas::Op;
    using blas::real;
    using blas::imag;

    //--------------------
    // MPI initializations
    int mpi_rank;
    int mpi_size;
    int retval;

    retval = MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    assert(retval == MPI_SUCCESS);

    retval = MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    assert(retval == MPI_SUCCESS);
    assert(mpi_size == p*q);

    //---------------------
    // test initializations
    if (mpi_rank == 0) {
        printf( "side=%c, uplo=%c, opA=%c, diag=%c, m=%lld, n=%lld, nb=%lld, p=%d, q=%d, lookahead=%lld, target=%d\n",
                char(side), char(uplo), char(opA), char(diag), m, n, nb, p, q, lookahead, int(target) );
    }

    // for now, trsm requires full tiles
    assert(m % nb == 0);
    assert(n % nb == 0);

    int64_t An  = (side == blas::Side::Left ? m : n);
    int64_t lda = An;
    int64_t ldb = m;

    // todo: complex
    scalar_t alpha = 1.234;

    scalar_t *A1 = nullptr;
    scalar_t *B1 = nullptr;
    scalar_t *B2 = nullptr;

    int64_t seed_a[] = {0, 1, 0, 3};
    A1 = new scalar_t[ lda*An ];
    lapack::larnv(1, seed_a, lda*An, A1);

    // set unused data to nan
    if (uplo == blas::Uplo::Lower) {
        for (int j = 0; j < An; ++j)
            for (int i = 0; i < j && i < An; ++i)  // upper
                A1[ i + j*lda ] = nan("");
    }
    else {
        for (int j = 0; j < An; ++j)
            for (int i = j+1; i < An; ++i)  // lower
                A1[ i + j*lda ] = nan("");
    }

    int64_t seed_c[] = {0, 0, 0, 1};
    B1 = new scalar_t[ ldb*n ];
    lapack::larnv(1, seed_c, ldb*n, B1);

    if (test) {
        if (mpi_rank == 0) {
            B2 = new scalar_t[ ldb*n ];
            memcpy(B2, B1, sizeof(scalar_t)*ldb*n);
        }
    }

    slate::TriangularMatrix<scalar_t> A(uplo,
                                        An, A1, lda,
                                        nb, p, q, MPI_COMM_WORLD);
    slate::Matrix<scalar_t> B(m, n, B1, ldb, nb, p, q, MPI_COMM_WORLD);

    if (opA == Op::Trans)
        A = transpose( A );
    else if (opA == Op::ConjTrans)
        A = conj_transpose( A );

    if (verbose && mpi_rank == 0) {
        printf( "alpha = %.4f + %.4fi;\n",
                real(alpha), imag(alpha) );
        print( "A1", An, An, A1, lda );
        print( "A",  A );
        print( "B1", m, n, B1, ldb );
        print( "B",  B );
    }

    //---------------------
    // run test
    if (trace)
        slate::trace::Trace::on();

    {
        slate::trace::Block trace_block("MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    }
    double start = omp_get_wtime();

    switch (target) {
        case slate::Target::Host:
        case slate::Target::HostTask:
            slate::trsm<slate::Target::HostTask>(
                side, diag,
                alpha, A, B, {{slate::Option::Lookahead, lookahead}});
            break;
        case slate::Target::HostNest:
            slate::trsm<slate::Target::HostNest>(
                side, diag,
                alpha, A, B, {{slate::Option::Lookahead, lookahead}});
            break;
        case slate::Target::HostBatch:
            slate::trsm<slate::Target::HostBatch>(
                side, diag,
                alpha, A, B, {{slate::Option::Lookahead, lookahead}});
            break;
        case slate::Target::Devices:
            slate::trsm<slate::Target::Devices>(
                side, diag,
                alpha, A, B, {{slate::Option::Lookahead, lookahead}});
            break;
    }

    {
        slate::trace::Block trace_block("MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    }
    double time = omp_get_wtime() - start;

    if (trace)
        slate::trace::Trace::finish();

    if (verbose) {
        print( "B1res", m, n, B1, ldb );
        print( "Bres", B );
    }

    //--------------
    // Print GFLOPS.
    if (mpi_rank == 0) {
        double ops = (double)n*n*n;
        double gflops = ops/time/1e9;
        printf("\t%.0f GFLOPS\n", gflops);
        fflush(stdout);
    }

    //------------------
    // Test correctness.
    if (test) {
        B.gather(B1, ldb);

        if (mpi_rank == 0) {
            blas::trsm(blas::Layout::ColMajor,
                       side, uplo, opA, diag,
                       m, n,
                       alpha, A1, lda,
                              B2, ldb);

            if (verbose && mpi_rank == 0) {
                print( "Bref", m, n, B2, ldb );
            }
            if (verbose)
                slate::Debug::diffLapackMatrices(m, n, B1, ldb, B2, ldb, nb, nb);

            blas::axpy((size_t)ldb*n, -1.0, B1, 1, B2, 1);
            real_t norm =
                lapack::lange(lapack::Norm::Fro, m, n, B1, ldb);

            real_t error =
                lapack::lange(lapack::Norm::Fro, m, n, B2, ldb);

            if (norm != 0)
                error /= norm;

            real_t eps = std::numeric_limits< real_t >::epsilon();
            bool okay = (error < 50*eps);
            printf("\t%.2e error, %s\n", error, okay ? "ok" : "failed");

            delete[] B2;
            B2 = nullptr;
        }
    }
    delete[] B1;
    B1 = nullptr;
}

//------------------------------------------------------------------------------
int main (int argc, char *argv[])
{
    //--------------------
    // MPI initializations
    int provided;
    int retval;
    int mpi_rank;

    retval = MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
    assert(retval == MPI_SUCCESS);
    assert(provided >= MPI_THREAD_MULTIPLE);

    retval = MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    assert(retval == MPI_SUCCESS);

    //--------------------
    // parse command line
    if (argc < 11 && mpi_rank == 0) {
        printf("Usage: %s {Left,Right} {Upper,Lower} {Notrans,Trans,Conjtrans} {Nonunit,Unit} m n nb p q lookahead [HostTask|HostNest|HostBatch|Devices] [s|d|c|z] [test] [verbose] [trace]\n"
               "For side, uplo, opA, diag, only the first letter is used.\n", argv[0]);
        return EXIT_FAILURE;
    }

    int arg = 1;
    blas::Side side  = blas::char2side( argv[arg][0] ); ++arg;
    blas::Uplo uplo  = blas::char2uplo( argv[arg][0] ); ++arg;
    blas::Op   opA   = blas::char2op  ( argv[arg][0] ); ++arg;
    blas::Diag diag  = blas::char2diag( argv[arg][0] ); ++arg;
    int64_t m  = atol(argv[arg]); ++arg;
    int64_t n  = atol(argv[arg]); ++arg;
    int64_t nb = atol(argv[arg]); ++arg;
    int64_t p  = atol(argv[arg]); ++arg;
    int64_t q  = atol(argv[arg]); ++arg;
    int64_t lookahead = atol(argv[arg]); ++arg;

    slate::Target target = slate::Target::HostTask;
    if (argc > arg) {
        std::string s( argv[arg] );
        if (s == "HostTask")
            target = slate::Target::HostTask;
        else if (s == "HostNest")
            target = slate::Target::HostNest;
        else if (s == "HostBatch")
            target = slate::Target::HostBatch;
        else if (s == "Devices")
            target = slate::Target::Devices;
        else {
            printf( "Unknown target: %s\n", argv[arg] );
            return EXIT_FAILURE;
        }
        ++arg;
    }

    char datatype = 'd';
    if (argc > arg) {
        datatype = argv[arg][0];
        ++arg;
    }

    bool test    = argc > arg && std::string(argv[arg]) == "test";    ++arg;
    bool verbose = argc > arg && std::string(argv[arg]) == "verbose"; ++arg;
    bool trace   = argc > arg && std::string(argv[arg]) == "trace";   ++arg;

    //--------------------
    // run test
    switch (datatype) {
        case 's':
            test_trsm< float >( side, uplo, opA, diag, m, n, nb, p, q, lookahead, target, test, verbose, trace );
            break;
        case 'd':
            test_trsm< double >( side, uplo, opA, diag, m, n, nb, p, q, lookahead, target, test, verbose, trace );
            break;
        case 'c':
            test_trsm< std::complex<float> >( side, uplo, opA, diag, m, n, nb, p, q, lookahead, target, test, verbose, trace );
            break;
        case 'z':
            test_trsm< std::complex<double> >( side, uplo, opA, diag, m, n, nb, p, q, lookahead, target, test, verbose, trace );
            break;
        default:
            printf( "unknown datatype: %c\n", datatype );
            break;
    }

    //--------------------
    MPI_Finalize();
    return EXIT_SUCCESS;
}
