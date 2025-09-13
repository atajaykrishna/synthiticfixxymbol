#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <unordered_map>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <sys/mman.h>
#include <fcntl.h>
#include <mutex>
#include <algorithm>
#include <fstream>
#include <regex>

struct PriceData {
    double bid;
    double ask;
    bool operator==(const PriceData& other) const {
        return bid == other.bid && ask == other.ask;
    }
    bool operator!=(const PriceData& other) const {
        return !(*this == other);
    }
};

constexpr int MAX_SYMBOLS = 100;

struct SharedMemory {
    char symbols[MAX_SYMBOLS][32];
    PriceData prices[MAX_SYMBOLS];
    int count;
};

// Global variables
SharedMemory* shm = nullptr;
std::vector<int> clients;
std::mutex clients_mutex;
const int PORT = 2222;

// --- New Structures for Formula Parsing ---
enum class Operation { ADD, SUBTRACT, NONE };
enum class PriceType { BID, ASK, CONSTANT, NONE };

struct FormulaTerm {
    Operation op;
    std::string symbol_name;
    PriceType price_type;
    double constant_value;
};

struct Formula {
    std::vector<FormulaTerm> terms;
};

struct SyntheticSymbol {
    Formula bid_formula;
    Formula ask_formula;
    int precision = 5; // New member to store the number of decimal places
};

std::unordered_map<std::string, SyntheticSymbol> synthetic_symbols;
std::mutex synthetic_mutex;

// --- Helper Functions ---

// Trim whitespace from a string
std::string trim(const std::string& str) {
    const std::string whitespace = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(whitespace);
    if (std::string::npos == start) {
        return str;
    }
    size_t end = str.find_last_not_of(whitespace);
    return str.substr(start, end - start + 1);
}

// Find a symbol's index in shared memory
int find_symbol_index(const std::string& symbol) {
    if (!shm) return -1;
    for (int i = 0; i < shm->count; ++i) {
        if (strcmp(shm->symbols[i], symbol.c_str()) == 0) {
            return i;
        }
    }
    return -1;
}

// Evaluate a formula using data from shared memory
double evaluate_formula(const Formula& formula) {
    if (formula.terms.empty()) {
        return 0.0;
    }

    double result = 0.0;
    
    // Process the first term
    const auto& first_term = formula.terms[0];
    if (first_term.price_type == PriceType::CONSTANT) {
        result = first_term.constant_value;
    } else {
        int idx = find_symbol_index(first_term.symbol_name);
        if (idx == -1) return 0.0; // Symbol not found
        result = (first_term.price_type == PriceType::BID) ? shm->prices[idx].bid : shm->prices[idx].ask;
    }

    // Process subsequent terms
    for (size_t i = 1; i < formula.terms.size(); ++i) {
        const auto& term = formula.terms[i];
        double value = 0.0;
        if (term.price_type == PriceType::CONSTANT) {
            value = term.constant_value;
        } else {
            int idx = find_symbol_index(term.symbol_name);
            if (idx == -1) continue; // Skip if symbol not found
            value = (term.price_type == PriceType::BID) ? shm->prices[idx].bid : shm->prices[idx].ask;
        }

        switch (term.op) {
            case Operation::ADD:
                result += value;
                break;
            case Operation::SUBTRACT:
                result -= value;
                break;
            default:
                break;
        }
    }
    return result;
}

// Load formulas from a config file
void load_formulas_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open " << filename << ". No synthetic prices will be generated.\n";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Split at '='
        size_t equals_pos = line.find('=');
        if (equals_pos == std::string::npos) continue;

        std::string synth_name_part = trim(line.substr(0, equals_pos));
        std::string formula_and_digits = trim(line.substr(equals_pos + 1));

        // Extract digits if present
        int precision = 5; // default
        size_t digits_pos = formula_and_digits.find(", digits=");
        std::string formula_str;
        if (digits_pos != std::string::npos) {
            formula_str = trim(formula_and_digits.substr(0, digits_pos));
            std::string digits_str = trim(formula_and_digits.substr(digits_pos + 9));
            try {
                precision = std::stoi(digits_str);
            } catch (...) {
                std::cerr << "Warning: Invalid digits for " << synth_name_part << "\n";
            }
        } else {
            formula_str = formula_and_digits;
        }

        // Extract bid/ask from name
        std::string synth_base_name;
        std::string price_type_str;
        size_t last_underscore = synth_name_part.find_last_of('_');
        if (last_underscore != std::string::npos) {
            synth_base_name = synth_name_part.substr(0, last_underscore);
            price_type_str = synth_name_part.substr(last_underscore + 1);
        } else continue;

        // Parse formula terms
        Formula current_formula;
        std::stringstream formula_ss(formula_str);
        std::string term_str;
        while (formula_ss >> term_str) {
            FormulaTerm term;
            term.op = Operation::NONE;

            if (term_str == "+") {
                term.op = Operation::ADD;
                formula_ss >> term_str;
            } else if (term_str == "-") {
                term.op = Operation::SUBTRACT;
                formula_ss >> term_str;
            } else {
                term.op = Operation::ADD; // first term
            }

            size_t last_dot = term_str.rfind('.');
            if (last_dot != std::string::npos) {
                std::string type = term_str.substr(last_dot + 1);
                if (type == "bid" || type == "ask") {
                    term.symbol_name = term_str.substr(0, last_dot);
                    term.price_type = (type == "bid") ? PriceType::BID : PriceType::ASK;
                } else {
                    try {
                        term.constant_value = std::stod(term_str);
                        term.price_type = PriceType::CONSTANT;
                    } catch (...) { continue; }
                }
            } else {
                try {
                    term.constant_value = std::stod(term_str);
                    term.price_type = PriceType::CONSTANT;
                } catch (...) { continue; }
            }
            current_formula.terms.push_back(term);
        }

        // Save formula and precision
        std::lock_guard<std::mutex> lock(synthetic_mutex);
        if (price_type_str == "bid") {
            synthetic_symbols[synth_base_name].bid_formula = current_formula;
            synthetic_symbols[synth_base_name].precision = precision;
        } else if (price_type_str == "ask") {
            synthetic_symbols[synth_base_name].ask_formula = current_formula;
            synthetic_symbols[synth_base_name].precision = precision;
        }
    }
    std::cout << "Loaded " << synthetic_symbols.size() << " synthetic symbol formulas.\n";
}

