#include "hiopMatrixRajaSparseTriplet.hpp"
#include "hiopVectorRajaPar.hpp"

#include <umpire/Allocator.hpp>
#include <umpire/ResourceManager.hpp>
#include <RAJA/RAJA.hpp>

#include "hiop_blasdefs.hpp"

#include <algorithm> //for std::min
#include <cmath> //for std::isfinite
#include <cstring>

#include <cassert>

namespace hiop
{
#ifdef HIOP_USE_GPU
  #include "cuda.h"
  using hiop_raja_exec   = RAJA::cuda_exec<128>;
  using hiop_raja_reduce = RAJA::cuda_reduce;
  using hiop_raja_atomic = RAJA::cuda_atomic;
  #define RAJA_LAMBDA [=] __device__
#else
  using hiop_raja_exec   = RAJA::omp_parallel_for_exec;
  using hiop_raja_reduce = RAJA::omp_reduce;
  using hiop_raja_atomic = RAJA::omp_atomic;
  #define RAJA_LAMBDA [=]
#endif


/// @brief Constructs a hiopMatrixRajaSparseTriplet with the given dimensions and memory space
hiopMatrixRajaSparseTriplet::hiopMatrixRajaSparseTriplet(int rows, int cols, int _nnz, std::string memspace)
  : hiopMatrixSparse(rows, cols, _nnz), row_starts_host(NULL), mem_space_(memspace)
{
  if(rows==0 || cols==0)
  {
    assert(nnz==0 && "number of nonzeros must be zero when any of the dimensions are 0");
    nnz = 0;
  }

#ifndef HIOP_USE_GPU
  mem_space_ = "HOST";
#endif

  //printf("Memory space: %s\n", mem_space_.c_str());

  auto& resmgr = umpire::ResourceManager::getInstance();
  umpire::Allocator devAlloc = resmgr.getAllocator(mem_space_);
  umpire::Allocator hostAlloc = resmgr.getAllocator("HOST");

  iRow_ = static_cast<int*>(devAlloc.allocate(nnz * sizeof(int)));
  jCol_ = static_cast<int*>(devAlloc.allocate(nnz * sizeof(int)));
  values_ = static_cast<double*>(devAlloc.allocate(nnz * sizeof(double)));

  // create host mirror if memory space is on the device
  if (mem_space_ == "DEVICE")
  {
    iRow_host_ = static_cast<int*>(hostAlloc.allocate(nnz * sizeof(int)));
    jCol_host_ = static_cast<int*>(hostAlloc.allocate(nnz * sizeof(int)));
    values_host_ = static_cast<double*>(hostAlloc.allocate(nnz * sizeof(double)));
  }
  else
  {
    iRow_host_ = iRow_;
    jCol_host_ = jCol_;
    values_host_ = values_;
  }
}

/// @brief Destructor for hiopMatrixRajaSparseTriplet
hiopMatrixRajaSparseTriplet::~hiopMatrixRajaSparseTriplet()
{
  delete row_starts_host;
  auto& resmgr = umpire::ResourceManager::getInstance();
  umpire::Allocator devAlloc = resmgr.getAllocator(mem_space_);
  umpire::Allocator hostAlloc = resmgr.getAllocator("HOST");

  devAlloc.deallocate(iRow_);
  devAlloc.deallocate(jCol_);
  devAlloc.deallocate(values_);

  // deallocate host mirror if memory space is on device
  if (mem_space_ == "DEVICE")
  {
    hostAlloc.deallocate(iRow_host_);
    hostAlloc.deallocate(jCol_host_);
    hostAlloc.deallocate(values_host_);
  }
}

/**
 * @brief Sets all the values of this matrix to zero.
 */
void hiopMatrixRajaSparseTriplet::setToZero()
{
  setToConstant(0.0);
}

/**
 * @brief Sets all the values of this matrix to some constant.
 * 
 * @param c A real number.
 */
void hiopMatrixRajaSparseTriplet::setToConstant(double c)
{
  double* dd = this->values_;
  auto nz = nnz;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nz),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      dd[i] = c;
    });
}

