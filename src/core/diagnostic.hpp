#ifndef fb_core_diagnostic_hpp
#define fb_core_diagnostic_hpp

#include "ca/sv.hpp"

namespace fb {

	/* A place in a script, 1-based; line 0 when unknown. */
	struct position final {
		int line;
		int col;
	};

	/* Why a script was rejected or stopped, and where: the message lives in the script's
	   fb::memory. */
	struct diagnostic final {
		ca::sv::str message;
		fb::position at;
	};

} // namespace fb

#endif // fb_core_diagnostic_hpp
