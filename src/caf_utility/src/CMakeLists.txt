
find_package(fmt)

SET(LINK_DEPS
	xstudio::utility
	caf::io
	caf::core
	fmt::fmt
)

create_component(caf_utility 0.1.0 "${LINK_DEPS}")

