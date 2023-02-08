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
    std::vector<std::unique_ptr<Product>> ready_products; // wetkor gdzie na bieżąco są dodawane zrobione gotowe produkty
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
                machines_threads.insert({machine.first, std::thread{machine_function, machine.first}});
                machines_queues.insert({machine.first, {}});
            }
        }

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
    }

    std::vector<unsigned int> getPendingOrders() const {
        std::vector<unsigned int> pending_orders;
    }

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {
        order_mutex.lock(); // początek składania zamówienia
        act_id++;
        CoasterPager new_pager{act_id};
        auto pager_ptr = std::make_unique<CoasterPager>(new_pager);

        pending_orders.emplace(std::make_tuple(pager_ptr, act_id, products));

        // TODO:tu powinien tylko wtedy notyfikować gdy czekają workerzy, w przeciwnym przypadku tylko ustawia sie w kolejce
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
            const auto& pager_ptr = std::get<0>(order);
            
            pager_map.insert(std::make_pair(order_id, pager_ptr));

            // składanie zamowień do maszyn od konkretnych produktów
            for(const auto& product: products_vector) {
                machine_mutex.lock();
                machines_queues.at(product).emplace(order_id);

                // jeśli maszyna aktualnie czeka na zamówienie
                if(machines_waiting.at(product)) {
                    machines_signal_map.at(product).notify_one();
                }
                machine_mutex.unlock();
            }
            order_mutex.unlock(); // koniec składania zamówienia, wszystko dodane do kolejek do maszyn
        }
    }
    void machine_function(std::string name) {
        while(true){
            machine_mutex.lock();
            if(machines_queues.at(name).size() == 0) {
                machines_waiting.at(name) = true;
                std::unique_lock<std::mutex> lock(machine_mutex);

                // opuszcza machine mutex bo wywował wait(chyba tak to działa)
                machines_signal_map.at(name).wait(lock); // czekanie aż przyjdzie klient
                machines_waiting.at(name) = false;
            }
            auto order_id = machines_queues.at(name).front();
            machines_queues.at(name).pop();
            machine_mutex.unlock();
            
            // skoro ma klienta to pobiera zamówienie
            std::unique_ptr<Product> product = machines_map.at(name)->getProduct();

            pager_access.lock();
            pager_map.at(order_id)->ready_products.push_back(product); // dodawanie zrobionego produktu do wektora zrobionych w CoasterPager
            pager_access.unlock();

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

    std::mutex pager_access; // mutex do dostępu do pagera żeby wiele maszyn naraz nie dodawało rzeczy do "zrobionych" w pagerze
    std::unordered_map<std::string, std::condition_variable> machines_signal_map; // mapa z condition variable dla maszyn
    std::mutex singal_mutex; // mutex dla mapy powyżej
    std::unordered_map<std::string, bool> machines_waiting;
    std::mutex machine_mutex; // mutex do operacji na machines_queues
    std::unordered_map<std::string, std::queue<unsigned int>> machines_queues; // mapa kolejek id_zamówień do maszyn
    
    std::unordered_map<unsigned int, std::unique_ptr<CoasterPager>> pager_map; // mapa (numer zamówienia, wskaźnik na CoasterPager)

    std::vector<unsigned int> pending_orders_vector; // wektor oczekujących numerów zamówień
    std::queue<std::tuple<std::unique_ptr<CoasterPager>, unsigned int, std::vector<std::string>>> pending_orders; // zamówienia złożone, ale jeszcze oczekujące na rozpoczęcie realizacji 
};