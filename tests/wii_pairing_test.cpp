#include <windows.h>
#include <bluetoothapis.h>
#include <cwchar>
#include <cstdio>
static bool connected=true, authenticated=true, found=true, auth_failure=false;
static unsigned auth_calls=0,service_calls=0;
static wchar_t captured_pin[6]{};
static HBLUETOOTH_RADIO_FIND fake_first_radio(const BLUETOOTH_FIND_RADIO_PARAMS*,HANDLE* h){*h=reinterpret_cast<HANDLE>(1);return reinterpret_cast<HBLUETOOTH_RADIO_FIND>(1);}
static BOOL fake_next_radio(HBLUETOOTH_RADIO_FIND,HANDLE*){return FALSE;}
static BOOL fake_close_radio(HBLUETOOTH_RADIO_FIND){return TRUE;}
static BOOL fake_close(HANDLE){return TRUE;}
static DWORD fake_radio_info(HANDLE,BLUETOOTH_RADIO_INFO* i){for(int k=0;k<6;++k)i->address.rgBytes[k]=BYTE(k);return ERROR_SUCCESS;}
static HBLUETOOTH_DEVICE_FIND fake_first_device(const BLUETOOTH_DEVICE_SEARCH_PARAMS*,BLUETOOTH_DEVICE_INFO* d){
 if(!found){SetLastError(ERROR_NO_MORE_ITEMS);return nullptr;}
 std::wcscpy(d->szName,L"Nintendo RVL-CNT-01-TR");d->fConnected=connected;d->fAuthenticated=authenticated;
 return reinterpret_cast<HBLUETOOTH_DEVICE_FIND>(1);
}
static BOOL fake_next_device(HBLUETOOTH_DEVICE_FIND,BLUETOOTH_DEVICE_INFO*){return FALSE;}
static BOOL fake_close_device(HBLUETOOTH_DEVICE_FIND){return TRUE;}
static DWORD fake_auth(HWND,HANDLE,BLUETOOTH_DEVICE_INFO*,PWSTR p,ULONG n){++auth_calls;
 if(n==6)for(int k=0;k<6;++k)captured_pin[k]=p[k];return auth_failure?ERROR_GEN_FAILURE:ERROR_SUCCESS;}
static DWORD fake_services(HANDLE,BLUETOOTH_DEVICE_INFO*,DWORD*,GUID*){return ERROR_SUCCESS;}
static DWORD fake_service(HANDLE,BLUETOOTH_DEVICE_INFO*,const GUID*,DWORD){++service_calls;return connected?ERROR_GEN_FAILURE:ERROR_SUCCESS;}
#define BluetoothFindFirstRadio fake_first_radio
#define BluetoothFindNextRadio fake_next_radio
#define BluetoothFindRadioClose fake_close_radio
#define CloseHandle fake_close
#define BluetoothGetRadioInfo fake_radio_info
#define BluetoothFindFirstDevice fake_first_device
#define BluetoothFindNextDevice fake_next_device
#define BluetoothFindDeviceClose fake_close_device
#define BluetoothAuthenticateDevice fake_auth
#define BluetoothEnumerateInstalledServices fake_services
#define BluetoothSetServiceState fake_service
#include "input/wii_pairing.h"
int main(){
 unsigned checks=0,failures=0;auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}};
 using namespace nwii::runtime::input;
 wii_pairing_detail::scan();auto message=wii_pairing_status().message;
 check(auth_calls==0 && service_calls==0,"Windows-connected remote must not reauthenticate/re-enable HID");
 check(message.find("requested")!=std::string::npos && message.find("error 31")==std::string::npos,"already connected remote is not a false pairing failure");
 connected=false;authenticated=false;wii_pairing_detail::scan();
 check(auth_calls==1 && service_calls==1,"new SYNC pairing authenticates then enables HID");
 bool correct=true;for(int k=0;k<6;++k)correct=correct && captured_pin[k]==k;
 check(correct,"SYNC PIN preserves six binary radio address bytes including NUL");
 auth_failure=true;auto prior=service_calls;wii_pairing_detail::scan();
 check(service_calls==prior,"failed authentication cannot enable HID or claim connection success");
 check(wii_pairing_status().message.find("error 31")!=std::string::npos,"genuine Windows authentication failure keeps its error code");
 found=false;wii_pairing_detail::scan();
 check(wii_pairing_status().message.find("No Wii Remote found")!=std::string::npos,"no remote has a useful recovery message");
 std::printf("RESULT %u/%u\n",checks-failures,checks);return failures?1:0;
}
