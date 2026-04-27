xwayland_libs = []
xwayland_required = [
	'xcb',
	'xcb-composite',
	'xcb-ewmh',
	'xcb-icccm',
	'xcb-render',
	'xcb-res',
	'xcb-xfixes',
]
xwayland_optional = {
	'xcb-errors': 'Required for printing X11 errors.',
}

msg = []
if get_option('xwayland').enabled()
	msg += 'Install "@0@" or pass "-Dxwayland=disabled".'
endif
if not get_option('xwayland').disabled()
	msg += 'Required for Xwayland support.'
endif


# Android: The XWayland binary runs inside proot, not as a native binary.
# Skip the pkg-config check for the xwayland binary.  The path is overridden
# at runtime via the WLR_XWAYLAND environment variable (see server.c).
if get_option('xwayland').disabled()
	subdir_done()
endif
internal_config.set_quoted('XWAYLAND_PATH', '/usr/bin/Xwayland')
internal_config.set10('HAVE_XWAYLAND_LISTENFD', false)
internal_config.set10('HAVE_XWAYLAND_NO_TOUCH_POINTER_EMULATION', false)
internal_config.set10('HAVE_XWAYLAND_FORCE_XRANDR_EMULATION', false)
internal_config.set10('HAVE_XWAYLAND_TERMINATE_DELAY', false)

foreach lib : xwayland_required
	dep = dependency(lib,
		required: get_option('xwayland'),
		not_found_message: '\n'.join(msg).format(lib),
	)
	if not dep.found()
		subdir_done()
	endif

	xwayland_libs += dep
endforeach

foreach lib, desc : xwayland_optional
	msg = []
	if get_option(lib).enabled()
		msg += 'Install "@0@" or pass "-D@0@=disabled".'
	endif
	if not get_option(lib).disabled()
		msg += desc
	endif

	dep = dependency(lib,
		required: get_option(lib),
		not_found_message: '\n'.join(msg).format(lib),
	)

	internal_features += { lib: dep.found() }
	xwayland_libs += dep
endforeach


wlr_files += files(
	'selection/dnd.c',
	'selection/incoming.c',
	'selection/outgoing.c',
	'selection/selection.c',
	'server.c',
	'shell.c',
	'sockets.c',
	'xwayland.c',
	'xwm.c',
)
wlr_deps += xwayland_libs
features += { 'xwayland': true }

# Android: xcb is built from source (libxcb 1.17.0 has this function).
internal_config.set10('HAVE_XCB_XFIXES_SET_CLIENT_DISCONNECT_MODE', true)
