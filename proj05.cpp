#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <pthread.h>
#include <iomanip>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <cmath>
#include <unistd.h> 
#include <cstdlib>  

// Struct definitions
struct InventoryItem {
    unsigned int productId;
    double price;
    unsigned int quantity;
    std::string description;
};

struct Order {
    unsigned int customerId;
    unsigned int productId;
    unsigned int quantity;
    bool isEndOfData = false; 
};

//Variable declarations
std::vector<InventoryItem> inventory;
std::mutex inventoryMutex;

int numProducers = 1; // Default number of producers
int bufferSize = 10;  // Default buffer size

class BoundedBuffer {
private:
    std::queue<Order> buffer;
    std::mutex mtx;
    std::condition_variable notFull;
    std::condition_variable notEmpty;
    size_t maxSize;

public:
    BoundedBuffer(size_t size) : maxSize(size) {}

    void insert(const Order& item) {
        std::unique_lock<std::mutex> lock(mtx); //lock
        //std::unique_lock<std::mutex> lock(mtx, std::defer_lock);
        /*notFull.wait(lock, [this]() { return buffer.size() < maxSize; });*/ 
        /*std::cout << "producing" << std::endl;*/
        notFull.wait(lock, [this]() { return buffer.size() < maxSize; });
        /*notFull.push(item);*/
        buffer.push(item);
        notEmpty.notify_one();
    }
    Order remove() {
        std::unique_lock<std::mutex> lock(mtx);
        notEmpty.wait(lock, [this]() { return !buffer.empty(); });
        Order item = buffer.front();
        buffer.pop();
        notFull.notify_one();
        return item;
    }
};

BoundedBuffer* boundedBuffer;

std::string formatPrice(double price) {
    std::stringstream ss;
    ss << "$" << std::fixed << std::setprecision(2) << price;
    return ss.str();
}

void readInventory(const std::string& filename) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Could not open " << filename << std::endl;
        exit(1);
    }

    InventoryItem item;
    while (file >> item.productId >> item.price >> item.quantity) {
        file.ignore(); // Skip space
        std::getline(file, item.description);
        inventory.push_back(item);
    }

    file.close();
}

void writeInventory(const std::string& filename) {
    std::ofstream file(filename);
    if (!file) {
        std::cerr << "Could not open " << filename << std::endl;
        exit(1);
    }

    for (const auto& item : inventory) {
        file << std::setw(6) << item.productId << " "
             << std::fixed << std::setprecision(2)
             << std::setw(5) << item.price << " "
             << std::setw(5) << item.quantity << " "
             << item.description << std::endl;
    }

    file.close();
}

void* producerThread(void* arg) {
    int threadNum = *(int*)arg;
    std::string filename = "orders" + std::to_string(threadNum);
    std::ifstream orderFile(filename);

    if (!orderFile) {
        std::cerr << "Could not open " << filename << std::endl;
        pthread_exit(NULL);
    }

    Order order;
    while (orderFile >> order.customerId >> order.productId >> order.quantity) {
        // Insert order into the bounded buffer
        boundedBuffer->insert(order);
    }

    // Insert a special 'end-of-data' order
    Order endOrder;
    endOrder.isEndOfData = true;
    boundedBuffer->insert(endOrder);

    orderFile.close();
    pthread_exit(NULL);
}

void* consumerThread(void* arg) {
    std::ofstream logFile("log");
    if (!logFile) {
        std::cerr << "Could not open log file" << std::endl;
        pthread_exit(NULL);
    }

    // Write header
    logFile << std::left
            << std::setw(10) << "Customer"
            << std::setw(10) << "Product"
            << std::setw(31) << "Description"
            << std::right
            << std::setw(10) << "Ordered"
            << std::setw(12) << "Amount"
            << std::setw(25) << "Result" << std::endl;

    int producersFinished = 0;

    while (producersFinished < numProducers) {
        Order order = boundedBuffer->remove();

        if (order.isEndOfData) {
            // Special 'end-of-data' order received
            producersFinished++;
            continue;
        }

        bool orderFilled = false;
        bool itemFound = false;
        double transactionAmount = 0.0;
        std::string productDesc;
        std::string resultMessage;

        {
            std::lock_guard<std::mutex> lock(inventoryMutex);
            for (auto& item : inventory) {
                if (item.productId == order.productId) {
                    itemFound = true;
                    productDesc = item.description;

                    if (item.quantity >= order.quantity) {
                        item.quantity -= order.quantity;
                        orderFilled = true;
                        transactionAmount = order.quantity * item.price;
                        resultMessage = "Filled";
                    } else {
                        resultMessage = "       Rejected - Insufficient quantity";
                    }
                    break;
                }
            }

            if (!itemFound) {
                resultMessage = "       Rejected - Item not found";
            }
        }

        // Write log entry
        logFile << std::left
                << std::setw(10) << order.customerId
                << std::setw(10) << order.productId
                << std::setw(31) << (itemFound ? productDesc : "Unknown Item")
                << std::right
                << std::setw(10) << order.quantity
                << std::setw(12) << (orderFilled ? formatPrice(transactionAmount) : "$0.00")
                << std::setw(25) << resultMessage
                << std::endl;
    }

    logFile.close();
    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "p:b:")) != -1) {
        switch (opt) {
            case 'p':
                numProducers = std::stoi(optarg);
                if (numProducers < 1 || numProducers > 9) {
                    std::cerr << "Number of producers must be between 1 and 9." << std::endl;
                    exit(1);
                }
                break;
            case 'b':
                bufferSize = std::stoi(optarg);
                if (bufferSize < 1 || bufferSize > 30) {
                    std::cerr << "Buffer size must be between 1 and 30." << std::endl;
                    exit(1);
                }
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-p numProducers] [-b bufferSize]" << std::endl;
                exit(1);
        }
    }

    boundedBuffer = new BoundedBuffer(bufferSize);
    readInventory("inventory.old");
    //create producer threads
    std::vector<pthread_t> producers(numProducers);
    std::vector<int> threadNums(numProducers);

    for (int i = 0; i < numProducers; ++i) {
        threadNums[i] = i + 1;
        if (pthread_create(&producers[i], NULL, producerThread, &threadNums[i]) != 0) {
            std::cerr << "Error creating producer thread " << i + 1 << std::endl;
            exit(1);
        }
    }

    //create consumer thread
    pthread_t consumer;
    if (pthread_create(&consumer, NULL, consumerThread, NULL) != 0) {
        std::cerr << "Error creating consumer thread" << std::endl;
        exit(1);
    }

    //Join producer
    for (int i = 0; i < numProducers; ++i) {
        if (pthread_join(producers[i], NULL) != 0) {
            std::cerr << "Error joining producer thread " << i + 1 << std::endl;
            exit(1);
        }
    }

    //Join consumer
    if (pthread_join(consumer, NULL) != 0) {
        std::cerr << "Error joining consumer thread" << std::endl;
        exit(1);
    }

    //Write final inventory
    writeInventory("inventory.new");

    //Clean up
    delete boundedBuffer;

    return 0;
}
