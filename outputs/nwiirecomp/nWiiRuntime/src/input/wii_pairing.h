#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bluetoothapis.h>
#pragma comment(lib,"Bthprops.lib")
#endif

namespace nwii::runtime::input {
struct WiiPairingStatus { bool busy=false; std::string message; };
namespace wii_pairing_detail {
struct Store { std::atomic<bool> busy{false}; std::mutex mutex; std::string message; };
inline Store& store() { static auto* s=new Store; return *s; }
inline void message(std::string text) { auto& s=store();std::lock_guard lock(s.mutex);s.message=std::move(text); }
inline void scan() {
#ifdef _WIN32
    BLUETOOTH_FIND_RADIO_PARAMS rp{sizeof(rp)};
    HANDLE radio=nullptr;
    auto radios=BluetoothFindFirstRadio(&rp,&radio);
    if(!radios) { message("No Windows Bluetooth radio found. Turn Bluetooth on and try again.");return; }
    unsigned found=0,ready=0;DWORD last_error=0;
    do {
        BLUETOOTH_RADIO_INFO info{sizeof(info)};
        if(BluetoothGetRadioInfo(radio,&info)==ERROR_SUCCESS) {
            BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
            sp.dwSize=sizeof(sp);sp.hRadio=radio;
            sp.fReturnAuthenticated=sp.fReturnRemembered=sp.fReturnUnknown=sp.fReturnConnected=TRUE;
            sp.fIssueInquiry=TRUE;sp.cTimeoutMultiplier=4;
            BLUETOOTH_DEVICE_INFO device{sizeof(device)};
            auto devices=BluetoothFindFirstDevice(&sp,&device);
            if(devices) {
                do {
                    if(std::wstring(device.szName)!=L"Nintendo RVL-CNT-01-TR" &&
                       std::wstring(device.szName)!=L"Nintendo RVL-CNT-01") continue;
                    ++found;
                    DWORD error=ERROR_SUCCESS;
                    if(!device.fAuthenticated && !device.fConnected) {
                        // SYNC uses the host radio's six address bytes as the PIN.
                        // Binary bytes must be passed with an explicit length, including NULs.
                        wchar_t pin[6];for(int i=0;i<6;++i)pin[i]=info.address.rgBytes[i];
#pragma warning(push)
#pragma warning(disable:4995 4996)
                        error=BluetoothAuthenticateDevice(nullptr,radio,&device,pin,6);
#pragma warning(pop)
                        if(error==ERROR_SUCCESS) {
                            DWORD count=0;
                            error=BluetoothEnumerateInstalledServices(radio,&device,&count,nullptr);
                            if(error==ERROR_MORE_DATA)error=ERROR_SUCCESS;
                        }
                    }
                    if(error==ERROR_SUCCESS) {
                        static constexpr GUID hid{0x00001124,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
                        error=BluetoothSetServiceState(radio,&device,&hid,BLUETOOTH_SERVICE_ENABLE);
                        if(error==ERROR_SUCCESS || (error==ERROR_SERVICE_ALREADY_RUNNING && device.fConnected))++ready;
                    }
                    if(error!=ERROR_SUCCESS)last_error=error;
                } while(BluetoothFindNextDevice(devices,&device));
                BluetoothFindDeviceClose(devices);
            } else if(GetLastError()!=ERROR_NO_MORE_ITEMS)last_error=GetLastError();
        }
        CloseHandle(radio);
    } while(BluetoothFindNextRadio(radios,&radio));
    BluetoothFindRadioClose(radios);
    if(ready)message("Wii Remote HID connection requested. Wait for SkyBreak to show the controller below; pairing alone does not confirm sensor input.");
    else if(found)message("Wii Remote found, but Windows could not connect it (error "+std::to_string(last_error)+"). Press the red SYNC button and try again. Close other apps using the remote.");
    else message("No Wii Remote found. Attach the Nunchuk and press the red SYNC button, then try again.");
#else
    message("Pair the Wii Remote in your system's Bluetooth manager, then restart SkyBreak.");
#endif
}
}
inline WiiPairingStatus wii_pairing_status() {
    auto& s=wii_pairing_detail::store();std::lock_guard lock(s.mutex);
    return {s.busy.load(),s.message};
}
inline void start_wii_pairing() {
    auto& s=wii_pairing_detail::store();bool idle=false;
    if(!s.busy.compare_exchange_strong(idle,true))return;
    wii_pairing_detail::message("Searching for Wii Remotes. Keep the red SYNC pairing LEDs flashing...");
    try {
        std::thread([]{
            try { wii_pairing_detail::scan(); }
            catch(...) { wii_pairing_detail::message("Bluetooth pairing failed. Try again."); }
            wii_pairing_detail::store().busy.store(false);
        }).detach(); // Store outlives the worker; Bluetooth inquiry never blocks the UI.
    } catch(...) {
        wii_pairing_detail::message("Could not start Bluetooth pairing.");s.busy.store(false);
    }
}
} // namespace nwii::runtime::input
