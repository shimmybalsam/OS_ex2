//
// Created by naamagl on 3/31/19.
//
#include "uthreads.h"
#include <queue>
#include <vector>
#include <list>
#include <map>
#include <iostream>
#include "thread.h"
#include <signal.h>
#include <sys/time.h>
#include <setjmp.h>
#include "sleeping_threads_list.h"

#define BLOCKED "blocked"
#define READY "ready"
#define RUNNING "running"
#define TIMEOUT "timeout"
#define TERMINATED "terminated"
#define SLEEP "sleep"
#define WAKE_UP "wake up"

using namespace std;

int cur_num_threads, quant_usecs, quantums;
static int running;
//vector<int>ids;
vector<int> ready_threads;
vector<int> blocked_threads;
map<int, thread*> threads;

struct sigaction sa_v, sa_r;
struct itimerval v_timer, r_timer;
SleepingThreadsList sleepingThreads;
timeval now;
sigset_t set;


void remove_thread(int tid);

sigjmp_buf env[MAX_THREAD_NUM];
address_t sp, pc;


typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}


/**
 * helper function, blocks all signals from interupting.
 */
void block_signals() {
    if (sigemptyset(&set) == - 1) {
        cerr << "system error: error in sigemptyset" << endl;
        exit(1);
    }
    if (sigaddset(&set, SIGALRM) == - 1) {
        cerr << "system error: error in sigaddset" << endl;
        exit(1);
    }

    if (sigaddset(&set, SIGVTALRM) == - 1) {
        cerr << "system error: error in sigaddset" << endl;
        exit(1);
    }

    if (sigprocmask(SIG_BLOCK, &set, NULL) == - 1) {
        cerr << "system error: error in sigprocmask" << endl;
        exit(1);
    }
}


/**
 * helper function, unblocks previous function, allowing signals to rise and interupt.
 */
void unblock_signals(){
    if (sigprocmask(SIG_UNBLOCK, &set, NULL) == -1){
        cerr<<"system error: error in sigprocmask"<<endl;
        exit(1);
    }
}


/**
 * helper function, resets virtual timer.
 */
void reset_v_timer(){
    v_timer.it_value.tv_sec = quant_usecs / 1000000;
    v_timer.it_value.tv_usec = quant_usecs % 1000000;

    v_timer.it_interval.tv_sec = 0;
    v_timer.it_interval.tv_usec = 0;
    if (setitimer (ITIMER_VIRTUAL, &v_timer, NULL)) {
        cerr<<"system error: timer reset error"<<endl;
        exit(1);
    }
}


/**
 * helper function, resets real timer.
 */
void reset_r_timer(timeval time_of_alarm){
//    gettimeofday(&now, nullptr);

    //first element added to empty sleepingThreads
    gettimeofday(&now, nullptr);
    timersub(&time_of_alarm, &now, &r_timer.it_value);
    if (setitimer(ITIMER_REAL, &r_timer, NULL)) {
        unblock_signals();
        cerr << "system error: error in setitimer" << endl;
        exit(1);
    }
}


/**
 * switches between last current running thread to next thread in the ready list.
 */
void switchThreads()
{
    int ret_val = sigsetjmp(env[running],1);
    if (ret_val == -1){
        cerr<<"system error: error in sigsetjmp"<<endl;
        unblock_signals();
        exit(1);
    }
    if (ret_val == 1) {
        unblock_signals();
        reset_v_timer();
        return;
    }
    running = ready_threads[0];
    ready_threads.erase(ready_threads.begin());
    quantums++;
    threads[running]->inc_quantums();
    threads[running]->set_status(RUNNING);
    reset_v_timer();
    unblock_signals();
    siglongjmp(env[running],1);
}


