SET(LINK_DEPS
	${CAF_LIBRARY_core}
	Qt5::Core
	xstudio::ui::qml::media
	xstudio::ui::qml::helper
	xstudio::ui::qml::playhead
	xstudio::ui::qml::playlist
	xstudio::utility
	xstudio::media
	xstudio::playlist
	
)

SET(EXTRAMOC
)

create_qml_component(subset 0.1.0 "${LINK_DEPS}" "${EXTRAMOC}")
