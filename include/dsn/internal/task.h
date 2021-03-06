/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
# pragma once

# include <dsn/internal/dsn_types.h>
# include <dsn/internal/extensible_object.h>
# include <dsn/internal/task_code.h>
# include <dsn/internal/error_code.h>
# include <dsn/internal/rpc_message.h>
# include <dsn/internal/end_point.h>

namespace dsn {

//----------------- common task -------------------------------------------------------

class task_worker;
class task_worker_pool;
class service_node;

class task : public ref_object, public extensible_object<task, 4>
{
public:
    task(task_code code, int hash = 0, service_node* node = nullptr);
    virtual ~task();
        
    virtual void exec() = 0;

    void                    exec_internal();    
    bool                    cancel(bool wait_until_finished);
    bool                    wait(int timeout_milliseconds = TIME_MS_MAX);
    virtual void            enqueue();
    void                    set_error_code(error_code err) { _error = err; }
    void                    set_delay(int delay_milliseconds = 0) { _delay_milliseconds = delay_milliseconds; }

    uint64_t                id() const { return _task_id; }
    task_state              state() const { return _state.load(); }
    task_code               code() const { return _spec->code; }
    task_spec&              spec() const { return *_spec; }
    int                     hash() const { return _hash; }
    int                     delay_milliseconds() const { return _delay_milliseconds; }
    error_code              error() const { return _error; }
    service_node*           node() const { return _node; }
    const char*             node_name() const;
    bool                    is_empty() const { return _is_null; }

    
    static task*            get_current_task();
    static uint64_t         get_current_task_id();
    static task_worker*     get_current_worker();
    static void             set_current_worker(task_worker* worker);

protected:
    void                    signal_waiters();
    void                    enqueue(task_worker_pool* pool);
    void                    set_task_id(uint64_t tid) { _task_id = tid;  }

    mutable std::atomic<task_state> _state;
    bool                   _is_null;

private:
    uint64_t               _task_id; 
    std::atomic<void*>     _wait_event;
    int                    _hash;
    int                    _delay_milliseconds;
    error_code             _error;
    bool                   _wait_for_cancel;
    task_spec              *_spec;
    service_node           *_node;
};

DEFINE_REF_OBJECT(task)

//----------------- timer task -------------------------------------------------------

class timer_task : public task
{
public:
    timer_task(task_code code,  uint32_t interval_milliseconds, int hash = 0);
    void exec();

    virtual bool on_timer() = 0;

private:
    uint32_t _interval_milliseconds;
};

//----------------- rpc task -------------------------------------------------------

class service_node;
class rpc_request_task : public task
{
public:
    rpc_request_task(message_ptr& request, service_node* node);

    message_ptr&  get_request() { return _request; }
    void          enqueue(service_node* node);

    virtual void  exec() = 0;

protected:
    message_ptr _request;
};

typedef ::boost::intrusive_ptr<rpc_request_task> rpc_request_task_ptr;

class rpc_server_handler
{
public:
    virtual rpc_request_task_ptr new_request_task(message_ptr& request, service_node* node) = 0;
    virtual ~rpc_server_handler(){}
};

struct rpc_handler_info
{
    task_code   code;
    std::string name;
    rpc_server_handler  *handler;

    rpc_handler_info(task_code code) : code(code) {}
    ~rpc_handler_info() { delete handler; }
};

typedef std::shared_ptr<rpc_handler_info> rpc_handler_ptr;

class rpc_response_task : public task
{
public:
    rpc_response_task(message_ptr& request, int hash = 0);

    virtual void on_response(error_code err, message_ptr& request, message_ptr& response) = 0;

    void             enqueue(error_code err, message_ptr& reply);
    virtual void     enqueue() { task::enqueue(_caller_pool); } // re-enqueue after above enqueue
    message_ptr&     get_request() { return _request; }
    message_ptr&     get_response() { return _response; }

    virtual void  exec();

private:
    message_ptr   _request;
    message_ptr   _response;
    task_worker_pool *_caller_pool;

    friend class rpc_engine;    
};

class rpc_response_task_empty : public rpc_response_task
{
public:
    rpc_response_task_empty(message_ptr& request, int hash = 0);

    virtual void on_response(error_code err, message_ptr& request, message_ptr& response) {}
};

typedef ::boost::intrusive_ptr<rpc_response_task> rpc_response_task_ptr;

//------------------------- disk AIO task ---------------------------------------------------

enum aio_type
{
    AIO_Invalid,
    AIO_Read,
    AIO_Write
};

class disk_engine;
class disk_aio
{
public:    
    // filled by apps
    handle_t     file;
    void*        buffer;
    uint32_t     buffer_size;    
    uint64_t     file_offset;

    // filled by frameworks
    aio_type     type;
    disk_engine *engine;

    disk_aio() : type(aio_type::AIO_Invalid) {}
};

typedef ::std::shared_ptr<disk_aio> disk_aio_ptr;

class aio_task : public task
{
public:
    aio_task(task_code code, int hash = 0);

    void            enqueue(error_code err, uint32_t transferred_size, service_node* node);
    uint32_t        get_transferred_size() const { return _transferred_size; }
    disk_aio_ptr    aio() { return _aio; }
    void            exec();

    virtual void on_completed(error_code err, uint32_t transferred_size) = 0;

private:
    disk_aio_ptr     _aio;
    uint32_t         _transferred_size;
};

class aio_task_empty : public aio_task
{
public:
    aio_task_empty(task_code code, int hash = 0) : aio_task(code, hash) { _is_null = true;  }
    virtual void on_completed(error_code err, uint32_t transferred_size) override {}
};

typedef ::boost::intrusive_ptr<aio_task> aio_task_ptr;

} // end namespace