/**
 * @brief Multiplies this matrix by a vector and stores it in an output vector.
 * 
 * @param beta Amount to scale the output vector by before adding to it.
 * @param y The output vector.
 * @param alpha The amount to scale this matrix by before multiplying.
 * @param x The vector by which to multiply this matrix.
 * 
 * @pre _x_'s length must equal the number of columns in this matrix.
 * @pre _y_'s length must equal the number of rows in this matrix.
 * @post _y_ will contain the output of the following equation:
 * 
 * The full operation performed is:
 * _y_ = _beta_ * _y_ + _alpha_ * this * _x_
 */
void hiopMatrixRajaSparseTriplet::timesVec(double beta,  hiopVector& y,
  double alpha, const hiopVector& x) const
{
  assert(x.get_size() == ncols);
  assert(y.get_size() == nrows);

  auto& yy = dynamic_cast<hiopVectorRajaPar&>(y);
  const auto& xx = dynamic_cast<const hiopVectorRajaPar&>(x);

  double* y_data = yy.local_data();
  const double* x_data = xx.local_data_const();

  timesVec(beta, y_data, alpha, x_data);
}
 
/**
 * @brief Multiplies this matrix by a vector and stores it in an output vector.
 * 
 * @see above timesVec function for more detail. This overload takes raw data
 * pointers rather than hiop constructs.
 */
void hiopMatrixRajaSparseTriplet::timesVec(double beta,  double* y,
  double alpha, const double* x) const
{
  // y = beta * y
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nrows),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      y[i] *= beta;
    });

#ifndef NDEBUG
  auto nrs = nrows;
  auto ncs = ncols;
#endif
  auto irw = iRow_;
  auto jcl = jCol_;
  auto vls = values_;
  // atomic is needed to prevent data race from ocurring;
  // y[jCol_[i]] can be referenced by multiple threads concurrently
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      assert(irw[i] < nrs);
      assert(jcl[i] < ncs);
      RAJA::AtomicRef<double, hiop_raja_atomic> yy(&y[irw[i]]);
      yy += alpha * x[jcl[i]] * vls[i];
    });
}

/**
 * @brief Multiplies the transpose of this matrix by a vector and stores it 
 * in an output vector.
 * 
 * @see above timesVec function for more detail. This function implicitly transposes
 * this matrix for the multiplication.
 * 
 * The full operation performed is:
 * y = beta * y + alpha * this^T * x
 */
void hiopMatrixRajaSparseTriplet::transTimesVec(double beta, hiopVector& y,
  double alpha, const hiopVector& x) const
{
  assert(x.get_size() == nrows);
  assert(y.get_size() == ncols);

  hiopVectorRajaPar& yy = dynamic_cast<hiopVectorRajaPar&>(y);
  const hiopVectorRajaPar& xx = dynamic_cast<const hiopVectorRajaPar&>(x);
  
  double* y_data = yy.local_data();
  const double* x_data = xx.local_data_const();
  
  transTimesVec(beta, y_data, alpha, x_data);
}
 
/**
 * @brief Multiplies the transpose of this matrix by a vector and stores it 
 * in an output vector.
 * 
 * @see above transTimesVec function for more detail. This overload takes raw data
 * pointers rather than hiop constructs.
 * 
 * The full operation performed is:
 * y = beta * y + alpha * this^T * x
 */
void hiopMatrixRajaSparseTriplet::transTimesVec(double beta, double* y,
  double alpha, const double* x ) const
{
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, ncols),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      y[i] *= beta;
    });
  
#ifndef NDEBUG
  int num_rows = nrows;
  int num_cols = ncols;
#endif
  int* iRow = iRow_;
  int* jCol = jCol_;
  double* values = values_;
  // atomic is needed to prevent data race from ocurring;
  // y[jCol_[i]] can be referenced by multiple threads concurrently
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      assert(iRow[i] < num_rows);
      assert(jCol[i] < num_cols);
      RAJA::AtomicRef<double, hiop_raja_atomic> yy(&y[jCol[i]]);
      yy += alpha * x[iRow[i]] * values[i];
    });
}

void hiopMatrixRajaSparseTriplet::timesMat(double beta, hiopMatrix& W, 
				       double alpha, const hiopMatrix& X) const
{
  assert(false && "not needed");
}

void hiopMatrixRajaSparseTriplet::transTimesMat(double beta, hiopMatrix& W, 
					    double alpha, const hiopMatrix& X) const
{
  assert(false && "not needed");
}

