#include <iostream>
#include <random>
#include <cstddef>
#include "memory_manager.h"
#include "parallel_vector.h"

// #define MAX_TASK 5

int get_args(int argc, char** argv, int& n, int& div_num, int& seed, int& cache_size) {
    n = -1, div_num = -1, seed = 0, cache_size = DEFAULT_CACHE_SIZE;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-size") {
            if (i + 1 < argc) {
                n = atoi(argv[++i]);
            } else {
                return -1;
            }
        }

        if (std::string(argv[i]) == "-d") {
            if (i + 1 < argc) {
                div_num = atoi(argv[++i]);
            } else {
                return -1;
            }
        }

        if (std::string(argv[i]) == "-s") {
            if (i + 1 < argc) {
                seed = atoi(argv[++i]);
            } else {
                return -1;
            }
        }

        if (std::string(argv[i]) == "-cache_size" || std::string(argv[i]) == "-cs") {
            if (i + 1 < argc) {
                cache_size = atoi(argv[++i]);
            } else {
                return -1;
            }
        }
    }

    if (n == -1) {
        if (memory_manager::get_MPI_rank() == 1)
            std::cerr<<"You need to define matrixes size!"<<std::endl;
        return -1;
    }

    if (n <= 0) {
        if (memory_manager::get_MPI_rank() == 1)
            std::cerr<<"matrices size must be positive!"<<std::endl;
        return -1;
    }

    if (div_num < 0) {
        if (memory_manager::get_MPI_rank() == 1)
            std::cerr<<"number of matrices divisions must be positive!"<<std::endl;
        return -1;
    }

    if (n % div_num != 0) {
        if (memory_manager::get_MPI_rank() == 1)
            std::cerr<<"matrices size must be divided by the number divisions!"<<std::endl;
        return -1;
    }

    if (cache_size <= 0) {
        if (memory_manager::get_MPI_rank() == 1)
            std::cerr << "cache_size must be positive number!" << std::endl;
        return -1;
    }

    return 0;
}

static void show_usage() {
    if (memory_manager::get_MPI_rank() == 1)
        std::cerr << "Usage: mpiexec <-n matrices size> matrixmult_queue <-size matrices size> <-d num_of_divisions> [-s seed] [-cache_size|-cs cache_size]"<<std::endl;
}

void generate_matrices(parallel_vector<int>& pva, parallel_vector<int>& pvb, parallel_vector<int>& pvc, int n, int seed, int minn = 0, int maxx = 1000) {
    int rank = memory_manager::get_MPI_rank();
    int size = memory_manager::get_MPI_size();
    if (rank != 0) {
        int worker_rank = rank - 1;
        int worker_size = size - 1;
        int portion = (n * n)/worker_size + (worker_rank < (n * n) % worker_size?1:0);  // количество элементов на отдельном процессе
        int index = 0;
        if (worker_rank < (n * n) % worker_size) {
            index = portion * worker_rank;
        } else {
            index = (portion + 1) * ((n * n) % worker_size) + portion * (worker_rank - (n * n) % worker_size);
        }
        std::mt19937 mt(seed + rank);
        std::uniform_int_distribution<int> rand(1, maxx);
        // инициализация
        for (int i = index; i < index + portion; ++i) {
            pva.set_elem(i, rand(mt));
            pvb.set_elem(i, rand(mt));
            pvc.set_elem(i, 0);
        }
    }
}

void print_matrices(parallel_vector<int>& pva, parallel_vector<int>& pvb, parallel_vector<int>& pvc, int n) {
    int rank = memory_manager::get_MPI_rank();
    if (rank != 0) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                std::cout << pva.get_elem(n * i + j) << " ";
            }
            std::cout<< " | ";

            for (int j = 0; j < n; ++j) {
                std::cout << pvb.get_elem(n * i + j) << " ";
            }
            std::cout<< " | ";

            for (int j = 0; j < n; ++j) {
                std::cout << pvc.get_elem(n * i + j) << " ";
            }
            std::cout << "\n";
        }
    }
}

