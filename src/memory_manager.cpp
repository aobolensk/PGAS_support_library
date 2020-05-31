#include <thread>
#include <mpi.h>
#include <vector>
#include <climits>
#include <iostream>
#include <cassert>
#include <mutex>
#include "memory_manager.h"

// посылка мастеру: [операция; идентификатор структуры, откуда требуются данные; требуемый номер кванта]
// посылка рабочему от мастера: [операция; идентификатор структуры, откуда требуются данные; 
//                               требуемый номер кванта; номер процесса, которому требуется передать квант]
// если номер структуры = -1, то завершение функций worker_helper_thread и master_helper_thread
memory_manager mm;

void memory_manager::memory_manager_init(int argc, char**argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if(provided != MPI_THREAD_MULTIPLE) {
            abort();
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    worker_rank = rank - 1;
    worker_size = size - 1;
    is_read_only_mode = false;  // по умолчанию включен READ_WRITE режим
    num_of_change_mode_procs = 0;
    num_of_change_mode = 0;
    times.resize(size, LLONG_MIN);
    time = LLONG_MIN;
    if(rank == 0) {
        helper_thr = std::thread(master_helper_thread);
    } else {
        helper_thr = std::thread(worker_helper_thread);
    }
    // создание своего типа для пересылки посылок ???
}

int memory_manager::get_MPI_rank() {
    return rank;
}

int memory_manager::get_MPI_size() {
    return size;
}

int memory_manager::create_object(int number_of_elements) {
    memory_line line;
    line.logical_size = number_of_elements;
    int num_of_quantums = (number_of_elements + QUANTUM_SIZE - 1) / QUANTUM_SIZE;
    if (rank == 0) {
        line.quantums_for_lock.resize(num_of_quantums);
        line.quantum_owner.resize(num_of_quantums);
        line.owners.resize(num_of_quantums);
        for (int i = 0; i < int(line.quantums_for_lock.size()); i++) {
            line.quantums_for_lock[i] = -1;
            line.quantum_owner[i] = {0, -1};
            line.owners[i] = std::vector<int>();
        }
    } else {
        line.mutexes.resize(num_of_quantums);
        line.quantums.resize(num_of_quantums);
        for (int i = 0; i < num_of_quantums; i++) {
            line.quantums[i] = nullptr;
            line.mutexes[i] = new std::mutex();
        }
    }
    line.num_change_mode.resize(num_of_quantums, 0);

    memory.emplace_back(line);
    MPI_Barrier(MPI_COMM_WORLD);
    return memory.size()-1;
}

int memory_manager::get_data(int key, int index_of_element) {
    int num_quantum = get_quantum_index(index_of_element);
    auto& quantum = mm.memory[key].quantums[num_quantum];
    bool f = false;
    if (!is_read_only_mode)   // если read_write mode, то используем мьютекс на данный квант
        mm.memory[key].mutexes[num_quantum]->lock();
    if (quantum != nullptr) {  // на данном процессе есть квант?
        if (mm.memory[key].num_change_mode[num_quantum] == mm.num_of_change_mode) {  // не было изменения режима? (данные актуальны?)
            int elem = quantum[index_of_element%QUANTUM_SIZE];
            if (!is_read_only_mode)
                mm.memory[key].mutexes[num_quantum]->unlock();
            return elem;  // элемент возвращается без обращения к мастеру
        }
    } else {
        quantum = new int[QUANTUM_SIZE];  // выделение памяти
    }
    int request[3] = {GET_INFO, key, num_quantum};  // обращение к мастеру с целью получить квант
    MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);
    int to_rank = -2;
    MPI_Status status;
    MPI_Recv(&to_rank, 1, MPI_INT, 0, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD, &status);  // получение ответа от мастера
    mm.memory[key].num_change_mode[num_quantum] = mm.num_of_change_mode;
    if (is_read_only_mode && to_rank == rank) {  // если read_only_mode и данные уже у процесса,
                                                 // ответ мастеру о том, что данные готовы, отправлять не нужно
        return quantum[index_of_element%QUANTUM_SIZE];
    }
    if (to_rank != rank) {  // если данные не у текущего процесса, иницируется передача данных от указанного мастером процесса
        assert(quantum != nullptr);
        assert(to_rank > 0 && to_rank < size);
        MPI_Recv(quantum, QUANTUM_SIZE, MPI_INT, to_rank, GET_DATA_FROM_HELPER, MPI_COMM_WORLD, &status);
    }
    request[0] = SET_INFO;
    MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);  // уведомление мастера о том, что данные готовы для передачи другим процессам
    int elem = quantum[index_of_element%QUANTUM_SIZE];
    if (!is_read_only_mode)
        mm.memory[key].mutexes[num_quantum]->unlock();
    return elem;
}

