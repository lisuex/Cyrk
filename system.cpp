#include "system.hpp"

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <thread>
#include <queue>

#include "machine.hpp"

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
    CoasterPager(unsigned int id, std::vector<std::unique_ptr<Product>>& ready_products,
        std::mutex& is_ready_mutex, unsigned int products_num, std::mutex& wait_function_mutex):
        id(id),
        ready_products(ready_products),
        is_ready_mutex(is_ready_mutex),
        products_num(products_num),
        wait_function_mutex(wait_function_mutex) {}

    void wait() const {
        wait_function_mutex.lock();
    }

    void wait(unsigned int timeout) const {
        // nie wiem jak zrobić czas lub mutex
    }

    [[nodiscard]] unsigned int getId() const {
        return id;
    }

    [[nodiscard]] bool isReady() const {
        is_ready_mutex.lock();
        if(ready_products.size() < products_num) {
            return false;
        }
        else return true;
        is_ready_mutex.unlock();
    }

private:
    unsigned int id;
    unsigned int products_num;
    std::mutex& is_ready_mutex; // mutex protecting concurrently checking if the products is ready
    std::mutex& wait_function_mutex; // mutex waiting for the order to be ready
    std::vector<std::unique_ptr<Product>> ready_products;
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
        act_id(0) {}

    

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const;

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
        wait_for_worker.pop();

        ordering_mutex.lock();
        
        // creating parameters for new_pager
        std::vector<std::unique_ptr<Product>> ready_products;
        std::mutex is_ready_mutex;
        bool is_ready = false;

        orders_ready_mutex.lock();
        orders_mutexes_ready.insert({id, is_ready});
        orders_ready_mutex.unlock();

        std::mutex wait_function_mutex;

        wait_function_mutex.lock();
        CoasterPager new_pager(id, ready_products, is_ready_mutex, products.size(), wait_function_mutex);

        std::thread worker{worker_orders, products, ready_products, is_ready_mutex, wait_function_mutex, id};

    }

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
        orders_ready_mutex.lock();
        if(!orders_mutexes_ready[CoasterPager->getId()]) throw OrderNotReadyException();
        orders_ready_mutex.unlock();
        //TODO: reszta wyjątków
    }

    unsigned int getClientTimeout() const;
private:
    // funkcja symbolizująca workera
    void worker_orders(std::vector<std::string> products, 
        std::vector<std::unique_ptr<Product>>& ready_products, std::mutex& is_ready_mutex,
        std::mutex& wait_function_mutex, int id) {

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

        wait_function_mutex.unlock();
    }

    void wait_for_product(std::string& product, std::mutex& mutex, std::unique_ptr<Product> future_product) {
        mutex.lock();
        machines_mutexes[product].pop();
        future_product = machines_map[product]->getProduct();

        machine_mutex.lock();
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

    std::mutex orders_ready_mutex; // mutex protecting orders_mutexes_ready map
    // map with order_id, mutex and boolean if the order is ready
    std::unordered_map<unsigned int, bool> orders_mutexes_ready;
    std::mutex worker_mutex; // mutex protecting concurrent changes in wait_for_worker queue;
    std::queue<std::mutex> wait_for_worker; // queue in which orders are waiting for workers when all of them are working
    std::mutex ordering_mutex; // mutex protectining from interrupting creating "waiting_orders" by one worker;
    std::mutex machine_mutex; // mutex protecting concurrent changes in machines_mutexes map;
    std::unordered_map<std::string, std::queue<std::mutex>> machines_mutexes;
};