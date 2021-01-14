//
// Created by naamagl on 3/31/19.
//

#ifndef OS_EX2_THREAD_H
#define OS_EX2_THREAD_H

#include <string>

#define STACK_SIZE 4096
using namespace std;

typedef unsigned long address_t;

class thread {
private:
//    address_t _sp, _pc;
    int _tid;
    string _status = "ready"; // do we do or by signaling?
    int _quantums = 0;
    bool _sleeping = false;
    bool _needs_resume = false;
    char* _stack;

public:

    // remember to implement default rule of 3

    thread() = default;

    thread(void (*f)(void), int tid){
        _tid = tid;
        _stack = new char[STACK_SIZE];
    }

    ~thread()
    {
        delete[] _stack;
    }

    int get_tid(){return _tid;}
    void set_status(string status){
        _status = status;
    }
    string get_status(){return _status;}

    int get_quantums(){return _quantums;}

    void inc_quantums(){++_quantums;}

    char* get_stack(){return _stack;}

    bool is_sleeping(){return _sleeping;}

    bool needs_resume(){return _needs_resume;}

    void set_sleeping(bool mode){_sleeping = mode;}

    void set_needs_resume(bool mode){_needs_resume = mode;}
};


#endif //OS_EX2_THREAD_H