void memory_manager::set_data(int key, int index_of_element, int value) {
    assert(key >= 0 && key < (int)mm.memory.size());
    if(is_read_only_mode) {
        throw -1;  // запись в READ_ONLY режиме запрещена
    }
    int num_quantum = get_quantum_index(index_of_element);
    assert(index_of_element >= 0 && index_of_element < (int)mm.memory[key].logical_size);
    assert(num_quantum >= 0 && num_quantum < (int)mm.memory[key].quantums.size());
    auto& quantum = mm.memory[key].quantums[num_quantum];
    assert(num_quantum >= 0 && num_quantum < (int)mm.memory[key].mutexes.size());
    mm.memory[key].mutexes[num_quantum]->lock();
    assert((index_of_element%QUANTUM_SIZE) >= 0);
    if (quantum != nullptr) {
        if (mm.memory[key].num_change_mode[num_quantum] == mm.num_of_change_mode) {
            quantum[index_of_element%QUANTUM_SIZE] = value;
            mm.memory[key].mutexes[num_quantum]->unlock();
            return;
        }
    } else {
        quantum = new int[QUANTUM_SIZE];
    }
    int request[3] = {GET_INFO, key, num_quantum};
    MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);  // обращение к мастеру с целью получить квант
    int to_rank = -2;
    MPI_Status status;
    MPI_Recv(&to_rank, 1, MPI_INT, 0, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD, &status);  // получение ответа от мастера
    assert(num_quantum >= 0 && num_quantum < (int)mm.memory[key].num_change_mode.size());
    mm.memory[key].num_change_mode[num_quantum] = mm.num_of_change_mode;
    if (to_rank != rank) {  // если данные не у текущего процесса, иницируется передача данных от указанного мастером процесса
        assert(quantum != nullptr);
        assert(to_rank > 0 && to_rank < size);
        MPI_Recv(quantum, QUANTUM_SIZE, MPI_INT, to_rank, GET_DATA_FROM_HELPER, MPI_COMM_WORLD, &status);
    }
    request[0] = SET_INFO;
    assert(quantum != nullptr);
    quantum[index_of_element%QUANTUM_SIZE] = value;
    MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);  // уведомление мастера о том, что данные готовы для передачи другим процессам
    mm.memory[key].mutexes[num_quantum]->unlock();
}

int memory_manager::get_quantum_index(int index) {
    return index/QUANTUM_SIZE;
}


void worker_helper_thread() {
    int request[4] = {-2, -2, -2, -2};
    MPI_Status status;
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    while(true) {
        MPI_Recv(request, 4, MPI_INT, MPI_ANY_SOURCE, SEND_DATA_TO_HELPER, MPI_COMM_WORLD, &status);
        if(request[0] == -1 && request[1] == -1 && request[2] == -1 && request[3] == -1) {  // окончание работы вспомогательного потока
            // освобождение памяти
            for (int key = 0; key < int(mm.memory.size()); key++) {
                for(int i = 0; i < int(mm.memory[key].quantums.size()); i++) {
                    if(mm.memory[key].quantums[i] != nullptr) {
                        delete[] mm.memory[key].quantums[i];
                        mm.memory[key].quantums[i] = nullptr;
                    }
                    delete mm.memory[key].mutexes[i];
                }
            }
            break;
        }
        int key = request[1], quantum_index = request[2], to_rank = request[3];
        assert(mm.memory[key].quantums[quantum_index] != nullptr);
        assert(to_rank > 0 && to_rank < size);
        assert(key >= 0 && key < (int)mm.memory.size());
        assert(quantum_index >= 0 && quantum_index < (int)mm.memory[key].quantums.size());
        // запросы на GET_DATA_R и GET_DATA_RW принимаются только от мастера
        switch(request[0]) {
            case GET_DATA_R:  // READ_ONLY режим, запись запрещена, блокировка мьютекса для данного кванта не нужна
                MPI_Send(mm.memory[key].quantums[quantum_index], QUANTUM_SIZE,
                                        MPI_INT, to_rank, GET_DATA_FROM_HELPER, MPI_COMM_WORLD);
                break;
            case GET_DATA_RW:  // READ_WRITE режим
                assert(quantum_index >= 0 && quantum_index < (int)mm.memory[key].mutexes.size());
                mm.memory[key].mutexes[quantum_index]->lock();
                MPI_Send(mm.memory[key].quantums[quantum_index], QUANTUM_SIZE,
                                        MPI_INT, to_rank, GET_DATA_FROM_HELPER, MPI_COMM_WORLD);
                delete[] mm.memory[key].quantums[quantum_index];  // после отправки данных в READ_WRITE режиме квант на данном процессе удаляется
                mm.memory[key].quantums[quantum_index] = nullptr;
                mm.memory[key].mutexes[quantum_index]->unlock();
                break;
        }
    }
}

