#include "system.hpp"

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <thread>
#include <queue>
#include <chrono>

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

class CoasterPager
{
public:
    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const;

    [[nodiscard]] bool isReady() const;
};

class MyCoasterPager: public CoasterPager {
public:
    MyCoasterPager(unsigned int id, std::vector<std::unique_ptr<Product>>& ready_products,
    std::mutex& is_ready_mutex, unsigned int products_num, std::condition_variable& wait_function_con, std::unique_lock<std::mutex>& uniq_mutex):
    id(id),
    ready_products(ready_products),
    is_ready_mutex(is_ready_mutex),
    products_num(products_num),
    wait_function_con(wait_function_con),
    uniq_mutex(uniq_mutex) {}

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
        if(ready_products.size() < products_num) {
            return false;
        }
        else return true;
    }

private:
    unsigned int id;
    unsigned int products_num;
    std::mutex& is_ready_mutex; // mutex protecting concurrently checking if the products is ready
    std::condition_variable& wait_function_con; // mutex waiting for the order to be ready
    std::vector<std::unique_ptr<Product>>& ready_products;
    std::unique_lock<std::mutex>& uniq_mutex;
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
        act_id(0) {
            std::vector<std::string> create_menu;
            for(auto& record : machines_map) create_menu.push_back(record.first);
            this->menu = create_menu;
        }

    //TODO
    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
        return this->menu;
    }

    //TODO
    std::vector<unsigned int> getPendingOrders() const;

    //TODO: release workers after work is done
    std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {

        worker_mutex.lock();
        act_id++; // order id increase
        int id = act_id;
        std::mutex order_waiting;
        wait_for_worker.push(order_waiting);
        if(!wait_for_worker.empty()) order_waiting.lock(); // if another threads are waiting for the worker
        worker_mutex.unlock();

        order_waiting.lock();
        

        ordering_mutex.lock();
        
        // creating parameters for new_pager
        std::vector<std::unique_ptr<Product>> ready_products;
        std::mutex is_ready_mutex;
        bool is_ready = false;

        orders_ready_mutex.lock();
        orders_mutexes_ready.insert({id, is_ready});
        orders_ready_mutex.unlock();

        std::condition_variable wait_function_con;

        // create nedeed mutexes for Pager
        std::mutex mut;
        std::unique_lock<std::mutex> lock(mut);

        MyCoasterPager new_pager(id, ready_products, is_ready_mutex, products.size(), wait_function_con, lock);

        std::thread worker{worker_orders, products, ready_products, is_ready_mutex, wait_function_con, id};

    }

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
        orders_ready_mutex.lock();
        if(!orders_mutexes_ready[CoasterPager->getId()]) throw OrderNotReadyException();
        orders_ready_mutex.unlock();
        //TODO: reszta wyjątków
    }

    unsigned int getClientTimeout() const {
        return this->clientTimeout;
    }
private:
    // funkcja symbolizująca workera
    void worker_orders(std::vector<std::string> products, 
        std::vector<std::unique_ptr<Product>>& ready_products, std::mutex& is_ready_mutex,
        std::condition_variable& wait_function_con, int id) {

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
        
        is_ready_mutex.lock();
        for(auto& product: future_products) {
            ready_products.push_back(product);
        }

        orders_ready_mutex.lock();
        // changing the boolean value in map to true which helps collectOrder() to throw an exception
        orders_mutexes_ready[id] = true;
        orders_ready_mutex.unlock();

        is_ready_mutex.unlock();

        wait_function_con.notify_one();
        
        //TODO: tutaj czekasz ileś sekund aż ktoś odbierze i wtedy wpuszczasz "kolejnego pracownika"
        // wpuszczanie jakoś tak wygląda: 
        /*
        worker_mutex.lock();
        wait_for_worker.pop(); // wyrzucenie siebie z kolejki
        if(!wait_for_worker.empty()) wait_for_worker.front().unlock();
        worker_mutex.unlock();
        */
    }

    void wait_for_product(std::string& product, std::mutex& mutex, std::unique_ptr<Product> future_product) {
        mutex.lock();
        future_product = machines_map[product]->getProduct();
        
        machine_mutex.lock();
        machines_mutexes[product].pop();
        if(!machines_mutexes[product].empty()) machines_mutexes[product].front().unlock();
        machine_mutex.unlock();
    }

    unsigned int act_id;
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;
    std::queue<std::thread> workers;
    unsigned int free_workers;
    std::mutex wait_for_any_worker;
    machines_t machines_map;
    std::vector<std::string> menu;

    std::mutex orders_ready_mutex; // mutex protecting orders_mutexes_ready map
    // map with order_id, mutex and boolean if the order is ready
    std::unordered_map<unsigned int, bool> orders_mutexes_ready;
    std::mutex worker_mutex; // mutex protecting concurrent changes in wait_for_worker queue;
    std::queue<std::mutex> wait_for_worker; // queue in which orders are waiting for workers when all of them are working
    std::mutex ordering_mutex; // mutex protectining from interrupting creating "waiting_orders" by one worker;
    std::mutex machine_mutex; // mutex protecting concurrent changes in machines_mutexes map;
    std::unordered_map<std::string, std::queue<std::mutex>> machines_mutexes;
};