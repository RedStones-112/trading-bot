#include "../src/strategy.hpp"
#include "../src/mock_broker.hpp"
#include <cassert>
#include <iostream>

int main() {
    // Flat history, then a sharp rise: short SMA should cross above long SMA -> Buy.
    std::vector<double> closes(20, 100.0);
    closes.push_back(150.0); // 21st point, pushes short SMA(5) above long SMA(20)
    assert(smaCrossSignal(closes, 5, 20) == Signal::Buy);

    // Flat history, then a sharp drop -> short SMA crosses below long SMA -> Sell.
    std::vector<double> closes2(20, 100.0);
    closes2.push_back(50.0);
    assert(smaCrossSignal(closes2, 5, 20) == Signal::Sell);

    // No cross: still flat -> Hold.
    std::vector<double> closes3(21, 100.0);
    assert(smaCrossSignal(closes3, 5, 20) == Signal::Hold);

    // Not enough history -> throws.
    bool threw = false;
    try {
        smaCrossSignal(std::vector<double>{1.0, 2.0}, 5, 20);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // MockBroker: never touches the network, returns sane values.
    MockBroker broker(70000.0, /*seed=*/42);
    double price = broker.getCurrentPrice("005930");
    assert(price > 0);
    auto history = broker.getDailyCloses("005930", 10);
    assert(history.size() == 10);
    auto orderId = broker.placeMarketOrder("005930", IBroker::Side::Buy, 1);
    assert(!orderId.empty());

    std::cout << "all tests passed\n";
    return 0;
}