void master_helper_thread() {
    int request[3] = {-2, -2, -2};
    MPI_Status status;
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    while(true) {
        MPI_Recv(&request, 3, MPI_INT, MPI_ANY_SOURCE, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD, &status);
        if (request[0] == -1 && request[1] == -1 && request[2] == -1) {  // окончание работы вспомогательного потока
            break;
        }
        int key = request[1], quantum = request[2];
        if (request[0] != CHANGE_MODE) {
            assert(key >= 0 && key < (int)mm.memory.size());
            assert(quantum >= 0 && quantum < (int)mm.memory[key].quantum_owner.size());
        }
        switch(request[0]) {
            case LOCK:  // блокировка кванта
                if (mm.memory[key].quantums_for_lock[quantum] == -1) {  // квант не заблокирован
                    int to_rank = status.MPI_SOURCE;
                    int tmp = 1;
                    mm.memory[key].quantums_for_lock[quantum] = status.MPI_SOURCE;
                    MPI_Send(&tmp, 1, MPI_INT, to_rank, GET_DATA_FROM_MASTER_HELPER_LOCK, MPI_COMM_WORLD);  // уведомление о том, 
                                                                                                            // что процесс может заблокировать квант
                } else {  // квант уже заблокирован другим процессом, данный процесс помещается в очередь ожидания по данному кванту
                    if (mm.memory[key].wait_locks.find(quantum) == mm.memory[key].wait_locks.end())
                        mm.memory[key].wait_locks.insert({quantum, std::queue<int>{}});
                    mm.memory[key].wait_locks[quantum].push(status.MPI_SOURCE);
                }
                break;
            case UNLOCK:  // разблокировка кванта
                if (mm.memory[key].quantums_for_lock[quantum] == status.MPI_SOURCE) {
                    mm.memory[key].quantums_for_lock[quantum] = -1;
                    if (mm.memory[key].wait_locks.find(quantum) != mm.memory[key].wait_locks.end()) {  // проверка, есть ли в очереди
                                                                                                       // ожидания по данному кванту какой-либо процесс
                        int to_rank = mm.memory[key].wait_locks[quantum].front();
                        mm.memory[key].wait_locks[quantum].pop();  // если есть, то ожидающий процесс удаляется из очереди
                        mm.memory[key].quantums_for_lock[quantum] = to_rank;
                        if (mm.memory[key].wait_locks[quantum].size() == 0)
                            mm.memory[key].wait_locks.erase(quantum);
                        int tmp = 1;
                        MPI_Send(&tmp, 1, MPI_INT, to_rank, GET_DATA_FROM_MASTER_HELPER_LOCK, MPI_COMM_WORLD);  // уведомление о том, что процесс, изъятый
                                                                                                                // из очереди, может заблокировать квант
                    }
                }
                break;
            case GET_INFO:  // получить квант
                if (mm.is_read_only_mode) {  // READ_ONLY mode?
                    assert(quantum < (int)mm.memory[key].num_change_mode.size());
                    assert(quantum < (int)mm.memory[key].quantum_owner.size());
                    assert(quantum < (int)mm.memory[key].owners.size());
                    if (mm.memory[key].num_change_mode[quantum] != mm.num_of_change_mode) {  // был переход между режимами?
                        if (mm.memory[key].quantum_owner[quantum].second == -1)
                            throw -1;
                        assert(mm.memory[key].quantum_owner[quantum].first == true);
                        mm.memory[key].owners[quantum].clear();
                        mm.memory[key].owners[quantum].push_back(mm.memory[key].quantum_owner[quantum].second);
                        mm.memory[key].num_change_mode[quantum] = mm.num_of_change_mode;
                        int to_rank = mm.memory[key].owners[quantum][0];
                        if (to_rank == status.MPI_SOURCE) {  // после перехода оказалось, что квант находится на процессе, который отправил запрос?
                            MPI_Send(&to_rank, 1, MPI_INT, status.MPI_SOURCE, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);
                            break;
                        }
                    }
                    if (mm.memory[key].owners[quantum].empty()) {  // квант не был иницииализирован
                            throw -1;
                    }
                    int to_rank = -2;
                    long long minn = mm.time+1;
                    for (int owner: mm.memory[key].owners[quantum]) {  // поиск процесса, с которым мастер не взаимодействовал наиболее долгое время
                        assert(owner < (int)mm.times.size());
                        if (owner == status.MPI_SOURCE) {  // данные есть также и у процесса, который отправил запрос?
                            to_rank = owner;
                            break;
                        }
                        if (mm.times[owner] < minn) {
                            to_rank = owner;
                            minn = mm.times[owner];
                        }
                    }
                    assert(to_rank > 0 && to_rank < size);
                    mm.times[to_rank] = mm.time;
                    mm.times[status.MPI_SOURCE] = mm.time++;
                    int to_request[4] = {GET_DATA_R, key, quantum, status.MPI_SOURCE};
                    MPI_Send(&to_rank, 1, MPI_INT, status.MPI_SOURCE, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);  // отправление информации о том, с каким процессом
                                                                                                                     // нужно взаимодействовать для получения кванта
                    if(to_rank != status.MPI_SOURCE) {
                        MPI_Send(to_request, 4, MPI_INT, to_rank, SEND_DATA_TO_HELPER, MPI_COMM_WORLD);  // отправление запроса вспомогательному потоку
                                                                                                         // процесса-рабочего о переслыке данных
                    } else {  // в ходе поиска было обнаружено, что
                              // данные уже есть у процесса
                        mm.memory[key].owners[quantum].push_back(to_rank);
                    }
                } else {  // READ_WRITE mode?
                    assert(quantum < (int)mm.memory[key].num_change_mode.size());
                    if (mm.memory[key].num_change_mode[quantum] != mm.num_of_change_mode) {  // был переход между режимами?
                        long long minn = mm.time+1;
                        int to_rank = -1;
                        for (int owner: mm.memory[key].owners[quantum]) {  // поиск процесса, с которым мастер не взаимодействовал наиболее долгое время
                            assert(owner < (int)mm.times.size());
                            if (owner == status.MPI_SOURCE) {  // данные есть также и у процесса, который отправил запрос?
                                to_rank = owner;
                                break;
                            }
                            if (mm.times[owner] < minn) {
                                to_rank = owner;
                                minn = mm.times[owner];
                            }
                        }
                        mm.memory[key].num_change_mode[quantum] = mm.num_of_change_mode;
                        mm.memory[key].quantum_owner[quantum] = {false, status.MPI_SOURCE};
                        if (to_rank == -1 || to_rank == status.MPI_SOURCE) {  // данные у процесса, отправившего запрос?
                            MPI_Send(&status.MPI_SOURCE, 1, MPI_INT, status.MPI_SOURCE, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);
                        } else {
                            int to_request[4] = {GET_DATA_RW, key, quantum, status.MPI_SOURCE};
                            MPI_Send(&to_rank, 1, MPI_INT, status.MPI_SOURCE, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);  // отправление информации о том, с каким процессом
                                                                                                                             // нужно взаимодействовать для получения кванта
                            MPI_Send(to_request, 4, MPI_INT, to_rank, SEND_DATA_TO_HELPER, MPI_COMM_WORLD);  // отправление запроса вспомогательному потоку
                                                                                                             // процесса-рабочего о переслыке данных
                        }
                        break;
                    }
                    assert(quantum < (int)mm.memory[key].quantum_owner.size());
                    assert(mm.memory[key].quantum_owner[quantum].second != status.MPI_SOURCE);
                    if (mm.memory[key].quantum_owner[quantum].second == -1) {  // данные ранее не запрашивались?
                        mm.memory[key].quantum_owner[quantum] = {false, status.MPI_SOURCE};
                        MPI_Send(&status.MPI_SOURCE, 1, MPI_INT, status.MPI_SOURCE, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);  // отправление информации о том, что процесс,
                                                                                                                                   // отправивший запрос, может забрать квант без
                                                                                                                                   // пересылок данных
                        break;
                    }  // empty
                    if (mm.memory[key].quantum_owner[quantum].first) {  // данные готовы к пересылке?
                        int to_rank = mm.memory[key].quantum_owner[quantum].second;
                        int to_request[4] = {GET_DATA_RW, key, quantum, status.MPI_SOURCE};
                        mm.memory[key].quantum_owner[quantum] = {false, status.MPI_SOURCE};
                        MPI_Send(&to_rank, 1, MPI_INT, status.MPI_SOURCE, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);  // отправление информации о том, с каким процессом
                                                                                                                         // нужно взаимодействовать для получения кванта
                        MPI_Send(to_request, 4, MPI_INT, to_rank, SEND_DATA_TO_HELPER, MPI_COMM_WORLD);  // отправление запроса вспомогательному потоку
                                                                                                         // процесса-рабочего о переслыке данных
                    } else {  // данные не готовы к пересылке (в данный момент пересылаются другому процессу)
                        if (mm.memory[key].wait_quantums.find(quantum) == mm.memory[key].wait_quantums.end())
                            mm.memory[key].wait_quantums.insert({quantum, std::queue<int>()});
                        mm.memory[key].wait_quantums[quantum].push(status.MPI_SOURCE);  // процесс помещается в очередь ожидания по данному кванту
                    }
                }
                break;
            case SET_INFO:  // данные готовы для пересылки
                if (mm.is_read_only_mode) {  // READ_ONLY mode
                    mm.memory[key].owners[quantum].push_back(status.MPI_SOURCE); // процесс помещается в вектор процессов,
                                                                                 // которые могут пересылать данный квант другим процессам
                } else {  // READ_WRITE mode
                    assert(mm.memory[key].quantum_owner[quantum].second == status.MPI_SOURCE);
                    assert(mm.memory[key].quantum_owner[quantum].first == false);
                    mm.memory[key].quantum_owner[quantum].first = true;
                    if (mm.memory[key].wait_quantums.find(quantum) != mm.memory[key].wait_quantums.end()) {  // есть процессы, ожидающие готовности кванта?
                        std::queue<int>& wait_queue = mm.memory[key].wait_quantums[quantum];
                        int source_rank = wait_queue.front();
                        wait_queue.pop();  // ожидающий процесс извлекается из очереди
                        if (wait_queue.size() == 0) {
                            mm.memory[key].wait_quantums.erase(quantum);
                            assert(mm.memory[key].wait_quantums.find(quantum) == mm.memory[key].wait_quantums.end());
                        }
                        int to_rank = mm.memory[key].quantum_owner[quantum].second;
                        mm.memory[key].quantum_owner[quantum] = {false, source_rank};
                        int to_request[4] = {GET_DATA_RW, key, quantum, source_rank};
                        assert(source_rank != to_rank);
                        assert(to_rank > 0 && to_rank < size);
                        MPI_Send(&to_rank, 1, MPI_INT, source_rank, GET_INFO_FROM_MASTER_HELPER, MPI_COMM_WORLD);  // отправление информации о том, с каким процессом
                                                                                                                   // нужно взаимодействовать для получения кванта
                        MPI_Send(to_request, 4, MPI_INT, to_rank, SEND_DATA_TO_HELPER, MPI_COMM_WORLD);  // отправление запроса вспомогательному потоку
                                                                                                         // процесса-рабочего о переслыке данных
                    }
                }
                break;
            case CHANGE_MODE:  // изменить режим работы с памятью
                mm.num_of_change_mode_procs++;
                if (mm.num_of_change_mode_procs == mm.worker_size) {  // все процессы дошли до этапа изменения режима?
                    mm.time = 0;
                    mm.is_read_only_mode = request[1];
                    int ready = 1;
                    for (int i = 1; i < size; i++) {
                        MPI_Send(&ready, 1, MPI_INT, i, GET_PERMISSION_FOR_CHANGE_MODE, MPI_COMM_WORLD);  // информирование о смене режима и о том, что 
                                                                                                          // другие процессы могут продолжить выполнение программы дальше
                        mm.times[i] = 0;
                    }
                    mm.num_of_change_mode_procs = 0;
                    mm.num_of_change_mode++;
                }
                break;
        }
    }
}

