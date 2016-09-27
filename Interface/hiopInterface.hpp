#ifndef HIOP_INTERFACE_BASE
#define HIOP_INTERFACE_BASE

#ifdef WITH_MPI
#include "mpi.h"
#else
#define MPI_Comm int
#define MPI_COMM_WORLD 0
#endif

class hiopInterfaceBase
{
  /** Base class for the solver's interface that has no assumptions how the 
   *  matrices are stored. The vectors are dense and distributed row-wise. 
   *  The data distribution is decided by the calling code (that implements 
   *  this interface) and specified to the optimization via 'get_vecdistrib_info'
   *
   *  Two possible implementations are for sparse NLPs and NLPs with small 
   *  number of global constraints.
   *  
   *  
   */
public:
  enum NonlinearityType{ hiopLinear=0, hiopQuadratic, hiopNonlinear};
public:
  hiopInterfaceBase() {};
  virtual ~hiopInterfaceBase() {};

  /** problem dimensions: n number of variables, m number of constraints */
  virtual bool get_prob_sizes(long long& n, long long& m)=0;
  /** bounds on the variables 
   *  (xlow<=-1e20 means no lower bound, xupp>=1e20 means no upper bound) */
  virtual bool get_vars_info(const long long& n, double *xlow, double* xupp, NonlinearityType* type)=0;
  /** bounds on the constraints 
   *  (clow<=-1e20 means no lower bound, cupp>=1e20 means no upper bound) */
  virtual bool get_cons_info(const long long& m, double* clow, double* cupp, NonlinearityType* type)=0;

  //! initial point specification

  /** Objective function evaluation
   *  When MPI enabled, each rank returns the obj. value. Also, x points to the local entries and 
   *  the function is responsible for knowing the local buffer size.
   */
  virtual bool eval_f(const long long& n, const double* x, bool new_x, double& obj_value)=0;
  /** Gradient of objective.
   *  When MPI enabled, each rank works only with local buffers x and gradf.
   */
  virtual bool eval_grad_f(const long long& n, const double* x, bool new_x, double* gradf)=0;

  /** Evaluates a subset of the constraints cons(x) (where clow<=cons(x)<=cupp). The subset is of size
   *  'num_cons' and is described by indexes in the 'idx_cons' array. The methods may be called 
   *  multiple times, each time for a subset of the constraints, for example, for the 
   *  subset containing the equalities and for the subset containing the inequalities. However, each 
   *  constraint will be inquired EXACTLY once. This is done for performance considerations, to avoid 
   *  temporary holders and memory copying.
   *
   *  Parameters:
   *   - n, m: the global number of variables and constraints
   *   - num_cons, idx_cons (array of size num_cons): the number and indexes of constraints to be evaluated
   *   - x: the point where the constraints are to be evaluated
   *   - new_x: whether x has been changed from the previous call to f, grad_f, or Jac
   *   - cons: array of size num_cons containing the value of the  constraints indicated by idx_cons
   *  
   *  When MPI enabled, every rank populates 'cons' since the constraints are not distributed.
   */
  virtual bool eval_cons(const long long& n, const long long& m, 
			 const long long& num_cons, const long long* idx_cons,  
			 const double* x, bool new_x, 
			 double* cons)=0;
  /** Jacobian of constraints is to be specified in a derived class since it can be sparse 
   *  or dense+distributed
  virtual bool eval_Jac_cons(const long long& n, const long long& m, const double* x, bool new_x, ...)
  */

  /** pass the communicator, defaults to MPI_COMM_WORLD (dummy for non-MPI builds)  */
  virtual bool get_MPI_comm(MPI_Comm& comm_out) { comm_out=MPI_COMM_WORLD; return true;}
  /**  column partitioning specification for distributed memory vectors 
  *  Process P owns cols[P], cols[P]+1, ..., cols[P+1]-1, P={0,1,...,NumRanks}.
  *  Example: for a vector x of 6 elements on 3 ranks, the col partitioning is cols=[0,2,4,6].
  *  The caller manages memory associated with 'cols', array of size NumRanks+1 
  */
  virtual bool get_vecdistrib_info(long long global_n, long long* cols) {
    return false; //defaults to serial 
  }

private:
  hiopInterfaceBase(const hiopInterfaceBase& ) {};
  void operator=(const hiopInterfaceBase&) {};
};

/** Specialized interface for NLPs with 'global' but few constraints. 
 */
class hiopInterfaceDenseConstraints : public hiopInterfaceBase 
{
public:
  hiopInterfaceDenseConstraints() {};
  virtual ~hiopInterfaceDenseConstraints() {};
  /** Evaluates the Jacobian of the subset of constraints indicated by idx_cons and of size num_cons.
   *  Example: Assuming idx_cons[k]=i, which means that the gradient of the (i+1)th constraint is
   *  to be evaluated, one needs to do Jac[k][0]=d/dx_0 con_i(x), Jac[k][1]=d/dx_1 con_i(x), ...
   *  When MPI enabled, each rank computes only the local columns of the Jacobian, that is the partials
   *  with respect to local variables.
   *
   *  Parameters: see eval_cons
   */
  virtual bool eval_Jac_cons(const long long& n, const long long& m, 
			     const long long& num_cons, const long long* idx_cons,  
			     const double* x, bool new_x,
			     double** Jac) = 0;
			 

};

#endif