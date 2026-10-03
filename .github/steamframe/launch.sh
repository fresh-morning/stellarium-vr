#!/bin/sh
# Starts the bundled Stellarium, from the package folder or as the AppImage's
# AppRun. SteamOS and the Steam Linux Runtime have no Qt: the bundled one is
# under lib/ and plugins/. Stellarium logs to ~/.stellarium/log.txt.
here=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$here/plugins"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
# From an SSH shell there is no display; the headset's own Xwayland is usually :0.
export DISPLAY="${DISPLAY:-:0}"
# The OpenXR plugin binds the headset to a GLX context.
export QT_XCB_GL_INTEGRATION="${QT_XCB_GL_INTEGRATION:-xcb_glx}"
# The headset paces frames in xrWaitFrame; a vsynced window would block too (Mesa).
export vblank_mode="${vblank_mode:-0}"
exec "$here/bin/stellarium" "$@"