void memory_manager::set_lock(int key, int quantum_index) {
    int request[] = {LOCK, key, quantum_index};
    MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);  // отправление мастеру запроса о блокировке кванта
    int ans;
    MPI_Status status;
    MPI_Recv(&ans, 1, MPI_INT, 0, GET_DATA_FROM_MASTER_HELPER_LOCK, MPI_COMM_WORLD, &status);  // квант заблокирован
}

void memory_manager::unset_lock(int key, int quantum_index) {
    int request[3] = {UNLOCK, key, quantum_index};
    MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);  // отправление мастеру запроса о разблокировке кванта
}

void memory_manager::change_mode(int mode) {
    if (mode == READ_ONLY && is_read_only_mode ||
        mode == READ_WRITE && !is_read_only_mode)
        return;
    // информирование мастера о том, что данный процесс дошёл до этапа изменения режима работы с памятью
    if (mode == READ_ONLY) {
        int request[3] = {CHANGE_MODE, 1, -1};
        MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);
    } else if (mode == READ_WRITE) {
        int request[3] = {CHANGE_MODE, 0, -1};
        MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);
    }
    int is_ready;
    MPI_Status status;
    MPI_Recv(&is_ready, 1, MPI_INT, 0, GET_PERMISSION_FOR_CHANGE_MODE, MPI_COMM_WORLD, &status);  // после получения ответа данный процесс может продолжить выполнение
    if (mode == READ_ONLY)
        is_read_only_mode = true;
    else if (mode == READ_WRITE)
        is_read_only_mode = false;
    mm.num_of_change_mode++;
}

