#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/SocketInitiator.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/FileStore.h"
#include "quickfix/FileLog.h"
#include "quickfix/fix44/MarketDataRequest.h"
#include "quickfix/fix44/MarketDataSnapshotFullRefresh.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

struct PriceData {
    double bid;
    double ask;
};

// Maximum number of symbols you want to support
constexpr int MAX_SYMBOLS = 100;

struct SharedMemory {
    char symbols[MAX_SYMBOLS][32]; // symbol names
    PriceData prices[MAX_SYMBOLS];
    int count;
};

SharedMemory* shm = nullptr;

class MarketDataListener : public FIX::Application, public FIX::MessageCracker {
public:
    MarketDataListener() {
        // Create shared memory
        int fd = shm_open("/market_prices", O_CREAT | O_RDWR, 0666);
        ftruncate(fd, sizeof(SharedMemory));
        shm = (SharedMemory*)mmap(nullptr, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (!shm) {
            std::cerr << "Failed to map shared memory\n";
        }
        shm->count = 0;
        close(fd);
    }

    ~MarketDataListener() {
        if (shm) {
            munmap(shm, sizeof(SharedMemory));
            shm_unlink("/market_prices");
        }
    }

    void onCreate(const FIX::SessionID &sessionID) override { 
        std::cout << "Session created: " << sessionID << std::endl; 
    }

    void onLogon(const FIX::SessionID &sessionID) override {
        std::cout << "Logon: " << sessionID << std::endl;
        requestMarketData(sessionID, {"XAUUSD.m", "GCZ25.m"});
    }

    void onLogout(const FIX::SessionID &sessionID) override { 
        std::cout << "Logout: " << sessionID << std::endl; 
    }

    void toAdmin(FIX::Message &, const FIX::SessionID &) override {}
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID& s)
        throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override {
        std::cout << "Admin Msg: " << msg.toString() << std::endl;
    }

    void toApp(FIX::Message& msg, const FIX::SessionID& s) throw(FIX::DoNotSend) override {
        std::cout << "Sending: " << msg.toString() << std::endl;
    }

    void fromApp(const FIX::Message& msg, const FIX::SessionID& s)
        throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override {
        crack(msg, s);
    }

    void onMessage(const FIX44::MarketDataSnapshotFullRefresh& msg, const FIX::SessionID&) {
        FIX::Symbol symbol;
        FIX::NoMDEntries noEntries;
        msg.get(symbol);
        msg.get(noEntries);

        double bid = 0.0, ask = 0.0;
        for (int i = 1; i <= noEntries; ++i) {
            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
            msg.getGroup(i, group);
            FIX::MDEntryType type;
            FIX::MDEntryPx px;
            group.get(type);
            group.get(px);
            if (type.getValue() == FIX::MDEntryType_BID) bid = px.getValue();
            else if (type.getValue() == FIX::MDEntryType_OFFER) ask = px.getValue();
        }

        saveToSharedMemory(symbol.getString(), bid, ask);
    }

private:
    void requestMarketData(const FIX::SessionID &sessionID, const std::vector<std::string> &symbols) {
        static int mdReqID = 1;
        FIX44::MarketDataRequest mdReq;
        mdReq.set(FIX::MDReqID("MD_" + std::to_string(mdReqID++)));
        mdReq.set(FIX::SubscriptionRequestType(FIX::SubscriptionRequestType_SNAPSHOT_PLUS_UPDATES));
        mdReq.set(FIX::MarketDepth(1));
        mdReq.set(FIX::MDUpdateType(FIX::MDUpdateType_INCREMENTAL_REFRESH));

        FIX44::MarketDataRequest::NoMDEntryTypes entryTypes;
        entryTypes.set(FIX::MDEntryType(FIX::MDEntryType_BID));
        mdReq.addGroup(entryTypes);
        entryTypes.set(FIX::MDEntryType(FIX::MDEntryType_OFFER));
        mdReq.addGroup(entryTypes);

        for (const auto &s : symbols) {
            FIX44::MarketDataRequest::NoRelatedSym group;
            group.set(FIX::Symbol(s));
            mdReq.addGroup(group);
        }

        try {
            FIX::Session::sendToTarget(mdReq, sessionID);
        } catch (...) {
            std::cerr << "Failed to send MarketDataRequest\n";
        }
    }

    void saveToSharedMemory(const std::string& symbol, double bid, double ask) {
        // Check if symbol exists
        for (int i = 0; i < shm->count; ++i) {
            if (symbol == shm->symbols[i]) {
                shm->prices[i].bid = bid;
                shm->prices[i].ask = ask;
                return;
            }
        }

        // Add new symbol
        if (shm->count < MAX_SYMBOLS) {
            strncpy(shm->symbols[shm->count], symbol.c_str(), sizeof(shm->symbols[0])-1);
            shm->symbols[shm->count][sizeof(shm->symbols[0])-1] = '\0';
            shm->prices[shm->count].bid = bid;
            shm->prices[shm->count].ask = ask;
            shm->count++;
        } else {
            std::cerr << "Shared memory full, cannot add symbol " << symbol << std::endl;
        }
    }
};

int main() {
    try {
        FIX::SessionSettings settings("initiator.cfg");
        MarketDataListener app;
        FIX::FileStoreFactory storeFactory(settings);
        FIX::FileLogFactory logFactory(settings);
        FIX::SocketInitiator initiator(app, storeFactory, settings, logFactory);

        initiator.start();
        std::cout << "FIX listener started...\n";
        while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
        initiator.stop();
    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
