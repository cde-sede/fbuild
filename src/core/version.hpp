#ifndef fb_core_version_hpp
#define fb_core_version_hpp

#include "ca/sv.hpp"

namespace fb {

	/* fbuild's version; fbuildd reports the same one. */
	inline constexpr ca::sv::str version = "1.0.0";

} // namespace fb

#endif // fb_core_version_hpp
