SET(LINK_DEPS
	${CAF_LIBRARY_core}
	Qt5::Core
	xstudio::ui::qml::helper
	xstudio::utility
)

SET(EXTRAMOC
)

create_qml_component(json_tree_model 0.1.0 "${LINK_DEPS}" "${EXTRAMOC}")