void scheduler(string occurrence){
    if (occurrence == TERMINATED || occurrence == BLOCKED || occurrence == SLEEP){
        if (!ready_threads.empty()) {
            switchThreads();
        }
        else{
            unblock_signals();
        }
        return;
    }
    if (occurrence == TIMEOUT){
        ready_threads.push_back(running);
        threads[running]->set_status(READY);
        switchThreads();
        return;
    }
    if (occurrence == WAKE_UP){
        gettimeofday(&now, nullptr);
        while (sleepingThreads.peek() != nullptr && timercmp(&sleepingThreads.peek()->awaken_tv, &now, <=)) {
            int woken_tid = sleepingThreads.peek()->id;
            if (!threads[woken_tid]->needs_resume()) {
                ready_threads.push_back(woken_tid);
                threads[woken_tid]->set_status(READY);

            }
            threads[woken_tid]->set_sleeping(false);
            sleepingThreads.pop();
            gettimeofday(&now, nullptr);
        }
        if (sleepingThreads.peek() != nullptr) {
            reset_r_timer(sleepingThreads.peek()->awaken_tv);
        }
        unblock_signals();
        return;
    }
}


void v_timer_handler(int sig)
{
    block_signals();
    if (sig != SIGVTALRM) {
        cerr<<"system error: wrong signal given to handler"<<endl;
        unblock_signals();
        exit(1);
    }

    scheduler(TIMEOUT);
    return;
}

void r_timer_handler(int sig)
{
    block_signals();
    if (sig != SIGALRM) {
        cerr<<"system error: wrong signal given to handler"<<endl;
        unblock_signals();
        exit(1);
    }
    scheduler(WAKE_UP);
    return;
}

/*
 * Description: This function initializes the thread library.
 * You may assume that this function is called before any other thread library
 * function, and that it is called exactly once. The input to the function is
 * the length of a quantum in micro-seconds. It is an error to call this
 * function with non-positive quantum_usecs.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs){
    if (quantum_usecs <= 0){
        cerr<<"thread library error: quantum_usecs must be positive"<<endl;
        return -1;
    }
    quant_usecs = quantum_usecs;
    quantums = 1;
    cur_num_threads = 0;
    running = 0; //main
    threads[running] = new thread(nullptr,0);
    threads[running]->inc_quantums();
    threads[running]->set_status(RUNNING);

    // Install v_timer_handler as the signal handler for SIGVTALRM.
    sa_v.sa_handler = &v_timer_handler;
    if (sigaction(SIGVTALRM, &sa_v,NULL) < 0) {
        cerr << "system error: error in sigaction" << endl;
        exit(1);
    }
    reset_v_timer();

    // Install r_timer_handler as the signal handler for SIGALRM.
    sa_r.sa_handler = &r_timer_handler;
    if (sigaction(SIGALRM, &sa_r,NULL) < 0) {
        cerr << "system error: error in sigaction" << endl;
        exit(1);
    }
    return 0;
}

/*
 * gets minimal free id to emit to new thread.
 * return minimal id if found possible, or -1 if no id is free.
 */
int get_min_id(){
    for (int i = 1; i < MAX_THREAD_NUM; i++){
        if (threads.find(i) == threads.end()){
            return i;
        }
    }
    return -1;
}
/*
 * Description: This function creates a new thread, whose entry point is the
 * function f with the signature void f(void). The thread is added to the end
 * of the READY threads list. The uthread_spawn function should fail if it
 * would cause the number of concurrent threads to exceed the limit
 * (MAX_THREAD_NUM). Each thread should be allocated with a stack of size
 * STACK_SIZE bytes.
 * Return value: On success, return the ID of the created thread.
 * On failure, return -1.
*/
int uthread_spawn(void (*f)(void)){
    block_signals();
    if (cur_num_threads >= MAX_THREAD_NUM){
        cerr<<"thread library error: reached the maximum number of threads"<<endl;
        return -1;
    }
    ++cur_num_threads;
    int tid = get_min_id();
    // what is the case of tid 0?
    if (tid <= 0){
        cerr<<"thread library error: no available tid"<<endl;
        return -1;
    }

    threads[tid] = new thread(f, tid);
    ready_threads.push_back(tid);
    sp = (address_t)threads[tid]->get_stack() + STACK_SIZE - sizeof(address_t);
    pc = (address_t)f;
    sigsetjmp(env[tid], 1);
    (env[tid]->__jmpbuf)[JB_SP] = translate_address(sp);
    (env[tid]->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&env[tid]->__saved_mask);
    unblock_signals();
    return tid;
}


