SET(LINK_DEPS
	xstudio::global_store
	caf::core
	xstudio::media
	#stdc++fs
	dl
)

create_component(media_hook 0.1.0 "${LINK_DEPS}")

