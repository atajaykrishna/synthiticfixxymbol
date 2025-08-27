#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/Session.h"
#include "quickfix/ThreadedSocketAcceptor.h"
#include "quickfix/FileStore.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/ExecutionReport.h"
#include "quickfix/fix44/OrderCancelRequest.h"
#include "quickfix/fix44/OrderCancelReplaceRequest.h"

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
        std::cout << "Received raw message: " << message.toString() << std::endl;

        // Extract MsgType (tag 35)
        FIX::MsgType msgType;
        message.getHeader().getField(msgType);

        try {
            if (msgType == "D") {
                handleNewOrder(message, sessionID);
            } else if (msgType == "F") {
                handleCancel(message, sessionID);
            } else if (msgType == "H") {
                handleCancelReplace(message, sessionID);
            } else {
                std::cout << "Unsupported MsgType: " << msgType.getString() << std::endl;
            }
        } catch (std::exception& e) {
            std::cerr << "Error handling MsgType " << msgType.getString() << ": " << e.what() << std::endl;
        }
    }

private:
    void handleNewOrder(const FIX::Message& message, const FIX::SessionID& sessionID) {
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

        std::cout << "Parsed NewOrderSingle -> "
                  << "ClOrdID: " << clOrdID.getString() << ", "
                  << "Symbol: " << symbol.getString() << ", "
                  << "Side: "   << (side.getValue() == FIX::Side_BUY ? "BUY" : "SELL") << ", "
                  << "Qty: "    << qty.getString()
                  << std::endl;

        // Send ExecutionReport ACK
        FIX44::ExecutionReport exec(
            FIX::OrderID("ORDER_" + std::to_string(std::rand())),
            FIX::ExecID("EXEC_" + std::to_string(std::rand())),
            FIX::ExecType(FIX::ExecType_FILL),
            FIX::OrdStatus(FIX::OrdStatus_FILLED),
            side,
            FIX::LeavesQty(0),
            FIX::CumQty(qty),
            FIX::AvgPx(0)
        );

        exec.set(clOrdID);
        exec.set(symbol);
        exec.set(qty);
        exec.set(ordType);

        FIX::Session::sendToTarget(exec, sessionID);
        std::cout << "Sent ACK ExecutionReport (NewOrderSingle)\n";
    }

    void handleCancel(const FIX::Message& message, const FIX::SessionID& sessionID) {
        FIX44::OrderCancelRequest cancel(message);

        FIX::ClOrdID clOrdID;
        FIX::OrigClOrdID origClOrdID;
        FIX::Symbol symbol;

        if (cancel.isSetField(FIX::FIELD::ClOrdID)) cancel.get(clOrdID);
        if (cancel.isSetField(FIX::FIELD::OrigClOrdID)) cancel.get(origClOrdID);
        if (cancel.isSetField(FIX::FIELD::Symbol)) cancel.get(symbol);

        std::cout << "Parsed Cancel -> ClOrdID: " << clOrdID.getString()
                  << ", OrigClOrdID: " << origClOrdID.getString()
                  << ", Symbol: " << symbol.getString() << std::endl;

        // Send a cancel ExecutionReport
        FIX44::ExecutionReport exec(
            FIX::OrderID("CANCEL_" + std::to_string(std::rand())),
            FIX::ExecID("EXEC_" + std::to_string(std::rand())),
            FIX::ExecType(FIX::ExecType_CANCELED),
            FIX::OrdStatus(FIX::OrdStatus_CANCELED),
            FIX::Side(FIX::Side_BUY),
            FIX::LeavesQty(0),
            FIX::CumQty(0),
            FIX::AvgPx(0)
        );

        exec.set(clOrdID);
        exec.set(symbol);

        FIX::Session::sendToTarget(exec, sessionID);
        std::cout << "Sent Cancel ACK ExecutionReport\n";
    }

    void handleCancelReplace(const FIX::Message& message, const FIX::SessionID& sessionID) {
        FIX44::OrderCancelReplaceRequest replace(message);

        FIX::ClOrdID clOrdID;
        FIX::OrigClOrdID origClOrdID;
        FIX::Symbol symbol;
        FIX::OrderQty qty;

        if (replace.isSetField(FIX::FIELD::ClOrdID)) replace.get(clOrdID);
        if (replace.isSetField(FIX::FIELD::OrigClOrdID)) replace.get(origClOrdID);
        if (replace.isSetField(FIX::FIELD::Symbol)) replace.get(symbol);
        if (replace.isSetField(FIX::FIELD::OrderQty)) replace.get(qty);

        std::cout << "Parsed CancelReplace -> ClOrdID: " << clOrdID.getString()
                  << ", OrigClOrdID: " << origClOrdID.getString()
                  << ", Symbol: " << symbol.getString()
                  << ", New Qty: " << qty.getString() << std::endl;

        // Send replaced ExecutionReport
        FIX44::ExecutionReport exec(
            FIX::OrderID("REPLACED_" + std::to_string(std::rand())),
            FIX::ExecID("EXEC_" + std::to_string(std::rand())),
            FIX::ExecType(FIX::ExecType_REPLACE),
            FIX::OrdStatus(FIX::OrdStatus_REPLACED),
            FIX::Side(FIX::Side_BUY),
            FIX::LeavesQty(0),
            FIX::CumQty(qty),
            FIX::AvgPx(0)
        );

        exec.set(clOrdID);
        exec.set(symbol);
        exec.set(qty);

        FIX::Session::sendToTarget(exec, sessionID);
        std::cout << "Sent Replace ACK ExecutionReport\n";
    }
};

int main() {
    const char* cfgFile = "feedsender.cfg";

    std::ifstream f(cfgFile);
    if (!f.is_open()) {
        std::cerr << "Could not open config file: " << cfgFile << std::endl;
        return 1;
    }

    try {
        FIX::SessionSettings settings(cfgFile);
        TradeReceiverApp application;
        FIX::FileStoreFactory storeFactory(settings);

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