/*
 * Description: This function terminates the thread with ID tid and deletes
 * it from all relevant control structures. All the resources allocated by
 * the library for this thread should be released. If no thread with ID tid
 * exists it is considered an error. Terminating the main thread
 * (tid == 0) will result in the termination of the entire process using
 * exit(0) [after releasing the assigned library memory].
 * Return value: The function returns 0 if the thread was successfully
 * terminated and -1 otherwise. If a thread terminates itself or the main
 * thread is terminated, the function does not return.
*/
int uthread_terminate(int tid){
    block_signals();
    if (tid< 0 || tid>=MAX_THREAD_NUM)
    {
        cerr<<"thread library error: the thread id is invalid (it needs to be  between 0 to 99)"<<endl;
        unblock_signals();
        return -1;
    }
    if (tid == 0){
        for (auto i = threads.begin(); i != threads.end(); i++){
            delete threads[i->first];
        }
        exit(0);
    }
    auto it = threads.find(tid);
    if (it != threads.end()){
        --cur_num_threads;
        if (tid != running){
            if (threads[tid]->get_status() == READY) {
                for (auto i = ready_threads.begin(); i != ready_threads.end(); i ++) {
                    if (*i == tid) {
                        ready_threads.erase(i);

                        break;
                    }
                }
            }
            else if(threads[tid]->get_status() == BLOCKED) {
                for (auto i = blocked_threads.begin(); i != blocked_threads.end(); i ++) {
                    if (*i == tid) {
                        blocked_threads.erase(i);
                        break;
                    }
                }
                if (threads[tid]->is_sleeping()) {
                    if (sleepingThreads.peek()->id == tid){
                        sleepingThreads.pop();
                        if (sleepingThreads.peek() != nullptr) {
                            reset_r_timer(sleepingThreads.peek()->awaken_tv);
                        }
                        else{
                            r_timer.it_value.tv_sec = 0;
                            r_timer.it_value.tv_usec = 0;
                            if (setitimer(ITIMER_REAL, &r_timer, NULL)) {
                                unblock_signals();
                                cerr << "system error: error in setitimer" << endl;
                                exit(1);
                            }
                        }
                    }
                    else{
                        sleepingThreads.erase(tid);
                    }
                }
            }
            delete threads[tid];
            threads.erase(tid);
            unblock_signals();
            return 0;
        }
        delete threads[tid];
        threads.erase(tid);
        scheduler(TERMINATED);
    }else {
        cerr<<"thread library error: tid doesn't exist"<<endl;
        unblock_signals();
        return - 1;
    }
    return 0;
}