void hiopMatrixRajaSparseTriplet::timesMatTrans(double beta, hiopMatrix& W, 
					    double alpha, const hiopMatrix& X) const
{
  assert(false && "not needed");
}
void hiopMatrixRajaSparseTriplet::addDiagonal(const double& alpha, const hiopVector& d_)
{
  assert(false && "not needed");
}
void hiopMatrixRajaSparseTriplet::addDiagonal(const double& value)
{
  assert(false && "not needed");
}
void hiopMatrixRajaSparseTriplet::addSubDiagonal(const double& alpha, long long start, const hiopVector& d_)
{
  assert(false && "not needed");
}

void hiopMatrixRajaSparseTriplet::addMatrix(double alpha, const hiopMatrix& X)
{
  assert(false && "not needed");
}

/**
 * @brief Adds the contents of this matrix to a block within a dense matrix.
 * 
 * @todo Test this function
 * @todo Better document this function
 * 
 * block of W += alpha*this
 * Note W; contains only the upper triangular entries
 */
void hiopMatrixRajaSparseTriplet::addToSymDenseMatrixUpperTriangle(int row_start, int col_start, 
  double alpha, hiopMatrixDense& W) const
{
  assert(row_start>=0 && row_start+nrows<=W.m());
  assert(col_start>=0 && col_start+ncols<=W.n());
  assert(W.n()==W.m());

  RAJA::View<double, RAJA::Layout<2>> WM(W.local_buffer(), W.m(), W.n());
  auto Wm = W.m();
  auto Wn = W.n();
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type it)
    {
      const int i = iRow_[it] + row_start;
      const int j = jCol_[it] + col_start;
#ifdef HIOP_DEEPCHECKS
      assert(i < Wm && j < Wn);
      assert(i >= 0 && j >= 0);
      assert(i <= j && "source entries need to map inside the upper triangular part of destination");
#endif
      WM(i, j) += alpha * values_host_[it];
    });
}

/**
 * @brief Adds the transpose of this matrix to a block within a dense matrix.
 * 
 * @todo Test this function
 * @todo Better document this function
 * 
 * block of W += alpha*transpose(this) 
 * Note W; contains only the upper triangular entries
 */
void hiopMatrixRajaSparseTriplet::transAddToSymDenseMatrixUpperTriangle(int row_start, int col_start, 
  double alpha, hiopMatrixDense& W) const
{
  assert(row_start>=0 && row_start+ncols<=W.m());
  assert(col_start>=0 && col_start+nrows<=W.n());
  assert(W.n()==W.m());

  RAJA::View<double, RAJA::Layout<2>> WM(W.local_buffer(), W.m(), W.n());
  auto Wm = W.m();
  auto Wn = W.n();
  int* iRow = iRow_;
  int* jCol = jCol_;
  double* values = values_;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type it)
    {
      const int i = jCol[it] + row_start;
      const int j = iRow[it] + col_start;
#ifdef HIOP_DEEPCHECKS
      assert(i < Wm && j < Wn);
      assert(i>=0 && j>=0);
      assert(i<=j && "source entries need to map inside the upper triangular part of destination");
#endif
      WM(i, j) += alpha * values[it];
    });
}

/**
 * @brief Finds the maximum absolute value of the values in this matrix.
 */
double hiopMatrixRajaSparseTriplet::max_abs_value()
{
  double* values = values_;
  RAJA::ReduceMax<hiop_raja_reduce, double> norm(0.0);
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      norm.max(fabs(values[i]));
    });
  double maxv = static_cast<double>(norm.get());
  return maxv;
}

/**
 * @brief Returns whether all the values of this matrix are finite or not.
 */
bool hiopMatrixRajaSparseTriplet::isfinite() const
{
#ifdef HIOP_DEEPCHECKS
  assert(this->checkIndexesAreOrdered());
#endif
  double* values = values_;
  RAJA::ReduceSum<hiop_raja_reduce, int> any(0);
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      if (!std::isfinite(values[i]))
        any += 1;
    });

  return any.get() == 0;
}

/**
 * @brief Allocates a new hiopMatrixRajaSparseTriplet with the same dimensions
 * and size as this one.
 */
