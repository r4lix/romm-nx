#pragma once

namespace romm::model {

    // True when nifm reports a usable internet connection.
    //
    // This is a single service call with no retry or sleep in it, so it is safe
    // to poll from the render thread — unlike waiting for the network to come
    // up, which must never happen before the renderer exists or the user just
    // sees a black screen. Requires nifm to have been initialized (main.cpp);
    // returns false if it wasn't.
    bool IsNetworkConnected();

}
