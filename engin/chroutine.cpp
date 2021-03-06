#include <unistd.h>
#include <iostream>
#include <algorithm>    // std::swap
#include "chroutine.hpp"
#include "engine.hpp"


namespace chr {

std::atomic<chroutine_id_t> chroutine_thread_t::ms_chroutine_id(0);

chroutine_t::chroutine_t(chroutine_id_t id) : me(id)
{
    SPDLOG(TRACE, "chroutine_t created: {}", me);
    stack = new char[STACK_SIZE];
    ctx = new ucontext_t;
}

chroutine_t::~chroutine_t() 
{
    SPDLOG(TRACE, "chroutine_t destroyed: {}", me);
    delete [] stack;
    delete ctx;
}

chroutine_t::chroutine_t(chroutine_t &&other)
{
    SPDLOG(DEBUG, "chroutine_t created: {} (by move constructor)", other.me);
    std::swap(ctx, other.ctx);
    func = std::move(other.func);
    arg = other.arg;
    state = other.state;
    std::swap(stack, other.stack);
    yield_wait = other.yield_wait;
    yield_to = other.yield_to;
    me = other.me;
    father = other.father;
    son = other.son;
    reporter = other.reporter;
    stop_son_when_yield_over = other.stop_son_when_yield_over;
}

int chroutine_t::wait(std::time_t now) 
{
    if (yield_wait > 0)
        return yield_wait--;
    
    if (yield_to != 0 && yield_to > now)
        return 1;

    return 0;
}

chroutine_id_t chroutine_t::yield_over(son_result_t result) 
{
    chroutine_id_t timeout_chroutine = INVALID_ID;
    if (yield_to != 0 && stop_son_when_yield_over) {
        if (reporter.get()) {
            reporter.get()->set_result(result);
        }
        timeout_chroutine = son;
        son = INVALID_ID;
        stop_son_when_yield_over = false;
    }

    yield_to = 0;
    return timeout_chroutine;
}

void chroutine_t::son_finished() 
{
    if (reporter.get()) {
        reporter.get()->set_result(result_done);
    }
    yield_to = 0;
}

reporter_base_t *chroutine_t::get_reporter() 
{
    return reporter.get();
}

std::shared_ptr<chroutine_thread_t> chroutine_thread_t::new_thread()
{
    return std::shared_ptr<chroutine_thread_t>(new chroutine_thread_t());
}

chroutine_thread_t::chroutine_thread_t()
{
    set_state(thread_state_t_init);
    clear_entry_time();
}

chroutine_thread_t::~chroutine_thread_t()
{}

void chroutine_thread_t::yield(int tick)
{
    yield_current(tick);
}

void chroutine_thread_t::wait(std::time_t wait_time_ms)
{
    wait_current(wait_time_ms, true);
}

void chroutine_thread_t::sleep(std::time_t wait_time_ms)
{
    wait_current(wait_time_ms, false);
}

chroutine_t * chroutine_thread_t::get_chroutine(chroutine_id_t id)
{
    chutex_guard_t lock(m_chroutine_lock);

    auto iter = m_schedule.chroutines_map.find(id);
    if (iter == m_schedule.chroutines_map.end()) {
        return nullptr;
    }

    return iter->second.get();
}

void chroutine_thread_t::clear_all_chroutine()
{
    chutex_guard_t lock(m_chroutine_lock);
    m_schedule.chroutines_map.clear();
    m_schedule.chroutines_to_free.clear();
    m_schedule.chroutines_sched.clear();
}

void chroutine_thread_t::remove_chroutine(chroutine_id_t id)
{
    chutex_guard_t lock(m_chroutine_lock);
    
    auto iter = m_schedule.chroutines_map.find(id);
    if (iter == m_schedule.chroutines_map.end()) {
        return;
    }
        
    m_schedule.chroutines_to_free.push_back(iter->second);
    m_schedule.chroutines_map.erase(iter);

    auto iter_list = m_schedule.chroutines_sched.begin();
    for (; iter_list != m_schedule.chroutines_sched.end(); iter_list++) {
        if ((*iter_list)->id() == id) {
            iter_list = m_schedule.chroutines_sched.erase(iter_list);
            m_schedule.sched_iter = iter_list;
            break;
        }
    }
}

reporter_base_t * chroutine_thread_t::get_current_reporter()
{
    chroutine_t *p_c = get_chroutine(m_schedule.running_id);
    if (p_c == nullptr)
        return nullptr;

    return p_c->get_reporter();
}

void chroutine_thread_t::entry(void *arg)
{
    chroutine_thread_t *p_this = static_cast<chroutine_thread_t *>(arg);
    if (p_this == nullptr)
        return;

    chroutine_t * p_c = p_this->get_chroutine(p_this->m_schedule.running_id);
    if (p_c == nullptr)
        return;

    p_c->state = chroutine_state_running;
    p_c->func(p_c->arg);
    //p_c->state = chroutine_state_fin;
    p_this->remove_chroutine(p_c->id());
    p_this->m_schedule.running_id = INVALID_ID;

    if (p_c->father != INVALID_ID) {
        chroutine_t * father = p_this->get_chroutine(p_c->father);
        if (father != nullptr) {
            father->son_finished();
        }
    }
}

chroutine_id_t chroutine_thread_t::create_chroutine(func_t & func, void *arg)
{
    if (state() > thread_state_t_running) {
        SPDLOG(ERROR, "cant create_chroutine, thread state is: {}", state());
        return INVALID_ID;
    }
    if (func == nullptr) 
        return INVALID_ID;

    chroutine_id_t id = chroutine_thread_t::gen_chroutine_id();
    std::shared_ptr<chroutine_t> c(new chroutine_t(id));
    chroutine_t *p_c = c.get();
    if (p_c == nullptr)
        return INVALID_ID;

    getcontext(p_c->ctx);
    p_c->ctx->uc_stack.ss_sp = p_c->stack;
    p_c->ctx->uc_stack.ss_size = STACK_SIZE;
    p_c->ctx->uc_stack.ss_flags = 0;
    p_c->ctx->uc_link = &(m_schedule.main);
    p_c->func = std::move(func);
    p_c->arg = arg;
    p_c->state = chroutine_state_ready;
    makecontext(p_c->ctx,(void (*)(void))(entry), 1, this);

    {
        chutex_guard_t lock(m_chroutine_lock);
        m_schedule.chroutines_map[id] = c;
        m_schedule.chroutines_sched.push_back(c);
    }

    SPDLOG(TRACE, "create_chroutine {} over, thread type: {}", id, static_cast<int>(m_type));
    return id;
}

chroutine_id_t chroutine_thread_t::create_son_chroutine(func_t & func, const reporter_sptr_t & reporter)
{
    if (state() > thread_state_t_running) {
        SPDLOG(ERROR, "cant create_son_chroutine, thread state is: {}", state());
        return INVALID_ID;
    }

    chroutine_t * pfather = get_chroutine(m_schedule.running_id);
    if (pfather == nullptr)
        return INVALID_ID;
    
    pfather->reporter = reporter;

    chroutine_id_t son = create_chroutine(func, reporter.get()->get_data());
    if (son == INVALID_ID)
        return INVALID_ID;
    
    chutex_guard_t lock(m_chroutine_lock);
    chroutine_t * pson = m_schedule.chroutines_map[son].get();
    if (pson == nullptr)
        return INVALID_ID;

    pson->father = m_schedule.running_id;
    pfather->son = son;
    return son;
}

void chroutine_thread_t::yield_current(int tick)
{
    if (tick <= 0)
        return;
        
    if (m_schedule.running_id == INVALID_ID)
        return;

    chroutine_t * co = nullptr;
    {        
        chutex_guard_t lock(m_chroutine_lock);        
        co = m_schedule.chroutines_map[m_schedule.running_id].get();
        if (co == nullptr || co->state != chroutine_state_running)
            return;
    }
        
    co->state = chroutine_state_suspend;
    co->yield_wait += tick;
    m_schedule.running_id = INVALID_ID;
    swapcontext(co->ctx, &(m_schedule.main));
}

void chroutine_thread_t::wait_current(std::time_t wait_time_ms, bool stop_son_after_wait)
{
    if (wait_time_ms <= 0)
        return;

    if (m_schedule.running_id == INVALID_ID)
        return;

    chroutine_t * co = nullptr;
    {
        chutex_guard_t lock(m_chroutine_lock);        
        co = m_schedule.chroutines_map[m_schedule.running_id].get();
        if (co == nullptr || co->state != chroutine_state_running)
            return;
    }
    
    co->state = chroutine_state_suspend;
    co->yield_to = get_time_stamp() + wait_time_ms;
    co->stop_son_when_yield_over = stop_son_after_wait;
    m_schedule.running_id = INVALID_ID;
    swapcontext(co->ctx, &(m_schedule.main));
}

bool chroutine_thread_t::done()
{
    return m_schedule.chroutines_map.empty();
}

void chroutine_thread_t::resume_to(chroutine_id_t id)
{
    chroutine_t * co = get_chroutine(id);
    if (co == nullptr || co->state != chroutine_state_suspend)
        return;
    
    swapcontext(&(m_schedule.main), co->ctx);
}

int chroutine_thread_t::pick_run_chroutine()
{
    if (m_schedule.running_id != INVALID_ID)
        return 1;

    chroutine_list_t::iterator sched_iter = m_schedule.chroutines_sched.end();
    chroutine_t *p_c = nullptr;
    int pick_count = 0;
    std::time_t now = get_time_stamp();

    {
        chutex_guard_t lock(m_chroutine_lock);
        // clean finished tasks
        m_schedule.chroutines_to_free.clear();
        if (m_schedule.chroutines_sched.empty())
            return pick_count;

        if (m_schedule.sched_iter == m_schedule.chroutines_sched.end()) {
            m_schedule.sched_iter = m_schedule.chroutines_sched.begin();
        }

        for (; m_schedule.sched_iter != m_schedule.chroutines_sched.end(); m_schedule.sched_iter++) {
            auto &node = *(m_schedule.sched_iter);
            if (node->has_moved() || node->wait(now) > 0)
                continue;
            if (p_c == nullptr) {
                p_c = node.get();
                sched_iter = m_schedule.sched_iter;
                sched_iter++;
                pick_count++;
            }
        }
    }

    m_schedule.sched_iter = sched_iter;
    if (p_c) {
        remove_chroutine(p_c->yield_over());  // remove time out son chroutin
        p_c->state = chroutine_state_running;
        m_schedule.running_id = p_c->id();
        set_entry_time();
        swapcontext(&(m_schedule.main), p_c->ctx);
        clear_entry_time();
    }
    return pick_count;
}

void chroutine_thread_t::update_thread_id()
{
    m_std_thread_id = std::this_thread::get_id();
}

int chroutine_thread_t::schedule()
{
    update_thread_id();
    set_state(thread_state_t_running);
    m_is_running = true;
    SPDLOG(INFO, "chroutine_thread_t {:p} schedule is_running {}, m_type:{} ({})", (void*)(this)
        , m_is_running
        , static_cast<int>(m_type)
        , readable_thread_id(m_std_thread_id));

    if (m_type == thread_type_t::worker) {
        engine_t::instance().on_thread_ready(m_creating_index, m_std_thread_id);
    }
    while (!m_need_stop) {        
        int processed = 0;
        processed += select_all();
        processed += pick_run_chroutine();
        m_load.update(processed);
        if (processed == 0)
            thread_ms_sleep(10);
    }
    m_is_running = false;
    set_state(thread_state_t_finished);
    clear_all_chroutine();

    SPDLOG(INFO, "chroutine_thread_t {:p} schedule is_running {}, m_type:{} ({})"
        , (void*)(this)
        , m_is_running
        , static_cast<int>(m_type)
        , readable_thread_id(m_std_thread_id));
    return 0;
}

void chroutine_thread_t::start(size_t creating_index)
{
    if (m_is_running)
        return;

    m_creating_index = creating_index;
    std::thread thrd( [this] { this->schedule(); } );
    thrd.detach();
}

void chroutine_thread_t::stop()
{
    m_need_stop = true;
    SPDLOG(INFO, "chroutine_thread_t {:p} exiting...", (void*)this);
}

int chroutine_thread_t::select_all()
{
    int processed = 0;
    for (auto iter = m_selector_list.begin(); iter != m_selector_list.end(); iter++) {
        selectable_object_it *p_obj = iter->second.get();
        if (p_obj) {
            processed += p_obj->select(0);
        }
    }
    return processed;
}

void chroutine_thread_t::register_selector(const selectable_object_sptr_t & select_obj)
{
    void *key = select_obj.get();
    if (key) {
        auto iter = m_selector_list.find(key);
        if (iter == m_selector_list.end()) {
            m_selector_list[key] = select_obj;
        } else {
        }
    }
}

void chroutine_thread_t::unregister_selector(const selectable_object_sptr_t & select_obj)
{
    unregister_selector(select_obj.get());
}

void chroutine_thread_t::unregister_selector(selectable_object_it *p_obj)
{
    void *key = p_obj;
    auto iter = m_selector_list.find(key);
    if (iter == m_selector_list.end()) {
        SPDLOG(ERROR, "{} failed: key not exist: {}", __FUNCTION__, key);
    } else {
        m_selector_list.erase(iter);
        SPDLOG(DEBUG, "{} OK: key = {}", __FUNCTION__, key);
    }
}


int chroutine_thread_t::awake_chroutine(chroutine_id_t id)
{
    chroutine_t * p_c = get_chroutine(id);
    if (p_c == nullptr) {
        SPDLOG(ERROR, "{} p_c == nullptr ! id = {}", __FUNCTION__, id);
        return -1;
    }

    remove_chroutine(p_c->yield_over(result_done));
    return 0;
}

void chroutine_thread_t::set_state(thread_state_t state) 
{
    SPDLOG(INFO, "chroutine_thread_t {:p} state change {}->{}", (void*)this, this->state(), state);
    m_state.store(state,std::memory_order_relaxed);
}

thread_state_t chroutine_thread_t::state() 
{
    return m_state.load(std::memory_order_relaxed);
}

void chroutine_thread_t::move_chroutines_to_thread(const std::shared_ptr<chroutine_thread_t> & other_thread)
{
    if (other_thread.get() == this) {
        return;
    }

    set_state(thread_state_t_shifting);
    std::vector<chroutine_id_t> ids_to_move;
    
    {
        chutex_guard_t lock(m_chroutine_lock);
        auto iter_list = m_schedule.chroutines_sched.begin();
        for (; iter_list != m_schedule.chroutines_sched.end(); iter_list++) {
            if ((*iter_list)->id() != m_schedule.running_id) {
                chroutine_id_t resettled_id = other_thread->resettle(*(iter_list->get()));
                if ((*iter_list)->id() == resettled_id) {
                    (*iter_list)->set_moved();
                    ids_to_move.push_back((*iter_list)->id());
                }
                SPDLOG(INFO, "chroutine({}) of thread:{:p} move_chroutines_to_thread {:p} with resettled_id {}"
                        , (*iter_list)->id()
                        , (void*)(this)
                        , (void*)(other_thread.get())
                        , resettled_id);
            }
        }
    }

    for (auto &i : ids_to_move) {
        remove_chroutine(i);
    }

    set_state(thread_state_t_blocking);
}

chroutine_id_t chroutine_thread_t::resettle(chroutine_t &chroutine)
{
    std::shared_ptr<chroutine_t> c(new chroutine_t(std::move(chroutine)));
    chroutine_t *p_c = c.get();
    if (p_c == nullptr)
        return INVALID_ID;
        
    p_c->ctx->uc_link = &(m_schedule.main);
    {
        chutex_guard_t lock(m_chroutine_lock);
        m_schedule.chroutines_map[p_c->id()] = c;
        m_schedule.chroutines_sched.push_back(c);
    }

    return p_c->id();
}

std::time_t chroutine_thread_t::entry_time() 
{
    return m_entry_time.load(std::memory_order_relaxed);
}

void chroutine_thread_t::set_entry_time() 
{
    m_entry_time.store(get_time_stamp(),std::memory_order_relaxed);
}

void chroutine_thread_t::clear_entry_time() 
{
    m_entry_time.store(0,std::memory_order_relaxed);
}

}