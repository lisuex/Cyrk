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
        system->waiting_pager.at(order_id)->lock();
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
    CoasterPager(unsigned int order_id, std::vector<std::string> products, std::unique_ptr<System> system):
    order_id(order_id),
    products(products),
    system(system) {}

private:
    std::mutex ready_mutex;
    std::condition_variable ready; // condition_variable sygnalizowane gdy produkt jest gotowy
    unsigned int order_id;
    std::vector<std::string> products;
    std::unique_ptr<System>& system;
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
        act_id(0),
        worker_waiting(0) {
            // tworzenie stałych wątków dla workerów
            for(int i = 0; i < numberOfWorkers; i++) {
                worker_threads.insert({i,std::thread{worker_function}});
            }

            // tworzenie stałych wątków dla maszyn
            for(auto machine: machines) {
                menu.push_back(machine.first);
                machines_threads.insert({machine.first, std::thread{machine_function, machine.first}});
                machines_queues.insert({machine.first, {}});
            }

        }

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
        return menu;
    }

    std::vector<unsigned int> getPendingOrders() const {
        return pending_orders;
    }

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {
        order_mutex.lock(); // początek składania zamówienia
        act_id++;

        CoasterPager new_pager{act_id, products, std::make_unique<System>(*this)};
        auto pager_ptr = std::make_unique<CoasterPager>(new_pager);

        std::mutex mut;
        std::condition_variable con;
        pager_map.insert(std::make_pair(act_id, pager_ptr));

        worker_mutex.lock();

        pending_orders.push_back(act_id);

        std::mutex is_ready_mutex;
        is_ready_mutex.lock();
        auto is_ready_ptr = std::make_unique<std::mutex>(is_ready_mutex);
        waiting_pager.insert(std::make_pair(act_id, is_ready_ptr));

        if(worker_waiting > 0) {
            take_order.notify_one();
        }
        worker_mutex.unlock();

        return pager_ptr;
    }

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {

    }

    unsigned int getClientTimeout() const {
        return this->clientTimeout;
    }

    friend class CoasterPager;
private:
    void worker_function() {
        while(true){
            worker_mutex.lock();
            // jeśli nie ma żadnego zamówienia to sami musimy czekać
            if(pending_orders.empty()) {
                worker_waiting++;
                std::unique_lock<std::mutex> lock(worker_mutex);
                take_order.wait(lock);
                worker_waiting--;
            }

            // pobieranie pierwszego czekającego order_id
            unsigned int order_id = pending_orders.front();
            pending_orders.erase(pending_orders.begin());
            worker_mutex.unlock();

            const auto& pager_ptr = pager_map.at(order_id);
            const auto& products_vector = pager_ptr->products;
            
            

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
        auto machine = machines_map.at(name);
        machine->start();
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
            std::unique_ptr<Product> product = machine->getProduct();

            pager_access.lock();
            auto& pager = pager_map.at(order_id);
            pager->ready_products.push_back(product); // dodawanie zrobionego produktu do wektora zrobionych w CoasterPager
            // jeżeli jesteśmy ostatnim dodanych produktem do zamówienia
            if(pager->ready_products.size() >= pager->products.size()) {
                waiting_pager.at(order_id)->unlock();
            }
            pager_access.unlock();
        }
    }
    unsigned int act_id; // aktualne id zamówienia
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;

    std::vector<std::string> menu; // wektor z menu potrzebny do metody getMenu()

    std::mutex order_mutex; // mutex na operacje rozpoczęcia zamówienia

    machines_t machines_map;
    std::unordered_map<unsigned int, std::thread> worker_threads; // mapa z (id_workera, worker_thread)
    std::unordered_map<std::string, std::thread> machines_threads; // mapa z (nazwa_maszyny, worker_thread)

    std::mutex pager_access; // mutex do dostępu do pagera żeby wiele maszyn naraz nie dodawało rzeczy do "zrobionych" w pagerze
    std::unordered_map<std::string, std::condition_variable> machines_signal_map; // mapa z condition variable dla maszyn
    std::mutex singal_mutex; // mutex dla mapy powyżej
    std::unordered_map<std::string, bool> machines_waiting;
    std::mutex machine_mutex; // mutex do operacji na machines_queues
    std::unordered_map<std::string, std::queue<unsigned int>> machines_queues; // mapa kolejek id_zamówień do maszyn
    
    std::unordered_map<unsigned int, std::unique_ptr<CoasterPager>> pager_map; // mapa (numer zamówienia, wskaźnik na CoasterPager, condition_variable, mutex)

    std::unordered_map<unsigned int, std::unique_ptr<std::mutex>> waiting_pager; // mapa z mutexem na którym czeka wait w CoasterPager
    unsigned int worker_waiting;
    std::condition_variable take_order; // zmienna warunkowa sygnalizująca robotników, że jest jakieś zamówienie do przetworzenia
    std::mutex worker_mutex; // mutex na sprawdzanie pending_orders
    std::vector<unsigned int> pending_orders; // wektor oczekujących numerów zamówień
};