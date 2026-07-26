#include "NetworkStatus.hpp"

#include <switch.h>

namespace romm::model {

    bool IsNetworkConnected() {
        NifmInternetConnectionType type = (NifmInternetConnectionType)0;
        u32 wifi = 0;
        NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
        return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifi, &status)) &&
               status == NifmInternetConnectionStatus_Connected;
    }

}
