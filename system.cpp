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
// TODO: awarie maszyn

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
    }

    void wait(unsigned int timeout) const {
    }

    [[nodiscard]] unsigned int getId() const {
        return order_id;
    }

    [[nodiscard]] bool isReady() const {
    }

    friend class System;

protected:
    CoasterPager(unsigned int order_id):
    order_id(order_id) {}

private:
    unsigned int order_id;
};

class System
{
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
    
    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout) :
        machines_map(machines),
        numberOfWorkers(numberOfWorkers),
        clientTimeout(clientTimeout),
        act_id(0) {
            // tworzenie stałych wątków dla workerów
            for(int i = 0; i < numberOfWorkers; i++) {
                worker_threads.insert({i,std::thread{worker_function}});
            }

            // tworzenie stałych wątków dla maszyn
            for(auto machine: machines) {
                machines_threads.insert({machine.first, std::thread{machine_function}});
                machines_queues.insert({machine.first, {}});
            }
        }

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
    }

    std::vector<unsigned int> getPendingOrders() const {
    }

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {
        order_mutex.lock(); // początek składania zamówienia
        act_id++;
        CoasterPager new_pager{act_id};
        auto pager_ptr = std::make_unique<CoasterPager>(new_pager);
        pending_orders.emplace(std::make_tuple(pager_ptr, act_id, products));
        take_order.notify_one();
        return pager_ptr;
    }

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {

    }

    unsigned int getClientTimeout() const {
        return this->clientTimeout;
    }
private:
    void worker_function() {
        while(true){
            std::unique_lock<std::mutex> lock(take_order_mutex);
            take_order.wait(lock);
            auto& order = pending_orders.front(); 
            pending_orders.pop();

            const auto& products_vector = std::get<2>(order);
            unsigned int order_id = std::get<1>(order);
            
            // składanie zamowień do maszyn od konkretnych produktów
            for(const auto& product: products_vector) {
                // TODO: zaimplementuj by informować maszyny, że jest ktoś w kolejce
                // TODO: omutexuj kolejke do maszyn
                machines_queues.at(product).emplace(order_id);
            }

            order_mutex.unlock(); // koniec składania zamówienia, wszystko dodane do kolejek do maszyn
        }
    }
    void machine_function() {
        while(true){

        }
    }
    unsigned int act_id; // aktualne id zamówienia
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;

    std::mutex order_mutex; // mutex na operacje rozpoczęcia zamówienia

    std::condition_variable take_order; // zmienna warunkowa sygnalizująca robotników, że jest jakieś zamówienie do przetworzenia
    std::mutex take_order_mutex; // mutex dla zmiennej warunkowej take_order
    
    machines_t machines_map;
    std::unordered_map<unsigned int, std::thread> worker_threads; // mapa z (id_workera, worker_thread)
    std::unordered_map<std::string, std::thread> machines_threads; // mapa z (nazwa_maszyny, worker_thread)

    std::unordered_map<std::string, std::queue<unsigned int>> machines_queues; // mapa kolejek id_zamówień do maszyn
    
    std::queue<std::tuple<std::unique_ptr<CoasterPager>, unsigned int, std::vector<std::string>>> pending_orders; // zamówienia złożone, ale jeszcze oczekujące na rozpoczęcie realizacji 
};