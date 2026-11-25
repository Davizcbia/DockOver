#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Security.Credentials.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
using namespace winrt::Windows::Devices::WiFiDirect;

int main() {
    winrt::init_apartment();
    try {
        WiFiDirectAdvertisementPublisher publisher;
        const auto advertisement = publisher.Advertisement();
        advertisement.IsAutonomousGroupOwnerEnabled(true);
        const auto legacy = advertisement.LegacySettings();
        legacy.IsEnabled(true);
        publisher.Start();
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        auto status = publisher.Status();
        while (status != WiFiDirectAdvertisementPublisherStatus::Started &&
               status != WiFiDirectAdvertisementPublisherStatus::Aborted &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(100ms);
            status = publisher.Status();
        }
        if (status != WiFiDirectAdvertisementPublisherStatus::Started) {
            publisher.Stop();
            std::cerr << "Windows could not start Wi-Fi Direct. Check the adapter and turn off Mobile Hotspot.\n";
            return 1;
        }
        std::cout << winrt::to_string(legacy.Ssid()) << '\n'
                  << winrt::to_string(legacy.Passphrase().Password()) << std::endl;
        // EOF also closes the group when the browser host process exits.
        std::cin.get();
        publisher.Stop();
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "Wi-Fi Direct: " << winrt::to_string(error.message()) << '\n';
        return 1;
    }
}
