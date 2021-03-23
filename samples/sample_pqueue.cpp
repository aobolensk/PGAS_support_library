#include <iostream>
#include <cmath>
#include <algorithm>
#include "memory_manager.h"
#include "parallel_priority_queue.h"

int main(int argc, char** argv) {
    int num_of_quantums_proc, quantum_size;
    num_of_quantums_proc = atoi(argv[1]);
    quantum_size = atoi(argv[2]);

    if(argc < 3) {
        std::cout<<"need args!"<<std::endl;
        return 1;
    }

    memory_manager::memory_manager_init(argc, argv);
    parallel_priority_queue ppq(num_of_quantums_proc, quantum_size, 0);
    int rank = memory_manager::get_MPI_rank();
    int size = memory_manager::get_MPI_size();

    int n = num_of_quantums_proc*quantum_size*(size-1);
    for (int i = 0; i < n; i++) {
        ppq.insert(i+1);
        if(i%10 == 0) {
            if (rank == 1)
                std::cout<<"!";
            ppq.remove_max();
        }
        MPI_Barrier(MPI_COMM_WORLD);
        if(rank != 0) {
            int maxx = ppq.get_max(1);
            if(rank == 1 && i%10 == 0 && maxx%10 != 0)
                std::cout<<"ALYARMA!!!"<<std::endl;
            if (rank == 1)
                std::cout<< maxx << std::endl;
        }
    }
    if(rank != 0) {
        int maxx = ppq.get_max(1);
        if (rank == 1)
            std::cout<< maxx << std::endl;
    }

    memory_manager::finalize();
    return 0;
}