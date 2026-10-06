#pragma once

#include <libheif/heif.h>

// Implemented by the pinned libheif adapter patch; derived tmap items have no image handle.
extern "C" int awjHeifItemNclx(const heif_context*, heif_item_id, int*, int*, int*);
