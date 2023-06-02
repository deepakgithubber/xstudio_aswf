SET(LINK_DEPS
	${CAF_LIBRARY_core}
	Qt5::Core
	xstudio::ui::qml::helper
	xstudio::ui::qml::media
	xstudio::ui::qml::playhead
	xstudio::ui::qml::playlist
	xstudio::utility
	xstudio::playhead
	xstudio::playlist
)

SET(EXTRAMOC
)

create_qml_component(contact_sheet 0.1.0 "${LINK_DEPS}" "${EXTRAMOC}")
