SET(LINK_DEPS
	${CAF_LIBRARY_core}
	Qt5::Core
	xstudio::ui::qml::helper
	xstudio::ui::qml::playhead
	xstudio::ui::qml::playlist
	xstudio::media
	xstudio::utility
)

SET(EXTRAMOC
)

create_qml_component(timeline 0.1.0 "${LINK_DEPS}" "${EXTRAMOC}")
