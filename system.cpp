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
    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const;

    [[nodiscard]] bool isReady() const;
};

class System
{
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
    
    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout) :
        machines_map(machines),
        numberOfWorkers(numberOfWorkers),
        clientTimeout(clientTimeout),
        free_workers(numberOfWorkers) {}

    

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const;

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {

        worker_mutex.lock();
        std::mutex order_waiting;
        wait_for_worker.push(order_waiting);
        if(!wait_for_worker.empty()) order_waiting.lock(); // if another threads are waiting for the worker
        worker_mutex.unlock();

        order_waiting.lock();
        wait_for_worker.pop();

        ordering_mutex.lock();
        std::thread worker{worker_orders};

    }

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
        

        // gdzieś na końcu
        free_workers++;
    }

    unsigned int getClientTimeout() const;
private:
    // funkcja wywoływana przez workera by coś zamówić
    void worker_orders(std::vector<std::string> products) {
        for(auto& product_name: products) {
            std::mutex mutex;

            machine_mutex.lock();
            if(!machines_mutexes[product_name].empty()) mutex.lock(); // if another threads are waiting for this product
            machines_mutexes[product_name].push(mutex);
            machine_mutex.unlock();

            std::thread product_thread{wait_for_product, product_name, mutex};
        }
        ordering_mutex.unlock();
    }
    std::unique_ptr<Product> wait_for_product(std::string& product, std::mutex& mutex) {
        mutex.lock();
        machines_mutexes[product].pop();
        auto ready_product = machines_map[product]->getProduct();

        machine_mutex.lock();
        if(!machines_mutexes[product].empty()) machines_mutexes[product].front().unlock();
        machine_mutex.unlock();

        return ready_product;
    }
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;
    std::queue<std::thread> workers;
    unsigned int free_workers;
    std::mutex wait_for_any_worker;
    machines_t machines_map;

    std::mutex worker_mutex; // mutex protecting concurrent changes in wait_for_worker queue;
    std::queue<std::mutex> wait_for_worker; // queue in which orders are waiting for workers when all of them are working
    std::mutex ordering_mutex; // mutex protectining from interrupting creating "waiting_orders" by one worker;
    std::mutex machine_mutex; // mutex protecting concurrent changes in machines_mutexes map;
    std::unordered_map<std::string, std::queue<std::mutex>> machines_mutexes;
};