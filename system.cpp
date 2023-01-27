#include "system.hpp"

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <thread>
#include <queue>
#include <chrono>
#include <algorithm>

#include "machine.hpp"
using namespace std::chrono_literals;

class FulfillmentFailure : public std::exception
{
};

class OrderNotReadyException : public std::exception
{
};

class BadOrderException : public std::exception
{
};

class BadPagerException : public std::exception
{
};

class OrderExpiredException : public std::exception
{
};

class RestaurantClosedException : public std::exception
{
};

struct WorkerReport
{
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;
};

class CoasterPager {
public:
    void wait() const {
    wait_function_con.wait(uniq_mutex);
    }

    void wait(unsigned int timeout) const {
    auto now = std::chrono::system_clock::now();
    wait_function_con.wait_until(uniq_mutex, now + timeout * 1ms);
    }

    [[nodiscard]] unsigned int getId() const {
        return id;
    }

    [[nodiscard]] bool isReady() const {
        is_ready_mutex.lock();
        if(ready_products.size() < products.size()) {
            is_ready_mutex.unlock();
            return false;
        }
        else {
            is_ready_mutex.unlock();
            return true;
        }
    }

    friend class System;

protected:
    CoasterPager(unsigned int id, std::vector<std::unique_ptr<Product>>& ready_products,
        std::mutex& is_ready_mutex, std::vector<std::string>& products, std::condition_variable& wait_function_con, 
        std::unique_lock<std::mutex>& uniq_mutex, std::condition_variable& worker_con, unsigned int worker_id):
    id(id),
    ready_products(ready_products),
    is_ready_mutex(is_ready_mutex),
    products(products),
    wait_function_con(wait_function_con),
    uniq_mutex(uniq_mutex),
    worker_con(worker_con),
    worker_id(worker_id) {}

private:
    unsigned int worker_id;
    unsigned int id;
    std::vector<std::string>& products;
    std::mutex& is_ready_mutex; // mutex protecting concurrently checking if the products is ready
    std::condition_variable& wait_function_con; // mutex waiting for the order to be ready
    std::vector<std::unique_ptr<Product>>& ready_products;
    std::unique_lock<std::mutex>& uniq_mutex;
    std::condition_variable& worker_con; // condition variable for worker to get to know if client took his order
};

class System
{
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
    
    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout) :
        machines_map(machines),
        numberOfWorkers(numberOfWorkers),
        clientTimeout(clientTimeout),
        free_workers(numberOfWorkers),
        act_working(0),
        act_id(0) {
            std::vector<std::string> create_menu;
            for(auto& record : machines_map) create_menu.push_back(record.first);
            this->menu = create_menu;
            for(int i = 0; i < numberOfWorkers; i++) {
                workers_id.push(i);
            }
        }

    //TODO
    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
        return this->menu;
    }

    std::vector<unsigned int> getPendingOrders() const {
        return pending_orders;
    }

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {
        bool not_waited = false;

        worker_mutex.lock();
        act_id++; // order id increase
        unsigned int id = act_id;
        pending_orders.push_back(id);

        std::mutex order_waiting;
        if(!wait_for_worker.empty() || act_working >= numberOfWorkers){
            order_waiting.lock(); // if another threads are waiting for the worker
            wait_for_worker.push(order_waiting);
        } else {
            not_waited = true; // zaznacz, że nie czekałaś w kolejce, potrzebne by nie lockować ordering_mutex dwa razy
            act_working++;
            ordering_mutex.lock(); // lock tutaj by zachować dobrą kolejność gdy pare gości zamówi gdy są "wolne miejsca" (by sie nie wyprzedzali)
        }
        worker_mutex.unlock();

        order_waiting.lock(); // czekaj aż ktoś Cie wpuści albo masz miejsce by wbić

        giving_worker.lock();
        unsigned int worker_id = workers_id.front();
        workers_id.pop();
        giving_worker.unlock();

        if(!not_waited) ordering_mutex.lock();

        // creating parameters for new_pager
        std::vector<std::unique_ptr<Product>> ready_products;
        std::mutex is_ready_mutex;
        bool is_ready = false;

        orders_ready_mutex.lock();
        orders_mutexes_ready.insert(std::make_pair(id, is_ready));
        orders_ready_mutex.unlock();

        std::condition_variable wait_function_con;

        // create nedeed mutexes for Pager
        std::mutex mut;
        std::unique_lock<std::mutex> lock(mut);

        std::condition_variable worker_con; // worker condition var signalizing if client took his order

        CoasterPager new_pager(id, ready_products, is_ready_mutex, products, wait_function_con, lock, worker_con, worker_id);

        auto pager_ptr = std::make_unique<CoasterPager>(new_pager);


        pager_mutex.lock();
        pager_map.insert(std::make_pair(id, pager_ptr));
        pager_mutex.unlock();

        std::thread worker{worker_orders, products, pager_ptr};

    }

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
        orders_ready_mutex.lock();
        if(!orders_mutexes_ready[CoasterPager->getId()]) throw OrderNotReadyException();
        orders_ready_mutex.unlock();
        //TODO: reszta wyjątków


        // jeśli gotowe
        CoasterPager->worker_con.notify_one();
        return CoasterPager->ready_products;
    }

    unsigned int getClientTimeout() const {
        return this->clientTimeout;
    }