hiopMatrix* hiopMatrixRajaSparseTriplet::alloc_clone() const
{
  return new hiopMatrixRajaSparseTriplet(nrows, ncols, nnz, mem_space_);
}

/**
 * @brief Creates a deep copy of this matrix.
 */
hiopMatrix* hiopMatrixRajaSparseTriplet::new_copy() const
{
#ifdef HIOP_DEEPCHECKS
  assert(this->checkIndexesAreOrdered());
#endif
  hiopMatrixRajaSparseTriplet* copy = new hiopMatrixRajaSparseTriplet(nrows, ncols, nnz, mem_space_);
  auto& resmgr = umpire::ResourceManager::getInstance();
  resmgr.copy(copy->iRow_, iRow_);
  resmgr.copy(copy->jCol_, jCol_);
  resmgr.copy(copy->values_, values_);
  resmgr.copy(copy->iRow_host_, iRow_host_);
  resmgr.copy(copy->jCol_host_, jCol_host_);
  resmgr.copy(copy->values_host_, values_host_);
  return copy;
}

void hiopMatrixRajaSparseTriplet::copyFrom(const hiopMatrixSparse& dm)
{
  assert(false && "this is to be implemented - method def too vague for now");
}

#ifdef HIOP_DEEPCHECKS
/// @brief Ensures the rows and column triplet entries are ordered.
bool hiopMatrixRajaSparseTriplet::checkIndexesAreOrdered() const
{
  copyFromDev();
  if(nnz==0)
    return true;
  for(int i=1; i<nnz; i++)
  {
    if(iRow_host_[i] < iRow_host_[i-1])
      return false;
    /* else */
    if(iRow_host_[i] == iRow_host_[i-1])
      if(jCol_host_[i] < jCol_host_[i-1])
        return false;
  }
  return true;
}
#endif

/**
 * @brief This function cannot be described briefly. See below for more detail.
 * 
 * @param rowAndCol_dest_start Starting row & col within _W_ to be added to
 * in the operation.
 * @param alpha Amount to scale this matrix's values by in the operation.
 * @param D The inverse of this vector's values will be multiplied by with this
 * matrix's values in the operation.
 * @param W The output matrix, a block of which's values will be added to in
 * the operation.
 * 
 * @pre rowAndCol_dest_start >= 0
 * @pre rowAndCol_dest_start + this->nrows <= W.m()
 * @pre rowAndCol_dest_start + this->nrows <= W.n()
 * @pre D.get_size() == this->ncols
 * 
 * @post A this->nrows^2 block will be written to in _W_, containing the output
 * of the operation. 
 * 
 * The full operation performed is:
 * diag block of _W_ += _alpha_ * this * _D_^{-1} * transpose(this)
 */
void hiopMatrixRajaSparseTriplet::
addMDinvMtransToDiagBlockOfSymDeMatUTri(int rowAndCol_dest_start,
  const double& alpha, 
  const hiopVector& D, hiopMatrixDense& W) const
{
  const int row_dest_start = rowAndCol_dest_start, col_dest_start = rowAndCol_dest_start;
#ifndef NDEBUG
  int n = this->nrows;
  int num_non_zero = this->nnz;
#endif
  assert(row_dest_start>=0 && row_dest_start+n<=W.m());
  assert(col_dest_start>=0 && col_dest_start+nrows<=W.n());
  assert(D.get_size() == this->ncols);
  RAJA::View<double, RAJA::Layout<2>> WM(W.local_buffer(), W.m(), W.n());
  const double* DM = D.local_data_const();
  
  if(row_starts_host==NULL)
    row_starts_host = allocAndBuildRowStarts();
  assert(row_starts_host);

  int num_rows = this->nrows;
  int* idx_start = row_starts_host->idx_start_;
  int* jCol = jCol_;
  double* values = values_;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, this->nrows),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      //j==i
      double acc = 0.;
      for(int k=idx_start[i]; k<idx_start[i+1]; k++)
      {
        acc += values[k] / DM[jCol[k]] * values[k];
      }
      WM(i + row_dest_start, i + col_dest_start) += alpha*acc;

      //j>i
      for(int j=i+1; j<num_rows; j++)
      {
        //dest[i,j] = weigthed_dotprod(this_row_i,this_row_j)
        acc = 0.;

        int ki=idx_start[i];
        int kj=idx_start[j];
        while(ki<idx_start[i+1] && kj<idx_start[j+1])
        {
          assert(ki < num_non_zero);
          assert(kj < num_non_zero);
          if(jCol[ki] == jCol[kj])
          {
            acc += values[ki] / DM[jCol[ki]] * values[kj];
            ki++;
            kj++;
          }
          else
          {
            if(jCol[ki] < jCol[kj])
              ki++;
            else
              kj++;
          }
        } //end of loop over ki and kj

        WM(i + row_dest_start, j + col_dest_start) += alpha*acc;
      } //end j
    });
}

