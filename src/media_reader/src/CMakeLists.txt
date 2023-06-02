find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat swscale avutil)
SET(LINK_DEPS
	xstudio::media
	xstudio::media_cache
	xstudio::global_store
	xstudio::broadcast
	FFMPEG::avcodec
	FFMPEG::avformat
	FFMPEG::swscale
	FFMPEG::avutil
	
	caf::core
	#stdc++fs
	dl
)

create_component(media_reader 0.1.0 "${LINK_DEPS}")

