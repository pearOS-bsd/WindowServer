# sourced by login shells: when a WindowServer session is running, identify it like the Mac's (Aqua / Quartz Compositor)
if [ -n "$WS_SOCKET" ]; then export XDG_CURRENT_DESKTOP=Aqua XDG_SESSION_DESKTOP=Aqua XDG_SESSION_TYPE=WindowServer; fi