/**
 * @brief This function cannot be described briefly. See below for more detail.
 * 
 * @param row_dest_start Starting row in destination block.
 * @param col_dest_start Starting col in destination block.
 * @param alpha Amount to scale this matrix by during the operation.
 * @param D The inverse of this vector's values will be multiplied by with this
 * matrix's values in the operation.
 * @param M2mat Another sparse matrix, the transpose of which will be multiplied in
 * the following operation.
 * @param W A dense matrix, a block in which will be used to store the result of 
 * the operation.
 * 
 * @pre this->ncols == M2mat.ncols
 * @pre D.get_size() == this->ncols
 * @pre row_dest_start >= 0 
 * @pre row_dest_start + this->nrows <= W.m()
 * @pre col_dest_start >= 0
 * @pre col_dest_start + M2mat.nrows <= W.n()
 * 
 * The full operation performed is:
 * block of _W_ += _alpha_ * this * _D_^{-1} * transpose(_M2mat_)
 * Sizes: M1 is (m1 x nx);  D is vector of len nx, M2 is  (m2, nx).
 */
void hiopMatrixRajaSparseTriplet::
addMDinvNtransToSymDeMatUTri(int row_dest_start, int col_dest_start,
  const double& alpha, 
  const hiopVector& D, const hiopMatrixSparse& M2mat,
  hiopMatrixDense& W) const
{
  const auto& M2 = dynamic_cast<const hiopMatrixRajaSparseTriplet&>(M2mat);
  const hiopMatrixRajaSparseTriplet& M1 = *this;
  
  const int m1 = M1.nrows;
#ifndef NDEBUG
  const int nx = M1.ncols;
#endif
  const int m2 = M2.nrows;
  assert(nx==M2.ncols);
  assert(D.get_size() == nx);

  //does it fit in W ?
  assert(row_dest_start>=0 && row_dest_start+m1<=W.m());
  assert(col_dest_start>=0 && col_dest_start+m2<=W.n());

  //double** WM = W.get_M();
  RAJA::View<double, RAJA::Layout<2>> WM(W.local_buffer(), W.m(), W.n());
  const double* DM = D.local_data_const();

  // TODO: allocAndBuildRowStarts -> should create row_starts_host internally (name='prepareRowStarts' ?)
  if(M1.row_starts_host==NULL)
    M1.row_starts_host = M1.allocAndBuildRowStarts();
  assert(M1.row_starts_host);

  if(M2.row_starts_host==NULL)
    M2.row_starts_host = M2.allocAndBuildRowStarts();
  assert(M2.row_starts_host);

  int* M1_idx_start = M1.row_starts_host->idx_start_;
  int* M2_idx_start = M2.row_starts_host->idx_start_;
#ifndef NDEBUG
  int M1nnz = M1.nnz;
  int M2nnz = M2.nnz;
#endif
  int* M1jCol = M1.jCol_;
  int* M2jCol = M2.jCol_;
  double* M1values = M1.values_;
  double* M2values = M2.values_;
  int* jCol = jCol_;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, m1),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      for(int j=0; j<m2; j++)
      {
        // dest[i,j] = weigthed_dotprod(M1_row_i,M2_row_j)
        double acc = 0.;
        int ki=M1_idx_start[i];
        int kj=M2_idx_start[j];
        
        while(ki<M1_idx_start[i+1] && kj<M2_idx_start[j+1])
        {
          assert(ki<M1nnz);
          assert(kj<M2nnz);

          if(M1jCol[ki] == M2jCol[kj])
          {
            acc += M1values[ki] / DM[jCol[ki]] * M2values[kj];
            ki++;
            kj++;
          }
          else
          {
            if(M1jCol[ki] < M2jCol[kj])
              ki++;
            else
              kj++;
          }
        } //end of loop over ki and kj

#ifdef HIOP_DEEPCHECKS
        if(i+row_dest_start > j+col_dest_start)
          printf("[warning] lower triangular element updated in addMDinvNtransToSymDeMatUTri\n");
        assert(i+row_dest_start <= j+col_dest_start);
#endif
        WM(i+row_dest_start, j+col_dest_start) += alpha*acc;
      } //end j
    });
}


