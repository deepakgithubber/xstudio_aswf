SET(LINK_DEPS
	xstudio::module
	xstudio::plugin_manager
	xstudio::ui::opengl::viewport
	Imath::Imath
)

find_package(Imath)

create_plugin_with_alias(viewport_hud xstudio::viewport::viewport_hud 0.1.0  "${LINK_DEPS}")

add_subdirectory(qml)