#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"

template<typename T>
class threadpool {
public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    // Add task to request queue
    bool append(T* request);

private:
    static void* worker(void* arg);
    // Threadpool run task: get a task from request queue and work
    void run();

private:
    // Number of threads
    int m_thread_number;
    
    // Array describing the thread pool, size is m_thread_number 
    pthread_t * m_threads;

    // Maximum number of requests allowed in the request queue and waiting to be processed
    int m_max_requests;

    // Request queue
    std::list<T*> m_workqueue;

    // Mutex
    locker m_queuelocker;

    // Semaphore to determine whether there's tasks need to be processed
    sem m_queuestat;

    // Whether to end the thread
    bool m_stop;    
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
    m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL) {
        if ((thread_number <= 0) || (max_requests <= 0)) {
            throw std::exception();
        }

        // Create threads array
        m_threads = new pthread_t[m_thread_number];
        if (!m_threads) {
            throw std::exception();
        }

        // Create thread_number threads and set them as detached threads.
        for(int i = 0; i < thread_number; ++i) {
            printf("create the %dth thread\n", i);
            // worker must be a static function in C++
            // In order for worker to access other members who are not static, pass this pointer to it as parameters
            if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
                delete []m_threads;
                throw std::exception();
            }

            if (pthread_detach(m_threads[i])) {
                delete []m_threads;
                throw std::exception();
            }
        }
}

template<typename T>
threadpool<T>::~threadpool() {
    delete []m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request) {
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // Add semaphore
    m_queuestat.post();
    return true;

}

template<typename T>
void threadpool<T>::run() {
    while(!m_stop) {
        // Whether there's a task to be processed
        m_queuestat.wait();
        m_queuelocker.lock();

        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }

        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request) {
            continue;
        }

        request->process();

    }
}


template<typename T>
void* threadpool<T>::worker(void* arg){
    // Convert this pointer 
    threadpool* pool = (threadpool*) arg;
    pool->run();
    return pool;
}

#endif