/**
 * @brief Generates a pointer to a single RowStartsInfo struct containing
 * the number of rows and indices at which row data starts from this matrix.
 * 
 * Assumes triplets are ordered.
 */
hiopMatrixRajaSparseTriplet::RowStartsInfo* 
hiopMatrixRajaSparseTriplet::allocAndBuildRowStarts() const
{
  assert(nrows >= 0);

  RowStartsInfo* rsi = new RowStartsInfo(nrows, "HOST"); assert(rsi);
  RowStartsInfo* rsi_dev = new RowStartsInfo(nrows, mem_space_); assert(rsi_dev);
  if(nrows<=0)
  {
    delete rsi;
    return rsi_dev;
  }

  // build rsi on the host, then copy it to the device for simplicity
  int it_triplet = 0;
  rsi->idx_start_[0] = 0;
  for(int i = 1; i <= this->nrows; i++)
  {
    rsi->idx_start_[i]=rsi->idx_start_[i-1];
    
    while(it_triplet < this->nnz && this->iRow_host_[it_triplet] == i - 1)
    {
#ifdef HIOP_DEEPCHECKS
      if(it_triplet>=1)
      {
        assert(iRow_host_[it_triplet-1]<=iRow_host_[it_triplet] && "row indices are not sorted");
        //assert(iCol[it_triplet-1]<=iCol[it_triplet]);
        if(iRow_host_[it_triplet-1]==iRow_host_[it_triplet])
          assert(jCol_host_[it_triplet-1] < jCol_host_[it_triplet] && "col indices are not sorted");
      }
#endif
      rsi->idx_start_[i]++;
      it_triplet++;
    }
    assert(rsi->idx_start_[i] == it_triplet);
  }
  assert(it_triplet==this->nnz);

  auto& rm = umpire::ResourceManager::getInstance();
  rm.copy(rsi_dev->idx_start_, rsi->idx_start_);

  delete rsi;
  return rsi_dev;
}

/**
 * @brief Copies rows from another sparse matrix into this one.
 * Not implemented (and won't be). Do not use!
 * 
 * @todo Better document this function.
 */
void hiopMatrixRajaSparseTriplet::copyRowsFrom(const hiopMatrix& src_gen,
					   const long long* rows_idxs,
					   long long n_rows)
{
  assert(false && "This function does not exist for sparse triplet matrices!");
}
  
/// @brief Prints the contents of this function to a file.
void hiopMatrixRajaSparseTriplet::print(FILE* file, const char* msg/*=NULL*/, 
				    int maxRows/*=-1*/, int maxCols/*=-1*/, 
				    int rank/*=-1*/) const 
{
  int myrank_=0, numranks=1; //this is a local object => always print
  copyFromDev();

  if(file==NULL) file = stdout;

  int max_elems = maxRows>=0 ? maxRows : nnz;
  max_elems = std::min(max_elems, nnz);

  if(myrank_==rank || rank==-1) {

    if(NULL==msg) {
      if(numranks>1)
        fprintf(file, "matrix of size %lld %lld and nonzeros %lld, printing %d elems (on rank=%d)\n", 
		m(), n(), numberOfNonzeros(), max_elems, myrank_);
      else
        fprintf(file, "matrix of size %lld %lld and nonzeros %lld, printing %d elems\n", 
		m(), n(), numberOfNonzeros(), max_elems);
    } else {
      fprintf(file, "%s ", msg);
    }    

    // using matlab indices
    fprintf(file, "iRow_host_=[");
    for(int it=0; it<max_elems; it++)  fprintf(file, "%d; ", iRow_host_[it]+1);
    fprintf(file, "];\n");
    
    fprintf(file, "jCol_host_=[");
    for(int it=0; it<max_elems; it++)  fprintf(file, "%d; ", jCol_host_[it]+1);
    fprintf(file, "];\n");
    
    fprintf(file, "v=[");
    for(int it=0; it<max_elems; it++)  fprintf(file, "%22.16e; ", values_host_[it]);
    fprintf(file, "];\n");
  }
}

