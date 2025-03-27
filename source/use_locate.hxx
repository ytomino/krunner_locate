#ifndef USE_LOCATE_HXX
#define USE_LOCATE_HXX

#include <functional>
#include <string_view>

#define EUNKNOWNERROR 0x10001
#define ELOCATE_FAILURE 0x10002

inline int nonzero_errno(int error)
{
	return (error == 0) ? EUNKNOWNERROR : error;
}

int locate(
	std::string_view pattern, bool base_name, bool ignore_case,
	std::function<int (std::string_view)> f,
	int *status /* when the return value is ELOCATE_FAILURE */
);

#endif
