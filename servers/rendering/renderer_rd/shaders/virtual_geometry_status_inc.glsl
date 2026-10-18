#ifndef VIRTUAL_GEOMETRY_STATUS_INCLUDED
#define VIRTUAL_GEOMETRY_STATUS_INCLUDED
layout(set = 0, binding = 15, std430) buffer ViewStatus {
	uint data[];
}
vg_status;
#endif