/// @brief Copies the data stored in the host mirror to the device.
void hiopMatrixRajaSparseTriplet::copyToDev()
{
  if (mem_space_ == "DEVICE")
  {
    auto& resmgr = umpire::ResourceManager::getInstance();
    resmgr.copy(iRow_, iRow_host_);
    resmgr.copy(jCol_, jCol_host_);
    resmgr.copy(values_, values_host_);
  }
}

/// @brief Copies the data stored on the device to the host mirror.
void hiopMatrixRajaSparseTriplet::copyFromDev() const
{
  if (mem_space_ == "DEVICE")
  {
    auto& resmgr = umpire::ResourceManager::getInstance();
    resmgr.copy(iRow_host_, iRow_);
    resmgr.copy(jCol_host_, jCol_);
    resmgr.copy(values_host_, values_);
  }
}

hiopMatrixRajaSparseTriplet::RowStartsInfo::RowStartsInfo(int n_rows, std::string memspace)
  : num_rows_(n_rows), mem_space_(memspace)
{
  auto& rm = umpire::ResourceManager::getInstance();
  umpire::Allocator alloc = rm.getAllocator(mem_space_);
  idx_start_ = static_cast<int*>(alloc.allocate((num_rows_ + 1) * sizeof(int)));
}

hiopMatrixRajaSparseTriplet::RowStartsInfo::~RowStartsInfo()
{
  auto& rm = umpire::ResourceManager::getInstance();
  umpire::Allocator alloc = rm.getAllocator(mem_space_);
  alloc.deallocate(idx_start_);
}


/**********************************************************************************
  * Sparse symmetric matrix in triplet format. Only the UPPER triangle is stored
  **********************************************************************************/
void hiopMatrixRajaSymSparseTriplet::timesVec(double beta,  hiopVector& y,
					  double alpha, const hiopVector& x ) const
{
  assert(ncols == nrows);
  assert(x.get_size() == ncols);
  assert(y.get_size() == nrows);

  auto& yy = dynamic_cast<hiopVectorRajaPar&>(y);
  const auto& xx = dynamic_cast<const hiopVectorRajaPar&>(x);

  double* y_data = yy.local_data();
  const double* x_data = xx.local_data_const();

  timesVec(beta, y_data, alpha, x_data);
}
 
/** y = beta * y + alpha * this * x */
void hiopMatrixRajaSymSparseTriplet::timesVec(double beta,  double* y,
					  double alpha, const double* x) const
{
  assert(ncols == nrows);
  
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nrows),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      y[i] *= beta;
    });

  // addition to y[iRow[i]] must be atomic
  auto iRow = this->iRow_;
  auto jCol = this->jCol_;
  auto values = this->values_;
#ifndef NDEBUG
  auto nrows = this->nrows;
  auto ncols = this->ncols;
#endif
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type i)
    {
      assert(iRow[i] < nrows);
      assert(jCol[i] < ncols);
      RAJA::AtomicRef<double, hiop_raja_atomic> yy(&y[iRow[i]]);
      yy += alpha * x[jCol[i]] * values[i];
      if(iRow[i] != jCol[i])
        yy += alpha * x[iRow[i]] * values[i];
    });
}

hiopMatrix* hiopMatrixRajaSymSparseTriplet::alloc_clone() const
{
  assert(nrows == ncols);
  return new hiopMatrixRajaSymSparseTriplet(nrows, nnz, mem_space_);
}

hiopMatrix* hiopMatrixRajaSymSparseTriplet::new_copy() const
{
  assert(nrows == ncols);
  auto* copy = new hiopMatrixRajaSymSparseTriplet(nrows, nnz, mem_space_);
  auto& resmgr = umpire::ResourceManager::getInstance();
  resmgr.copy(copy->iRow_, iRow_);
  resmgr.copy(copy->jCol_, jCol_);
  resmgr.copy(copy->values_, values_);
  resmgr.copy(copy->iRow_host_, iRow_host_);
  resmgr.copy(copy->jCol_host_, jCol_host_);
  resmgr.copy(copy->values_host_, values_host_);
  return copy;
}

