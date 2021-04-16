// ex13_non_uniform_block_size.cc
// create 1000 x 1000 matrix on 2 x 2 MPI process grid, with non-uniform tile size
#include <slate/slate.hh>

#include "util.hh"

int mpi_size = 0;
int mpi_rank = 0;

//------------------------------------------------------------------------------
template <typename scalar_type>
void test_matrix_lambda()
{
    print_func( mpi_rank );

    int64_t n=1000, nb=256, p=2, q=2;
    assert( mpi_size == p*q );

    int nb_ = nb;
    int p_ = p;
    int q_ = q;
    int num_devices_ = 0;

    std::function< int64_t (int64_t j) >
    tileNb = [n, nb_](int64_t j)
    {
        return (j % 2 != 0 ? nb_/2 : nb_);
    };

    std::function< int (std::tuple<int64_t, int64_t> ij) >
    tileRank = [p_, q_](std::tuple<int64_t, int64_t> ij)
    {
        int64_t i = std::get<0>(ij);
        int64_t j = std::get<1>(ij);
        return int(i%p_ + (j%q_)*p_);
    };

    std::function< int (std::tuple<int64_t, int64_t> ij) >
    tileDevice = [num_devices_](std::tuple<int64_t, int64_t> ij)
    {
        int64_t i = std::get<0>(ij);
        return int(i)%num_devices_;
    };

    slate::Matrix<scalar_type> A(n, n, tileNb, tileNb, tileRank, tileDevice, MPI_COMM_WORLD);
    A.insertLocalTiles();

    for (int64_t j = 0; j < A.nt(); ++j) {
        for (int64_t i = 0; i < A.mt(); ++i) {
            if (A.tileIsLocal( i, j )) {
                slate::Tile<scalar_type> T = A( i, j );
                random_matrix( T.mb(), T.nb(), T.data(), T.stride() );
            }
        }
    }

    // verify nt, tileNb(i), and sum tileNb(i) == n
    int nt = A.nt();
    int jj = 0;
    for (int j = 0; j < nt; ++j) {
        assert( A.tileNb(j) == blas::min( tileNb(j), n - jj ) );
        jj += A.tileNb(j);
    }
    assert( jj == n );
}

//------------------------------------------------------------------------------
int main( int argc, char** argv )
{
    int provided = 0;
    int err = MPI_Init_thread( &argc, &argv, MPI_THREAD_MULTIPLE, &provided );
    assert( err == 0 );
    assert( provided == MPI_THREAD_MULTIPLE );

    err = MPI_Comm_size( MPI_COMM_WORLD, &mpi_size );
    assert( err == 0 );
    if (mpi_size != 4) {
        printf( "Usage: mpirun -np 4 %s  # 4 ranks hard coded\n", argv[0] );
        return -1;
    }

    err = MPI_Comm_rank( MPI_COMM_WORLD, &mpi_rank );
    assert( err == 0 );

    // so random_matrix is different on different ranks.
    srand( 100 * mpi_rank );

    test_matrix_lambda< float >();
    test_matrix_lambda< double >();
    test_matrix_lambda< std::complex<float> >();
    test_matrix_lambda< std::complex<double> >();

    err = MPI_Finalize();
    assert( err == 0 );
}