/*
 * Description: This function blocks the thread with ID tid. The thread may
 * be resumed later using uthread_resume. If no thread with ID tid exists it
 * is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision
 * should be made. Blocking a thread in BLOCKED state has no
 * effect and is not considered an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_block(int tid){
    block_signals();
    if (tid< 0 || tid>=MAX_THREAD_NUM)
    {
        cerr<<"thread library error: the thread id is invalid (it needs to be  between 0 to 99)"<<endl;
        unblock_signals();
        return -1;
    }
    if (tid == 0){
        cerr<<"thread library error: it's illegal to block the main thread"<<endl;
        unblock_signals();
        return -1;
    }
    if (tid != running){
        auto it = threads.find(tid);
        if (it != threads.end()){
            if (threads[tid]->get_status() == BLOCKED){
                unblock_signals();
                return 0;
            }
            it->second->set_status(BLOCKED);
            blocked_threads.push_back(tid);
            for (auto i = ready_threads.begin(); i != ready_threads.end(); i ++) {
                if (*i == tid) {
                    ready_threads.erase(i);
                    break;
                }
            }
            unblock_signals();
            return 0;
        }else{
            cerr<<"thread library error: tid doesn't exist"<<endl;
            unblock_signals();
            return -1;
        }
    }
    threads[running]->set_status(BLOCKED);
    if (threads[running]->is_sleeping()){
        threads[running]->set_needs_resume(true);
    }
    blocked_threads.push_back(tid);
    scheduler(BLOCKED);
    return 0;
}

/*
 * Description: This function resumes a blocked thread with ID tid and moves
 * it to the READY state if it's not synced. Resuming a thread in a RUNNING or READY state
 * has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid){
    block_signals();
    if (tid< 0 || tid>=MAX_THREAD_NUM)
    {
        cerr<<"thread library error: the thread id is invalid (it needs to be  between 0 to 99)"<<endl;
        unblock_signals();
        return -1;
    }
    if (tid == running || tid == 0){
        unblock_signals();
        return 0;
    }
    auto it = threads.find(tid);
    if (it != threads.end()){
        if (threads[tid]->get_status() == READY){
            unblock_signals();
            return 0;
        }
        if (threads[tid]->needs_resume()){
            threads[tid]->set_needs_resume(false);
        }
        if (threads[tid]->is_sleeping()){
            unblock_signals();
            return 0;
        }
        for (auto i = blocked_threads.begin(); i != blocked_threads.end(); i ++) {
            if (*i == tid) {
                blocked_threads.erase(i);
                break;
            }
        }
        it->second->set_status(READY);
        ready_threads.push_back(tid);
        unblock_signals();
        return 0;
    }else{
        unblock_signals();
        cerr<<"thread library error: tid doesn't exist"<<endl;
        return -1;
    }
}

timeval calc_wake_up_timeval(int usecs_to_sleep, timeval &now) {

    timeval time_to_sleep, wake_up_timeval;
    time_to_sleep.tv_sec = usecs_to_sleep / 1000000;
    time_to_sleep.tv_usec = usecs_to_sleep % 1000000;
    timeradd(&now, &time_to_sleep, &wake_up_timeval);
    return wake_up_timeval;
}

/*
 * Description: This function blocks the RUNNING thread for user specified micro-seconds (virtual time).
 * It is considered an error if the main thread (tid==0) calls this function.
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision
 * should be made.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_sleep(int usec) {
    block_signals();
    if (usec < 0) {
        cerr << "thread library error: quantum_usecs must be positive" << endl;
        unblock_signals();
        return - 1;
    }
    if (running == 0) {
        cerr << "thread library error:main thread can't be in sleep mode" << endl;
        unblock_signals();
        return - 1;
    }
    if (usec == 0){
        threads[running]->set_status(READY);
        threads[running]->set_sleeping(false);
        threads[running]->set_needs_resume(false);
        ready_threads.push_back(running);

        scheduler(SLEEP);
        return 0;
    }
    threads[running]->set_status(BLOCKED);
    threads[running]->set_sleeping(true);
    threads[running]->set_needs_resume(false);

    gettimeofday(&now, nullptr);
    timeval time_of_alarm = calc_wake_up_timeval(usec, now);
    if (sleepingThreads.peek() == nullptr || timercmp(&time_of_alarm, &sleepingThreads.peek()->awaken_tv, <=)) {
        reset_r_timer(time_of_alarm);
    }
    sleepingThreads.add(running, time_of_alarm);
    scheduler(SLEEP);
    return 0;
}

/*
 * Description: This function returns the thread ID of the calling thread.
 * Return value: The ID of the calling thread.
*/
int uthread_get_tid(){
    block_signals();
    int id = threads[running]->get_tid();
    unblock_signals();
    return id;
}

/*
 * Description: This function returns the total number of quantums since
 * the library was initialized, including the current quantum.
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number
 * should be increased by 1.
 * Return value: The total number of quantums.
*/
int uthread_get_total_quantums(){
    return quantums;
}

/*
 * Description: This function returns the number of quantums the thread with
 * ID tid was in RUNNING state. On the first time a thread runs, the function
 * should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state
 * when this function is called, include also the current quantum). If no
 * thread with ID tid exists it is considered an error.
 * Return value: On success, return the number of quantums of the thread with ID tid.
 * 			     On failure, return -1.
*/
int uthread_get_quantums(int tid){
    block_signals();
    if (tid< 0 || tid>=MAX_THREAD_NUM)
    {
        cerr<<"thread library error: the thread id is invalid (it needs to be  between 0 to 99)"<<endl;
        unblock_signals();
        return -1;

    }
    auto it = threads.find(tid);
    if (it != threads.end()){
        unblock_signals();
        return threads[tid]->get_quantums(); // make sure scheduler increments if in running

    }

    else{
        cerr<<"thread library error: tid doesn't exist"<<endl;
        unblock_signals();
        return -1;
    }
}


