#pragma once

#include <algorithm>

// English descriptions can wrap to more lines. Keep the Chinese page size
// unchanged and paginate earlier in English; slots remain indexed in source.
namespace tower::language {
inline int RowsPerPage(bool english, int nativeRows) { return english ? std::min(nativeRows, 5) : nativeRows; }
inline int Pages(int count, int pageSize) { return count > 0 ? (count + pageSize - 1) / pageSize : 1; }
inline int RowIndex(int page, int row, int pageSize) { return page * pageSize + row; }
}