/* block of W += alpha*this 
 * Note W; contains only the upper triangular entries */
void hiopMatrixRajaSymSparseTriplet::addToSymDenseMatrixUpperTriangle(int row_start, int col_start, 
  double alpha, hiopMatrixDense& W) const
{
  assert(row_start>=0 && row_start+nrows<=W.m());
  assert(col_start>=0 && col_start+ncols<=W.n());
  assert(W.n()==W.m());

  // double** WM = W.get_M();
  RAJA::View<double, RAJA::Layout<2>> WM(W.local_buffer(), W.get_local_size_m(), W.get_local_size_n());
  auto Wm = W.m();
  auto Wn = W.n();
  auto iRow = this->iRow_;
  auto jCol = this->jCol_;
  auto values = this->values_;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type it)
    {
      assert(iRow[it]<=jCol[it] && "sparse symmetric matrices should contain only upper triangular entries");
      const int i = iRow[it]+row_start;
      const int j = jCol[it]+col_start;
      assert(i<Wm && j<Wn); assert(i>=0 && j>=0);
      assert(i<=j && "symMatrices not aligned; source entries need to map inside the upper triangular part of destination");
      RAJA::AtomicRef<double, hiop_raja_atomic> ww(&WM(i, j));
      
      // printf("i = %d, j = %d\n", i, j);
      // printf("values[%d] = %f\n", it, values[it]);
      
      ww += alpha * values[it];
    });
}


void hiopMatrixRajaSymSparseTriplet::transAddToSymDenseMatrixUpperTriangle(int row_start, int col_start, 
  double alpha, hiopMatrixDense& W) const
{
  assert(row_start>=0 && row_start+ncols<=W.m());
  assert(col_start>=0 && col_start+nrows<=W.n());
  assert(W.n()==W.m());

  //double** WM = W.get_M();
  RAJA::View<double, RAJA::Layout<2>> WM(W.local_buffer(), W.m(), W.n());
  auto Wm = W.m();
  auto Wn = W.n();
  auto iRow = this->iRow_;
  auto jCol = this->jCol_;
  auto values = this->values_;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type it)
    {
      assert(iRow[it]<=jCol[it] && "sparse symmetric matrices should contain only upper triangle entries");
      const int i = iRow[it]+row_start;
      const int j = jCol[it]+col_start;
      assert(i<Wm && j<Wn); assert(i>=0 && j>=0);
      assert(i <= j && "symMatrices not aligned; source entries need to map inside the upper triangular part of destination");
      WM(j, i) += alpha * values[it];
    });
}

/* extract subdiagonal from 'this' (source) and adds the entries to 'vec_dest' starting at
 * index 'vec_start'. If num_elems>=0, 'num_elems' are copied; otherwise copies as many as
 * are available in 'vec_dest' starting at 'vec_start'
 */
void hiopMatrixRajaSymSparseTriplet::
startingAtAddSubDiagonalToStartingAt(int diag_src_start, const double& alpha, 
  hiopVector& vec_dest, int vec_start, int num_elems/*=-1*/) const
{
  auto& vd = dynamic_cast<hiopVectorRajaPar&>(vec_dest);
  if(num_elems < 0)
    num_elems = vd.get_size();
  assert(num_elems<=vd.get_size());

  assert(diag_src_start>=0 && diag_src_start+num_elems<=this->nrows);
  double* v = vd.local_data();

  auto vds = vd.get_size();
  auto iRow = this->iRow_;
  auto jCol = this->jCol_;
  auto values = this->values_;
  RAJA::forall<hiop_raja_exec>(RAJA::RangeSegment(0, nnz),
    RAJA_LAMBDA(RAJA::Index_type itnz)
    {
      const int row = iRow[itnz];
      if(row == jCol[itnz])
      {
        if(row >= diag_src_start && row < diag_src_start + num_elems)
        {
          assert(row+vec_start < vds);
          v[vec_start + row] += alpha * values[itnz];
        }
      }
    });
}

} //end of namespace