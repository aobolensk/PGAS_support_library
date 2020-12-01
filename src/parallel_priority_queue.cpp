#include "parallel_priority_queue.h"
#include "parallel_reduce.h"
#include <algorithm>
#include <climits>

parallel_priority_queue::parallel_priority_queue(int _num_of_quantums_proc, int _quantum_size) {
    worker_rank = memory_manager::get_MPI_rank()-1;
    worker_size = memory_manager::get_MPI_size()-1;
    num_of_quantums_proc = _num_of_quantums_proc;
    quantum_size = _quantum_size;
    num_of_elems_proc = num_of_quantums_proc*quantum_size;
    global_index_l = worker_rank*num_of_elems_proc;
    maxes = parallel_vector(worker_size, 1);
    sizes = parallel_vector(worker_size, 1);
    maxes.set_elem(worker_rank, 0);  // 0???
    sizes.set_elem(worker_rank, 0);  // 0???
    pqueues = parallel_vector(2*worker_size*num_of_elems_proc, quantum_size);
    for(int i = global_index_l; i < global_index_l + num_of_elems_proc; i++)
        sizes.set_elem(i, 0);  // 0???
}

void parallel_priority_queue::insert(int elem) {  // need to be called by all worker_processes
    // TODO: check rank w/ min size of priority_queue through parallel_reduce
    // need to update parallel_reduce
    int id_min = 0;
    int minn = INT_MAX;
    for (int i = 0; i < worker_size; i++) {
        int size_i = sizes.get_elem(i);
        if(size_i < minn) {
            id_min = i;
            minn = size_i;
        }
    }
    if(worker_rank == id_min)
        insert_internal(elem);
}

void parallel_priority_queue::insert_internal(int elem) {
    int local_index = sizes.get_elem(worker_rank);
    local_index += local_index-1;
    pqueues.set_elem(global_index_l + local_index, elem);
    sizes.set_elem(worker_rank, sizes.get_elem(worker_rank) + 1);
    for (int i = local_index; i > 0; i = i >> 1) {
        int elem = pqueues.get_elem(global_index_l + i);
        int elem2 = pqueues.get_elem(global_index_l + (i>>1));
        if(elem2 > elem) {
            pqueues.set_elem(global_index_l+i, elem2);
            pqueues.set_elem(global_index_l+(i>>1), elem);
            if (i == 1) {
                maxes.set_elem(global_index_l, elem);
            }
        } else {
            break;
        }
    }
}

class Func {
    parallel_vector* a;
public:
    Func(parallel_vector& pv) {
        a = &pv;
    }
    int operator()(int l, int r, int identity) const {
        return a->get_elem(l);
    }
};

int parallel_priority_queue::get_max() {
    auto reduction = [](int a, int b){return std::max(a, b);};
    return parallel_reduce(worker_rank, worker_rank+1, maxes, INT_MIN, 1, worker_size /*global_size*/, Func(maxes), reduction, worker_rank + 1 /*global_rank*/);
}