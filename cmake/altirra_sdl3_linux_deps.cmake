# Linux prerequisite validation for a bundled SDL3 build.
#
# SDL 3.4 deliberately fails configuration when an X11 integration is enabled
# but its development headers are absent. Check the complete set up front so
# users get one actionable Altirra error instead of fixing dependencies one at
# a time as SDL reaches each feature probe.

function(altirra_validate_sdl3_linux_dependencies)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()

    # An explicitly Wayland-only or console-only SDL build does not need X11.
    if(DEFINED SDL_X11 AND NOT SDL_X11)
        message(STATUS
            "SDL3 Linux prerequisite check: X11 explicitly disabled "
            "(-DSDL_X11=OFF)")
        return()
    endif()

    find_package(X11 QUIET)

    set(_missing_features)
    set(_missing_deb_packages)
    set(_missing_rpm_packages)
    set(_missing_arch_packages)

    macro(_altirra_sdl3_require_x11_feature option found_var label deb rpm arch)
        set(_feature_enabled TRUE)
        if(DEFINED ${option} AND NOT ${option})
            set(_feature_enabled FALSE)
        endif()

        if(_feature_enabled AND NOT ${found_var})
            list(APPEND _missing_features
                "  ${label} (${option}, missing ${found_var})")
            list(APPEND _missing_deb_packages "${deb}")
            list(APPEND _missing_rpm_packages "${rpm}")
            list(APPEND _missing_arch_packages "${arch}")
        endif()
    endmacro()

    # Xext supplies Xdbe, XShape, and XSync. Xfixes also needs XInput2,
    # which is supplied by Xi, so validate both dependencies for that feature.
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XCURSOR X11_Xcursor_FOUND "Xcursor"
        libxcursor-dev libXcursor-devel libxcursor)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XDBE X11_Xext_FOUND "X11 double-buffer extension"
        libxext-dev libXext-devel libxext)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XINPUT X11_Xi_FOUND "XInput2"
        libxi-dev libXi-devel libxi)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XFIXES X11_Xfixes_FOUND "XFixes"
        libxfixes-dev libXfixes-devel libxfixes)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XFIXES X11_Xi_FOUND "XInput2 required by XFixes"
        libxi-dev libXi-devel libxi)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XSYNC X11_Xext_FOUND "XSync"
        libxext-dev libXext-devel libxext)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XRANDR X11_Xrandr_FOUND "Xrandr"
        libxrandr-dev libXrandr-devel libxrandr)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XSCRNSAVER X11_Xss_FOUND "XScreenSaver"
        libxss-dev libXScrnSaver-devel libxss)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XSHAPE X11_Xext_FOUND "XShape"
        libxext-dev libXext-devel libxext)
    _altirra_sdl3_require_x11_feature(
        SDL_X11_XTEST X11_Xtst_FOUND "XTest"
        libxtst-dev libXtst-devel libxtst)

    # FindX11 can report individual extension results even when the core X11
    # headers are absent, so keep the base dependency explicit too.
    if(NOT X11_FOUND)
        list(PREPEND _missing_features
            "  X11 video backend (SDL_X11, missing X11_FOUND)")
        list(PREPEND _missing_deb_packages libx11-dev)
        list(PREPEND _missing_rpm_packages libX11-devel)
        list(PREPEND _missing_arch_packages libx11)
    endif()

    if(_missing_features)
        list(REMOVE_DUPLICATES _missing_deb_packages)
        list(REMOVE_DUPLICATES _missing_rpm_packages)
        list(REMOVE_DUPLICATES _missing_arch_packages)
        list(JOIN _missing_features "\n" _missing_feature_text)
        list(JOIN _missing_deb_packages " " _missing_deb_text)
        list(JOIN _missing_rpm_packages " " _missing_rpm_text)
        list(JOIN _missing_arch_packages " " _missing_arch_text)

        message(FATAL_ERROR
            "Bundled SDL3 cannot enable all requested Linux X11 features.\n"
            "Missing development dependencies:\n"
            "${_missing_feature_text}\n\n"
            "Install the missing packages, then configure again:\n"
            "  Debian/Ubuntu: sudo apt install ${_missing_deb_text}\n"
            "  Fedora/RHEL:   sudo dnf install ${_missing_rpm_text}\n"
            "  Arch Linux:    sudo pacman -S ${_missing_arch_text}\n\n"
            "Altirra keeps SDL's desktop functionality enabled by default and "
            "does not silently compile features out. For an intentional "
            "Wayland-only build, pass -DSDL_X11=OFF. To omit only a specific "
            "feature, pass the corresponding -D<option>=OFF shown above.\n"
            "Alternatively, use an installed SDL >= 3.4 with "
            "-DALTIRRA_STATIC_SDL3=OFF and SDL3_DIR/CMAKE_PREFIX_PATH.")
    endif()

    message(STATUS
        "SDL3 Linux prerequisite check: all enabled X11 development "
        "dependencies found")
endfunction()
