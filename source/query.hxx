#ifndef QUERY_HXX
#define QUERY_HXX

#include <compare>
#include <string>
#include <string_view>

struct locate_query_t {
	std::string pattern;
	bool base_name;
	bool ignore_case;
	
	friend std::strong_ordering operator <=> (
		locate_query_t const &left, locate_query_t const &right
	) = default;
};

enum file_type_filter_t {ftf_all, ftf_only_dir};

std::string_view image(file_type_filter_t x);

struct query_t {
	locate_query_t locate_query;
	bool absolute;
	file_type_filter_t file_type_filter;
	
	query_t() = default;
	query_t(query_t &&) = default;
	
	friend std::strong_ordering operator <=> (
		query_t const &left, query_t const &right
	) = default;
};

void parse_query(std::string_view pattern, query_t *result);

bool filter_query(std::string_view item, query_t const *query);
bool refilter_query(std::string_view item, query_t const *query);

#endif
