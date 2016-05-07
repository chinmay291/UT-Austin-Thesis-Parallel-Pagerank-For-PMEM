/*  -*- mode: c++ -*-  */
#include "gg.h"
#include "ggcuda.h"

void kernel_sizing(CSRGraph &, dim3 &, dim3 &);
#define TB_SIZE 256
const char *GGC_OPTIONS = "coop_conv=False $ outline_iterate_gb=False $ backoff_blocking_factor=4 $ parcomb=True $ np_schedulers=set(['wp', 'fg']) $ cc_disable=set([]) $ hacks=set([]) $ np_factor=8 $ instrument=set([]) $ unroll=[] $ read_props=None $ outline_iterate=True $ ignore_nested_errors=False $ np=True $ write_props=None $ quiet_cgen=True $ retry_backoff=True $ cuda.graph_type=basic $ cuda.use_worklist_slots=True $ cuda.worklist_type=basic";
unsigned int * P_DIST_CURRENT;
#include "kernels/reduce.cuh"
#include "gen_cuda.cuh"
__global__ void InitializeGraph(CSRGraph graph, int  nowned, unsigned int local_infinity, int local_src_node, unsigned int * p_dist_current)
{
  unsigned tid = TID_1D;
  unsigned nthreads = TOTAL_THREADS_1D;

  const unsigned __kernel_tb_size = TB_SIZE;
  index_type src_end;
  src_end = nowned;
  for (index_type src = 0 + tid; src < src_end; src += nthreads)
  {
    p_dist_current[src] = (src == local_src_node) ? 0 : local_infinity;
  }
}
__global__ void SSSP(CSRGraph graph, int  nowned, unsigned int * p_dist_current, Any any_retval)
{
  unsigned tid = TID_1D;
  unsigned nthreads = TOTAL_THREADS_1D;

  const unsigned __kernel_tb_size = TB_SIZE;
  index_type src_end;
  src_end = nowned;
  for (index_type src = 0 + tid; src < src_end; src += nthreads)
  {
    unsigned int sdist;
    index_type jj_end;
    sdist = p_dist_current[src];
    jj_end = (graph).getFirstEdge((src) + 1);
    for (index_type jj = (graph).getFirstEdge(src) + 0; jj < jj_end; jj += 1)
    {
      index_type dst;
      unsigned int new_dist;
      unsigned int old_dist;
      dst = graph.getAbsDestination(jj);
      new_dist = graph.getAbsWeight(jj) + sdist;
      old_dist = atomicMin(&p_dist_current[dst], new_dist);
      if (old_dist > new_dist)
      {
        any_retval.return_( 1);
      }
    }
  }
}
void InitializeGraph_cuda(int local_src_node, unsigned int local_infinity, struct CUDA_Context * ctx)
{
  dim3 blocks;
  dim3 threads;
  kernel_sizing(ctx->gg, blocks, threads);
  InitializeGraph <<<blocks, threads>>>(ctx->gg, ctx->nowned, local_infinity, local_src_node, ctx->dist_current.gpu_wr_ptr());
  check_cuda_kernel;
}
void SSSP_cuda(int & __retval, struct CUDA_Context * ctx)
{
  dim3 blocks;
  dim3 threads;
  kernel_sizing(ctx->gg, blocks, threads);
  *(ctx->p_retval.cpu_wr_ptr()) = __retval;
  ctx->any_retval.rv = ctx->p_retval.gpu_wr_ptr();
  SSSP <<<blocks, threads>>>(ctx->gg, ctx->nowned, ctx->dist_current.gpu_wr_ptr(), ctx->any_retval);
  check_cuda_kernel;
  __retval = *(ctx->p_retval.cpu_rd_ptr());
}