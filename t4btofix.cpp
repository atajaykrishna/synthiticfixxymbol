#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/Session.h"
#include "quickfix/ThreadedSocketAcceptor.h"
#include "quickfix/FileStore.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/ExecutionReport.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <cstdlib>

class TradeReceiverApp : public FIX::Application, public FIX::MessageCracker {
public:
    void onCreate(const FIX::SessionID& sessionID) override {
        std::cout << "Session created: " << sessionID << std::endl;
    }
    void onLogon(const FIX::SessionID& sessionID) override {
        std::cout << "Logon: " << sessionID << std::endl;
    }
    void onLogout(const FIX::SessionID& sessionID) override {
        std::cout << "Logout: " << sessionID << std::endl;
    }

    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&)
        throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override {}

    void toApp(FIX::Message&, const FIX::SessionID&)
        throw(FIX::DoNotSend) override {}

    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID)
        throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override 
    {
        // Always print the raw FIX string
        std::cout << "Received raw message: " << message.toString() << std::endl;

        // Try to parse NewOrderSingle (35=D)
        try {
            FIX44::NewOrderSingle order(message);

            FIX::ClOrdID clOrdID;
            FIX::Symbol symbol;
            FIX::Side side;
            FIX::OrderQty qty;
            FIX::OrdType ordType;

            if (order.isSetField(FIX::FIELD::ClOrdID)) order.get(clOrdID);
            if (order.isSetField(FIX::FIELD::Symbol)) order.get(symbol);
            if (order.isSetField(FIX::FIELD::Side)) order.get(side);
            if (order.isSetField(FIX::FIELD::OrderQty)) order.get(qty);
            if (order.isSetField(FIX::FIELD::OrdType)) order.get(ordType);

            std::cout << "Parsed Order -> "
                      << "ClOrdID: " << clOrdID.getString() << ", "
                      << "Symbol: " << symbol.getString() << ", "
                      << "Side: "   << (side.getValue() == FIX::Side_BUY ? "BUY" : "SELL") << ", "
                      << "Qty: "    << qty.getString()
                      << std::endl;

            // Build proper ExecutionReport
            FIX44::ExecutionReport exec(
                FIX::OrderID("ORDER_" + std::to_string(std::rand())),
                FIX::ExecID("EXEC_" + std::to_string(std::rand())),
                FIX::ExecType(FIX::ExecType_FILL),
                FIX::OrdStatus(FIX::OrdStatus_FILLED),
                side,           // mirror side from client
                FIX::LeavesQty(0),
                FIX::CumQty(qty),
                FIX::AvgPx(0)
            );

            exec.set(clOrdID);   // echo back client’s ClOrdID
            exec.set(symbol);    // echo back Symbol
            exec.set(qty);       // echo back Qty
            exec.set(ordType);   // echo back Order Type

            FIX::Session::sendToTarget(exec, sessionID);
            std::cout << "Sent ACK ExecutionReport\n";

        } catch (std::exception& e) {
            std::cerr << "Error parsing NewOrderSingle: " << e.what() << std::endl;
        }
    }
};

int main() {
    const char* cfgFile = "feedsender.cfg";  // config file

    // Check if file exists
    std::ifstream f(cfgFile);
    if (!f.is_open()) {
        std::cerr << "Could not open config file: " << cfgFile << std::endl;
        return 1;
    }

    try {
        FIX::SessionSettings settings(cfgFile);
        TradeReceiverApp application;
        FIX::FileStoreFactory storeFactory(settings);

        // Start FIX acceptor without ScreenLog
        FIX::ThreadedSocketAcceptor acceptor(application, storeFactory, settings);
        acceptor.start();
        std::cout << "FIX Acceptor started...\n";

        while (true)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        acceptor.stop();
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
