SET(LINK_DEPS
	${CAF_LIBRARY_core}
	Qt5::Core
	xstudio::ui::qml::helper
	xstudio::ui::qml::playlist
	xstudio::utility
	xstudio::global_store
	xstudio::playlist
)

SET(EXTRAMOC
)

create_qml_component(session 0.1.0 "${LINK_DEPS}" "${EXTRAMOC}")
