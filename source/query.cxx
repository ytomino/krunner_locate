#include "query.hxx"

#include <cassert>
#include <cerrno>
#include <cstring>

#include <alloca.h>
#include <fnmatch.h>
#include <sys/stat.h>

std::string_view image(file_type_filter_t x)
{
	switch(x){
	case ftf_all:
		return "all";
	case ftf_only_dir:
		return "only_dir";
	default:
		assert(false);
		return "";
	}
}

static bool has_uppercase(std::string_view pattern);

void parse_query(std::string_view pattern, query_t *result)
{
	result->locate_query.base_name = true;
	result->locate_query.ignore_case = true;
	result->absolute = false;
	result->file_type_filter = ftf_all;
	
	std::string_view::const_iterator begin = pattern.cbegin();
	std::string_view::const_iterator end = pattern.cend();
	if(pattern.starts_with('/')){
		++ begin;
		result->absolute = true;
	}
	if(pattern.ends_with('/')){
		-- end;
		result->file_type_filter = ftf_only_dir;
	}
	if(begin >= end){
		result->locate_query.pattern.clear();
	}else{
		if(has_uppercase(std::string_view(begin, end))){
			result->locate_query.ignore_case = false;
		}
		if(std::string_view(begin, end).find('/') != std::string_view::npos){
			result->locate_query.base_name = false;
			begin = pattern.cbegin();
		}
		
		result->locate_query.pattern.assign(begin, end);
	}
}

static bool do_fnmatch(
	std::size_t c_pattern_length,
	char *c_pattern, /* required c_pattern_length + 2 */
	char const *c_item, bool ignore_case, bool at_end
)
{
	if(c_pattern_length == 0){
		return false;
	}
	
	c_pattern[c_pattern_length] = '\0';
	
	int flags = FNM_PATHNAME;
	if(ignore_case) flags |= FNM_CASEFOLD;
	if(fnmatch(c_pattern, c_item, flags) == 0){
		return true;
	}
	if(! at_end && c_pattern[c_pattern_length - 1] != '*'){
		c_pattern[c_pattern_length] = '*';
		c_pattern[c_pattern_length + 1] = '\0';
		return fnmatch(c_pattern, c_item, flags) == 0;
	}
	return false;
}

static filtered_status_t do_stat(char const *c_item)
{
	struct stat statbuf;
	while(lstat(c_item, &statbuf) < 0){
		if(errno != EINTR) return fs_error;
	}
	switch(statbuf.st_mode & S_IFMT){
	case S_IFREG:
		return fs_regular;
	case S_IFDIR:
		return fs_directory;
	default:
		return fs_other;
	}
}

filtered_status_t filter_query(std::string_view item, query_t const *query)
{
	std::size_t item_length = item.size();
	char *c_item = static_cast<char *>(alloca(item_length + 1));
	std::memcpy(c_item, item.data(), item_length);
	c_item[item_length] = '\0';
	
	std::size_t pattern_length = query->locate_query.pattern.size();
	char *c_pattern = static_cast<char *>(alloca(pattern_length + 2));
	std::memcpy(c_pattern, query->locate_query.pattern.data(), pattern_length);
	
	bool only_dir = query->file_type_filter == ftf_only_dir;
		/* also means only matching at end */
	
	/* path */
	if(query->absolute){
		char const *begin = c_item;
		if(query->locate_query.base_name){
			if(*begin != '/'){
				return fs_error;
			}
			++ begin;
		}
		if(!
			do_fnmatch(
				pattern_length, c_pattern, begin, query->locate_query.ignore_case, only_dir
			)
		){
			return fs_error;
		}
	}else if(! query->locate_query.base_name || only_dir){
		bool matched = false;
		for(char const *i = c_item + item_length - 1; i >= c_item; -- i){
			if(query->locate_query.base_name && *i == '/'){
				break;
			}
			if(
				do_fnmatch(
					pattern_length, c_pattern, i, query->locate_query.ignore_case, only_dir
				)
			){
				matched = true;
				break;
			}
		}
		if(! matched){
			return fs_error;
		}
	}
	
	/* file type */
	filtered_status_t filtered_status = do_stat(c_item);
	if(only_dir && filtered_status != fs_directory){
		return fs_error;
	}
	return filtered_status;
}

/* ICU */
#include <unicode/uchar.h>
#include <unicode/uiter.h>

static bool has_uppercase(std::string_view pattern)
{
	UCharIterator iter;
	uiter_setUTF8(&iter, pattern.data(), pattern.size());
	while(iter.hasNext(&iter) != 0){
		UChar32 codepoint = iter.next(&iter);
		if(u_isupper(codepoint)){
			return true;
		}
	}
	return false;
}