void memory_manager::finalize() {
    int cnt = 0;
    if(rank != 0) {
        int tmp = 1;
        MPI_Send(&tmp, 1, MPI_INT, 0, 23221, MPI_COMM_WORLD);
        MPI_Status status;
        MPI_Recv(&tmp, 1, MPI_INT, 0, 12455, MPI_COMM_WORLD, &status);
    } else {
        int tmp;
        for(int i = 1; i < size; i++) {
            MPI_Status status;
            MPI_Recv(&tmp, 1, MPI_INT, i, 23221, MPI_COMM_WORLD, &status);
        }
        for(int i = 1; i < size; i++) {
            MPI_Send(&tmp, 1, MPI_INT, i, 12455, MPI_COMM_WORLD);
        }
    }
    if (rank == 0) {
        int request[4] = {-1, -1, -1, -1};
        for(int i = 1; i < size; i++) {
            MPI_Send(request, 4, MPI_INT, i, SEND_DATA_TO_HELPER, MPI_COMM_WORLD);  // завершение работы вспомогательных потоков процессов рабочих
        }
    } else if (rank == 1) {
        int request[3] = {-1, -1, -1};
        MPI_Send(request, 3, MPI_INT, 0, SEND_DATA_TO_MASTER_HELPER, MPI_COMM_WORLD);  // завершение работы вспомогательного потока процесса-мастера
    }
    assert(helper_thr.joinable());
    helper_thr.join();
    if (rank != 0) {
        for(int key = 0; key < (int)memory.size(); key++) {
            assert(memory[key].wait_quantums.size() == 0);
            if(!mm.is_read_only_mode) {
                for(int quantum = 0; quantum < (int)memory[key].quantum_owner.size(); quantum++) {
                    assert(memory[key].quantum_owner[quantum].first == true);
                }
            }
            for(int i = 0; i < (int)mm.memory[key].quantums.size(); i++) {
                assert(memory[key].quantums[i] == nullptr);
            }
        }
    }
    MPI_Finalize();
}