// Global variable and mutex for last known prices
std::unordered_map<std::string, PriceData> last_prices;
std::mutex last_prices_mutex;

void broadcast_prices() {
    while (true) {
        if (!shm) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::stringstream ss;
        std::lock_guard<std::mutex> last_prices_lock(last_prices_mutex);
        
        // Check and broadcast raw prices from shared memory if they've changed
        for (int i = 0; i < shm->count; ++i) {
            std::string symbol_name = shm->symbols[i];
            PriceData current_price = shm->prices[i];
            
            if (last_prices.find(symbol_name) == last_prices.end() || last_prices[symbol_name] != current_price) {
                ss << std::fixed << std::setprecision(5)
                   << symbol_name << " "
                   << current_price.bid << " "
                   << current_price.ask << "\n";
                last_prices[symbol_name] = current_price;
            }
        }
        
        // Check and broadcast calculated synthetic prices if they've changed
        std::lock_guard<std::mutex> synthetic_lock(synthetic_mutex);
        for (const auto& pair : synthetic_symbols) {
            std::string symbol_name = pair.first;
            PriceData current_price;
            current_price.bid = evaluate_formula(pair.second.bid_formula);
            current_price.ask = evaluate_formula(pair.second.ask_formula);

            // Using stringstream to round the values for comparison
            std::stringstream bid_ss, ask_ss;
            bid_ss << std::fixed << std::setprecision(pair.second.precision) << current_price.bid;
            ask_ss << std::fixed << std::setprecision(pair.second.precision) << current_price.ask;
            double rounded_bid = std::stod(bid_ss.str());
            double rounded_ask = std::stod(ask_ss.str());

            PriceData rounded_current_price = {rounded_bid, rounded_ask};

            if (last_prices.find(symbol_name) == last_prices.end() || last_prices[symbol_name] != rounded_current_price) {
                ss << std::fixed << std::setprecision(pair.second.precision)
                   << symbol_name << " " << rounded_current_price.bid << " " << rounded_current_price.ask << "\n";
                last_prices[symbol_name] = rounded_current_price;
            }
        }
        
        std::string data = ss.str();
        
        // Only broadcast if there's new data
        if (!data.empty()) {
            std::lock_guard<std::mutex> clients_lock(clients_mutex);
            for (auto it = clients.begin(); it != clients.end();) {
                ssize_t sent = send(*it, data.c_str(), data.size(), MSG_NOSIGNAL);
                if (sent <= 0) {
                    if (errno != EWOULDBLOCK && errno != EAGAIN) {
                        std::cout << "[Server] Client disconnected: " << *it << std::endl;
                        close(*it);
                        it = clients.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void handle_client(int conn_fd) {
    try {
        send(conn_fd, "Fake HFT DDE Server 1.0\nLogin:\n", 33, 0);
        char buffer[1024] = {0};
        read(conn_fd, buffer, 1024);
        send(conn_fd, "Password:\n", 10, 0);
        read(conn_fd, buffer, 1024);
        send(conn_fd, "> Access granted\n", 17, 0);

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(conn_fd);
        }

        while (true) {
            // Check for client disconnection by attempting a non-blocking read
            char dummy_buffer[1];
            ssize_t n = recv(conn_fd, dummy_buffer, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                // Client closed the connection
                throw std::runtime_error("Client disconnected.");
            } else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                // Other error, assume disconnection
                throw std::runtime_error("Socket error.");
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (...) {
        close(conn_fd);
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(std::remove(clients.begin(), clients.end(), conn_fd), clients.end());
    }
}

int main() {
    // Open shared memory
    int fd = shm_open("/market_prices", O_RDONLY, 0666);
    if (fd < 0) {
        std::cerr << "Failed to open shared memory. Make sure the producer is running.\n";
        return 1;
    }

    shm = (SharedMemory*)mmap(nullptr, sizeof(SharedMemory), PROT_READ, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        std::cerr << "Failed to map shared memory\n";
        close(fd);
        return 1;
    }
    close(fd);

    // Load formulas from file
    load_formulas_from_file("formulas.cfg");

    // Start TCP server
    int server_fd;
    struct sockaddr_in address{};
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        return 1;
    }

    std::thread broadcaster(broadcast_prices);
    broadcaster.detach();

    std::cout << "TCP server running on port " << PORT << std::endl;

    while (true) {
        int new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }
        std::cout << "New client connected: " << new_socket << std::endl;
        std::thread(handle_client, new_socket).detach();
    }

    munmap(shm, sizeof(SharedMemory));
    return 0;
}
