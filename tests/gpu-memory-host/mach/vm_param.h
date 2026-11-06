#pragma once
/* Deliberately model 16KiB XNU host pages, not Linux's usual 4KiB. */
static constexpr unsigned page_size=16384;
