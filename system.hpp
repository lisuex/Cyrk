#ifndef SYSTEM_HPP
#define SYSTEM_HPP

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

class System;

class CoasterPager
{
public:
    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const;

    [[nodiscard]] bool isReady() const;

    friend class System;
protected:
    CoasterPager(unsigned int order_id, System* system):
    order_id(order_id),
    system(system) {}

private:
    std::condition_variable ready; // condition_variable sygnalizowane gdy produkt jest gotowy
    unsigned int order_id;
    System* system;
    std::vector<std::unique_ptr<Product>> ready_products; // wetkor gdzie na bieżąco są dodawane zrobione gotowe produkty
};

class System
{
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
    
    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout);

    std::vector<WorkerReport> shutdown() {
        for(auto machine: machines_map) {
            machine.second->stop();
        }
        was_shutdown = true;

        return {};
    }

    std::vector<std::string> getMenu() const;

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    unsigned int getClientTimeout() const;

    friend class CoasterPager;
private:
    void worker_function();
    void machine_function(std::string name);

    machines_t machines_map;
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;

    unsigned int act_id; // aktualne id zamówienia

    std::vector<std::string> menu; // wektor z menu potrzebny do metody getMenu()

    std::mutex order_mutex; // mutex na operacje rozpoczęcia zamówienia

    std::unordered_map<unsigned int, std::thread> worker_threads; // mapa z (id_workera, worker_thread)
    std::unordered_map<std::string, std::thread> machines_threads; // mapa z (nazwa_maszyny, worker_thread)

    std::unordered_map<std::string, std::unique_ptr<std::condition_variable>> machines_signal_map; // mapa z condition variable dla maszyn
    std::mutex singal_mutex; // mutex dla mapy powyżej
    std::unordered_map<std::string, bool> machines_waiting;
    std::mutex machine_mutex; // mutex do operacji na machines_queues
    std::unordered_map<std::string, std::queue<unsigned int>> machines_queues; // mapa kolejek id_zamówień do maszyn

    std::unordered_map<unsigned int, std::vector<std::string> > orders;
    std::unordered_map<unsigned int, std::vector<std::unique_ptr<Product>> > collected_products;
    
    std::unordered_map<unsigned int, std::unique_ptr<std::mutex>> waiting_pager; // mapa z mutexem na którym czeka wait w CoasterPager
    unsigned int worker_waiting;
    std::condition_variable take_order; // zmienna warunkowa sygnalizująca robotników, że jest jakieś zamówienie do przetworzenia
    std::mutex worker_mutex; // mutex na sprawdzanie pending_orders
    std::vector<unsigned int> pending_orders; // wektor oczekujących numerów zamówień

    volatile bool was_shutdown;
};


#endif // SYSTEM_HPP