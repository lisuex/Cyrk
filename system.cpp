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
#include <chrono>
#include<unistd.h>

#include "machine.hpp"

    void CoasterPager::wait() const {
        system->waiting_pager.at(order_id)->lock();
        system->waiting_pager.at(order_id)->unlock();
    }

    void CoasterPager::wait(unsigned int timeout) const {
        auto now = std::chrono::steady_clock::now();
        system->waiting_pager.at(order_id)->try_lock_until(now + std::chrono::milliseconds(timeout));
        system->waiting_pager.at(order_id)->unlock();
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
        menu({}),
        machines_queues({}),
        worker_waiting(0),
        was_shutdown(false) {
            // tworzenie stałych wątków dla workerów
            for(int i = 0; i < (int)numberOfWorkers; i++) {
                worker_threads.insert({i,std::thread{&System::worker_function, this}});
            }

            // tworzenie stałych wątków dla maszyn
            for(auto machine: machines) {
                menu.push_back(machine.first);
                machines_queues.insert({machine.first, {}});
                machines_waiting.insert({machine.first, true});
                machines_signal_map.insert({machine.first, std::unique_ptr<std::condition_variable>(new std::condition_variable())});
                machines_threads.insert({machine.first, std::thread{&System::machine_function, this, machine.first}});
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
        if(was_shutdown) throw RestaurantClosedException();
        else {
            act_id++;
            broken.insert({act_id, false});

            worker_mutex.lock();

            pending_orders.push_back(act_id);
            waiting_pager.insert(std::make_pair(act_id, std::unique_ptr<std::timed_mutex>(new std::timed_mutex())));
            waiting_pager.at(act_id)->lock();

            std::unique_ptr<CoasterPager> pager_ptr = std::unique_ptr<CoasterPager>(new CoasterPager(act_id, this));

            orders.insert({act_id, products});
            collected_products.insert({act_id, std::vector<std::unique_ptr<Product>>{}});

            if(worker_waiting > 0) {
                take_order.notify_one();
            }
            worker_mutex.unlock();

            return pager_ptr;
        }
    }

    std::vector<std::unique_ptr<Product>> System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
        if(was_shutdown || broken.at(CoasterPager->order_id)) throw FulfillmentFailure();
        else {
            waiting_pager.at(CoasterPager->order_id)->lock();
            return std::move(collected_products.at(CoasterPager->order_id));
        }
    }

    unsigned int System::getClientTimeout() const {
        return this->clientTimeout;
    }

    void System::worker_function() {
        
        while(true){
            std::unique_lock<std::mutex> lock(worker_mutex);
            // jeśli nie ma żadnego zamówienia to sami musimy czekać
            if(pending_orders.empty()) {
                worker_waiting++;
                take_order.wait(lock);
                worker_waiting--;
            }

            // pobieranie pierwszego czekającego order_id
            unsigned int order_id = pending_orders.front();
            pending_orders.erase(pending_orders.begin());
            worker_mutex.unlock();
            
            // składanie zamowień do maszyn od konkretnych produktów
            for(auto product: orders.at(order_id)) {
                machine_mutex.lock();

                machines_queues.at(product).emplace(order_id);

                // jeśli maszyna aktualnie czeka na zamówienie
                if(machines_waiting.at(product)) {
                    machines_signal_map.at(product)->notify_one();
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
            std::unique_lock<std::mutex> lock(machine_mutex);
            if(machines_queues.at(name).size() == 0) {
                machines_waiting.at(name) = true;

                // opuszcza machine mutex bo wywował wait(chyba tak to działa)
                machines_signal_map.at(name)->wait(lock); // czekanie aż przyjdzie klient
                machines_waiting.at(name) = false;
            }
            auto order_id = machines_queues.at(name).front();
            machines_queues.at(name).pop();
            machine_mutex.unlock();
            
            // skoro ma klienta to pobiera zamówienie
            try {
                std::unique_ptr<Product> product = machine->getProduct();
                collected_products.at(order_id).push_back(std::move(product)); // dodawanie zrobionego produktu do wektora zrobionych w CoasterPager
                // jeżeli jesteśmy ostatnim dodanych produktem do zamówienia
                if(collected_products.at(order_id).size() >= orders.at(order_id).size()) {
                    waiting_pager.at(order_id)->unlock();
                }
            } catch(const MachineFailure& e) {
                broken.at(order_id) = true;
                waiting_pager.at(order_id)->unlock();
            }
            
        }
        
    }