private:
    // funkcja symbolizująca workera
    void worker_orders(std::vector<std::string> products, std::unique_ptr<CoasterPager> CoasterPager) {

        // array of "future" products that will be filled during wait_for_product function
        std::unique_ptr<Product> future_products[products.size()];
        std::thread product_threads[products.size()]; // array of threads waiting for products

        for(int i = 0; i < products.size(); i++) {
            std::mutex mutex;

            machine_mutex.lock();
            if(!machines_mutexes[products[i]].empty()) mutex.lock(); // if another threads are waiting for this product
            machines_mutexes[products[i]].push(mutex);
            machine_mutex.unlock();

            product_threads[i] = std::thread{wait_for_product, products[i], mutex, std::ref(future_products[i])};
        }
        ordering_mutex.unlock(); // here ordering is finished, now we ale (concurrently) waiting for products to be done

        for(auto& threads_to_join: product_threads) {
            threads_to_join.join();
        }
        
        CoasterPager->is_ready_mutex.lock();
        for(auto& product: future_products) {
            CoasterPager->ready_products.push_back(product);
        }

        orders_ready_mutex.lock();
        // changing the boolean value in map to true which helps collectOrder() to throw an exception
        orders_mutexes_ready[CoasterPager->id] = true;
        orders_ready_mutex.unlock();

        CoasterPager->is_ready_mutex.unlock();

        CoasterPager->wait_function_con.notify_one();
        
        auto now = std::chrono::system_clock::now();
        // creating unique_mutex needed for wait_until
        std::mutex mut;
        std::unique_lock<std::mutex> lock(mut);

        CoasterPager->worker_con.wait_until(lock, now + clientTimeout * 1ms); // czekanie na sygnał od klienta który odebrał zamówienie, albo na Timeout

        // TODO: jeśli poprawnie odebrane zamówienie
        workers_report[CoasterPager->worker_id].collectedOrders.push_back(CoasterPager->products);
        // TODO: jeśli niepoprawnie odebrane to do innych rzeczy trzeba dodać 
        
        std::remove(pending_orders.begin(), pending_orders.end(), CoasterPager->id);

        // freeing next worker if any order is waiting
        giving_worker.lock();
        worker_mutex.lock();
        workers_id.emplace(CoasterPager->worker_id); // dodanie pracownika jako, że już może pracować

        if(!wait_for_worker.empty()) {
            wait_for_worker.front().unlock();
            wait_for_worker.pop(); // wyrzucanie z kolejki gościa którego wpuściliśmy
        }
        else {
            act_working--;
        }
        worker_mutex.unlock();
        giving_worker.unlock();
    }

    void wait_for_product(std::string& product, std::mutex& mutex, std::unique_ptr<Product> future_product) {
        mutex.lock();
        future_product = machines_map[product]->getProduct();
        
        machine_mutex.lock();
        machines_mutexes[product].pop();
        if(!machines_mutexes[product].empty()) machines_mutexes[product].front().unlock();
        machine_mutex.unlock();
    }

    unsigned int act_working; // number of working workers at the moment
    unsigned int act_id;
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;
    std::queue<std::thread> workers;
    unsigned int free_workers;
    std::mutex wait_for_any_worker;
    machines_t machines_map;
    std::vector<std::string> menu;

    std::mutex giving_worker;
    std::queue<unsigned int> workers_id;
    std::vector<WorkerReport> workers_report;
    std::vector<unsigned int> pending_orders;

    std::mutex pager_mutex; // mutex for protecting operations on pager_map
    std::unordered_map<unsigned int, std::unique_ptr<CoasterPager>> pager_map; // map that keeps (order_number, pager pointer)

    std::mutex orders_ready_mutex; // mutex protecting orders_mutexes_ready map
    // map with order_id, mutex and boolean if the order is ready
    std::unordered_map<unsigned int, bool> orders_mutexes_ready;
    std::mutex worker_mutex; // mutex protecting concurrent changes in wait_for_worker queue;
    std::queue<std::mutex> wait_for_worker; // queue in which orders are waiting for workers when all of them are working
    std::mutex ordering_mutex; // mutex protectining from interrupting creating "waiting_orders" by one worker;
    std::mutex machine_mutex; // mutex protecting concurrent changes in machines_mutexes map;
    std::unordered_map<std::string, std::queue<std::mutex>> machines_mutexes;
};