template<class T>
void matrix_mult(parallel_vector<T>& pv1, parallel_vector<T>& pv2, parallel_vector<T>& pv3, int i1, int j1, int i2, int j2, int i3, int j3, int n, int num_in_block) {
    for (int i = 0; i < num_in_block; ++i) {
        for (int j = 0; j < num_in_block; ++j) {
            int i3_teq = i3 + i;
            int j3_teq = j3 + j;
            int temp = 0;
            for (int k = 0; k < num_in_block; ++k) {
                temp += pv1.get_elem((i1 + i) * n + j1 + k) * pv2.get_elem((i2 + j) * n + j2 + k);
            }
            pv3.set_lock(pv3.get_quantum(i3_teq * n + j3_teq));
            pv3.set_elem(i3_teq * n + j3_teq, pv3.get_elem(i3_teq * n + j3_teq) + temp);
            pv3.unset_lock(pv3.get_quantum(i3_teq * n + j3_teq));
        }
    }
}

struct task {
    int a_first, a_second, b_first, b_second;
};


int main(int argc, char** argv) { // матрица b транспонирована
    memory_manager::init(argc, argv);
    int n, div_num, seed, cache_size = DEFAULT_CACHE_SIZE;
    int res = get_args(argc, argv, n, div_num, seed, cache_size);
    if (res == -1) {
        show_usage();
        memory_manager::finalize();
        return 0;
    }
    parallel_vector<int> pva(n * n, DEFAULT_QUANTUM_SIZE, cache_size), pvb(n * n, DEFAULT_QUANTUM_SIZE, cache_size), pvc (n * n, DEFAULT_QUANTUM_SIZE, cache_size);
    generate_matrices(pva, pvb, pvc, n, seed);
    int part_size = n / div_num;
    std::queue<task> qu;
    int rank = memory_manager::get_MPI_rank();
    int size = memory_manager::get_MPI_size();
    if (rank == 1) {
        for (int i = 0; i < div_num; ++i) {
            for (int j = 0; j < div_num; ++j) {
                for (int k = 0; k < div_num; ++k) {
                    qu.push({i, j, k, j});
                }
            }
        }
    }
    memory_manager::wait_all();
    if (rank != 0) {
        pva.change_mode(0, pva.get_num_quantums(), READ_ONLY);
        pvb.change_mode(0, pvb.get_num_quantums(), READ_ONLY);
    }
    // if (rank == 1)
    //     print_matrices(pva, pvb, pvc, n);
    int count = 4;
    int blocklens[] = {1, 1, 1, 1};
    MPI_Aint indices[] = {
        (MPI_Aint)offsetof(task, a_first),
        (MPI_Aint)offsetof(task, a_second),
        (MPI_Aint)offsetof(task, b_first),
        (MPI_Aint)offsetof(task, b_second)
    };
    MPI_Datatype types[] = { MPI_INT, MPI_INT, MPI_INT, MPI_INT };

    parallel_vector<task> tasks(count, blocklens, indices, types, size, 1);
    double t1 = MPI_Wtime();
    if (rank != 0) {
        if (rank == 1) {
            int count_working = 0;
            for (int i = 0; i < size - 2 && !qu.empty(); ++i) {
                tasks.set_elem(i, qu.front());
                ++count_working;
                qu.pop();
                memory_manager::notify(i + 2);
            }

            while (!qu.empty()) {
                int to_rank = memory_manager::wait();
                tasks.set_elem(to_rank - 2, qu.front());
                qu.pop();
                memory_manager::notify(to_rank);
            }

            while (count_working) {
                memory_manager::wait();
                --count_working;
            }

            for (int i = 0; i < size - 2; ++i) {
                tasks.set_elem(i, {-1, -1, -1, -1});
                memory_manager::notify(i + 2);
            }
        } else {
            while (true) {
                memory_manager::wait(1);
                task t = tasks.get_elem(rank - 2);
                if (t.a_first == -1) {
                    break;
                } else {
                    int a_first = t.a_first * part_size;
                    int a_second = t.a_second * part_size;
                    int b_first = t.b_first * part_size;
                    int b_second = t.b_second * part_size;
                    matrix_mult(pva, pvb, pvc, a_first, a_second, b_first, b_second, a_first, b_first, n, part_size);
                }
                memory_manager::notify(1);
            }
        }
    }
    memory_manager::wait_all();
    double t2 = MPI_Wtime();
    if (rank == 1) {
        // std::cout << "----------------------------------" << std::endl;
        // print_matrices(pva, pvb, pvc, n);
        std::cout << t2 - t1 << std::endl;
    }
    memory_manager::finalize();
    return 0;
}
