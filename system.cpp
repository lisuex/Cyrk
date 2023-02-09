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
#include <iostream>

#include "machine.hpp"
using namespace std::chrono_literals;

    void CoasterPager::wait() const {
        ready_mutex->lock();
    }

    void CoasterPager::wait(unsigned int timeout) const {
        // cokolwiek by nie był unused
        std::cout << timeout;
    }

    [[nodiscard]] unsigned int CoasterPager::getId() const {
        return order_id;
    }

    [[nodiscard]] bool CoasterPager::isReady() const {
        return true;
    }
    
    System::System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout) :
        machines_map(machines),
        numberOfWorkers(numberOfWorkers),
        clientTimeout(clientTimeout),
        act_id(0),
        worker_waiting(0) {
            // tworzenie stałych wątków dla workerów
            for(int i = 0; i < (int)numberOfWorkers; i++) {
                std::thread t{&System::worker_function, this};
                worker_threads.insert({i,std::move(t)});
            }

            // tworzenie stałych wątków dla maszyn
            for(auto machine: machines) {
                menu.push_back(machine.first);
                machines_threads.insert({machine.first, std::thread{&System::machine_function, this, machine.first}});
                machines_queues.insert({machine.first, {}});
            }

        }

    std::vector<std::string> System::getMenu() const {
        return menu;
    }

    std::vector<unsigned int> System::getPendingOrders() const {
        return pending_orders;
    }

    std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
        order_mutex.lock(); // początek składania zamówienia
        act_id++;

        std::mutex is_ready_mutex;
        is_ready_mutex.lock();

        auto pager_ptr = std::unique_ptr<CoasterPager>(new CoasterPager(act_id, products, &is_ready_mutex));

        pager_map.insert(std::make_pair(act_id, std::move(pager_ptr)));

        worker_mutex.lock();

        pending_orders.push_back(act_id);
        waiting_pager.insert(std::make_pair(act_id, &is_ready_mutex));

        if(worker_waiting > 0) {
            take_order.notify_one();
        }
        worker_mutex.unlock();

        return pager_ptr;
    }

    std::vector<std::unique_ptr<Product>> System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
        return std::move(CoasterPager->ready_products);
    }

    unsigned int System::getClientTimeout() const {
        return this->clientTimeout;
    }

    void System::worker_function() {
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
    void System::machine_function(std::string name) {
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
            pager->ready_products.push_back(std::move(product)); // dodawanie zrobionego produktu do wektora zrobionych w CoasterPager
            // jeżeli jesteśmy ostatnim dodanych produktem do zamówienia
            if(pager->ready_products.size() >= pager->products.size()) {
                waiting_pager.at(order_id)->unlock();
            }
            pager_access.unlock();
        }
    }
