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
        menu({}),
        machines_queues({}),
        worker_waiting(0) {
            // tworzenie stałych wątków dla workerów
            for(int i = 0; i < (int)numberOfWorkers; i++) {
                worker_threads.insert({i,std::thread{&System::worker_function, this}});
            }

            // tworzenie stałych wątków dla maszyn
            for(auto machine: machines) {
                std::cout << "constuctor 0" << std::endl;
                menu.push_back(machine.first);
                std::cout << "menu read " << std::endl;
                for(auto x: menu) std::cout << "menu: " << x << std::endl;
                std::cout << "menu read end" << std::endl;

                std::cout << "constuctor 1" << std::endl;
                machines_queues.insert({machine.first, {}});
                std::cout << "constuctor 2" << std::endl;
                machines_threads.insert({machine.first, std::thread{&System::machine_function, this, machine.first}});
                
                std::cout << "constuctor 3" << std::endl;
            }

        }

    std::vector<std::string> System::getMenu() const {
        std::cout << "menu" << std::endl;
        for(auto x: menu) std::cout << "menu: " << x << std::endl;
        std::cout << "after for in menu" << std::endl;
        return menu;
    }

    std::vector<unsigned int> System::getPendingOrders() const {
        return pending_orders;
    }

    std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
        std::cout << "order0" << std::endl;
        order_mutex.lock(); // początek składania zamówienia
        act_id++;
        std::cout << "order1" << std::endl;
        std::mutex is_ready_mutex;
        std::cout << "order2" << std::endl;
        is_ready_mutex.lock();
        std::cout << "order3" << std::endl;

        auto pager_ptr = std::unique_ptr<CoasterPager>(new CoasterPager(act_id, products, &is_ready_mutex));
        std::cout << "order4" << std::endl;
        pager_map.insert(std::make_pair(act_id, std::move(pager_ptr)));

        std::cout << "order5" << std::endl;
        worker_mutex.lock();

        std::cout << "order6" << std::endl;
        pending_orders.push_back(act_id);
        std::cout << "order7" << std::endl;
        waiting_pager.insert(std::make_pair(act_id, &is_ready_mutex));
        std::cout << "order8" << std::endl;

        if(worker_waiting > 0) {
            take_order.notify_one();
        }
        std::cout << "order9" << std::endl;
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
            std::cout << "worker function 0" << std::endl;
            worker_mutex.lock();
            std::cout << "worker function 1" << std::endl;
            // jeśli nie ma żadnego zamówienia to sami musimy czekać
            if(pending_orders.empty()) {
                std::cout << "worker function 2" << std::endl;
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
        std::cout << "machines map before" << std::endl;
        auto machine = machines_map.at(name);
        std::cout << "machines map after" << std::endl;
        machine->start();
        while(true){
            std::cout << "machines0" << std::endl;
            machine_mutex.lock();
            std::cout << "machines1" << std::endl;
            if(machines_queues.at(name).size() == 0) {
                std::cout << "machines2" << std::endl;
                machines_waiting.at(name) = true;
                std::unique_lock<std::mutex> lock(machine_mutex);

                // opuszcza machine mutex bo wywował wait(chyba tak to działa)
                machines_signal_map.at(name).wait(lock); // czekanie aż przyjdzie klient
                machines_waiting.at(name) = false;
            }
            std::cout << "machines3" << std::endl;
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
